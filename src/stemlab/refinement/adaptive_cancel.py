"""Constrained spectral matching and subtraction for one leakage event."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import signal
from scipy.ndimage import uniform_filter1d


@dataclass
class CancelConfig:
    """Safety limits and FFT settings for adaptive spectral cancellation."""

    n_fft: int = 2048
    hop_length: int = 256
    max_alignment_ms: float = 12.0
    max_gain: float = 0.85
    min_gain: float = 0.0
    regularization: float = 1e-3
    frequency_smooth_bins: int = 15
    confidence_threshold: float = 0.55
    subtraction_strength: float = 0.50


@dataclass
class CancelResult:
    """Cleaned audio and the match confidence that produced it."""

    cleaned: np.ndarray
    confidence: float


def _mono(x: np.ndarray) -> np.ndarray:
    return x.mean(axis=0) if x.ndim == 2 else x


def _best_alignment(
    reference: np.ndarray,
    target: np.ndarray,
    sr: int,
    max_ms: float,
) -> int:
    r = _mono(reference)
    y = _mono(target)

    max_shift = max(1, int(sr * max_ms / 1000.0))
    corr = signal.correlate(y, r, mode="full", method="fft")
    lags = signal.correlation_lags(len(y), len(r), mode="full")

    valid = (lags >= -max_shift) & (lags <= max_shift)
    if not np.any(valid):
        return 0

    best = int(lags[valid][np.argmax(np.abs(corr[valid]))])
    return best


def _shift(x: np.ndarray, samples: int) -> np.ndarray:
    out = np.zeros_like(x)
    if samples == 0:
        return x.copy()

    if samples > 0:
        if samples < x.shape[-1]:
            out[..., samples:] = x[..., :-samples]
    else:
        amount = -samples
        if amount < x.shape[-1]:
            out[..., :-amount] = x[..., amount:]
    return out


def _frame_parameters(length: int, cfg: CancelConfig) -> tuple[int, int]:
    """Frame size and overlap that ``length`` samples can actually support.

    The analysed region is a fixed number of milliseconds, so at low sample
    rates it is shorter than the configured overlap - scipy then shrinks
    nperseg to the region but keeps noverlap and refuses the call.
    """
    nperseg = min(cfg.n_fft, max(1, int(length)))
    noverlap = min(cfg.n_fft - cfg.hop_length, max(0, nperseg - 1))
    return nperseg, noverlap


def _stft_multichannel(x: np.ndarray, sr: int, cfg: CancelConfig) -> np.ndarray:
    nperseg, noverlap = _frame_parameters(x.shape[-1], cfg)

    specs = []
    for ch in range(x.shape[0]):
        _, _, z = signal.stft(
            x[ch],
            fs=sr,
            window="hann",
            nperseg=nperseg,
            noverlap=noverlap,
            nfft=nperseg,
            boundary="zeros",
            padded=True,
        )
        specs.append(z)
    return np.stack(specs, axis=0)


def _istft_multichannel(
    spec: np.ndarray,
    sr: int,
    cfg: CancelConfig,
    length: int,
):
    nperseg, noverlap = _frame_parameters(length, cfg)

    channels = []
    for ch in range(spec.shape[0]):
        _, x = signal.istft(
            spec[ch],
            fs=sr,
            window="hann",
            nperseg=nperseg,
            noverlap=noverlap,
            nfft=nperseg,
            input_onesided=True,
            boundary=True,
        )
        if len(x) < length:
            x = np.pad(x, (0, length - len(x)))
        channels.append(x[:length])
    return np.stack(channels, axis=0).astype(np.float32)


def _smooth_frequency(h: np.ndarray, bins: int) -> np.ndarray:
    """Boxcar-average ``[channels, freq, frames]`` along the frequency axis.

    Equivalent to convolving every frame with ``ones(bins) / bins`` in
    "same" mode: the kernel is a normalized mean, and ``mode="constant"``
    with ``cval=0`` reproduces np.convolve's zero padding at the spectrum
    edges. Filtering runs in float64 - the running-sum filter would drift
    in float32 - and the result keeps ``h``'s complex dtype.
    """
    if bins <= 1:
        return h

    if bins % 2 == 0:
        bins += 1

    real = uniform_filter1d(
        h.real.astype(np.float64),
        size=bins,
        axis=1,
        mode="constant",
        cval=0.0,
    )
    imag = uniform_filter1d(
        h.imag.astype(np.float64),
        size=bins,
        axis=1,
        mode="constant",
        cval=0.0,
    )
    return (real + 1j * imag).astype(h.dtype)


def _spectral_similarity(a: np.ndarray, b: np.ndarray) -> float:
    """Cosine similarity of log magnitudes, mapped to [0,1]."""
    am = np.log1p(np.abs(a)).reshape(-1)
    bm = np.log1p(np.abs(b)).reshape(-1)

    denom = np.linalg.norm(am) * np.linalg.norm(bm) + 1e-12
    sim = float(np.dot(am, bm) / denom)
    return float(np.clip(sim, 0.0, 1.0))


def adaptive_cancel(
    reference: np.ndarray,
    target: np.ndarray,
    sr: int,
    cfg: CancelConfig | None = None,
) -> CancelResult:
    """Match a source reference to leakage in target, then subtract it.

    `reference` and `target` are [channels, samples] and should have the same
    number of samples.

    The transfer function is a regularized complex least-squares estimate:

        H = Y * conj(R) / (|R|^2 + lambda)

    but it is constrained so it cannot freely synthesize arbitrary target
    content. Gain is clamped and the transfer function is smoothed in
    frequency. A similarity score gates final subtraction.
    """
    cfg = cfg or CancelConfig()

    if reference.shape != target.shape:
        raise ValueError(
            f"reference and target must have same shape; got {reference.shape} vs {target.shape}"
        )

    alignment = _best_alignment(
        reference,
        target,
        sr=sr,
        max_ms=cfg.max_alignment_ms,
    )
    aligned = _shift(reference, alignment)

    r = _stft_multichannel(aligned, sr, cfg)
    y = _stft_multichannel(target, sr, cfg)

    # Similarity before fitting is intentionally part of confidence. If the
    # target does not resemble the reference at all, do almost nothing.
    confidence = _spectral_similarity(r, y)

    denom = np.abs(r) ** 2 + cfg.regularization
    h = y * np.conj(r) / denom

    h = _smooth_frequency(h, cfg.frequency_smooth_bins)

    # Constrain gain while preserving phase.
    magnitude = np.abs(h)
    phase = np.exp(1j * np.angle(h))
    magnitude = np.clip(magnitude, cfg.min_gain, cfg.max_gain)
    h = magnitude * phase

    matched_spec = h * r

    # Extra soft cap: never let the modeled leakage exceed target magnitude by
    # a large amount in an individual time-frequency cell.
    y_mag = np.abs(y)
    m_mag = np.abs(matched_spec)
    cap = np.minimum(
        1.0,
        (y_mag * 0.85 + 1e-8) / (m_mag + 1e-8),
    )
    matched_spec *= cap

    matched = _istft_multichannel(
        matched_spec,
        sr=sr,
        cfg=cfg,
        length=target.shape[-1],
    )

    if confidence < cfg.confidence_threshold:
        strength = 0.0
    else:
        normalized = (confidence - cfg.confidence_threshold) / max(
            1e-6, 1.0 - cfg.confidence_threshold
        )
        strength = cfg.subtraction_strength * np.clip(normalized, 0.0, 1.0)

    cleaned = target - matched * float(strength)

    return CancelResult(
        cleaned=cleaned.astype(np.float32),
        confidence=confidence,
    )
