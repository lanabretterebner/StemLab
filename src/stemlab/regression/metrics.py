"""Numerical comparison of separated stems.

Moving separation off PyTorch turns "does it still sound the same?" into the
only question that matters, and the failure modes are quiet ones. A stem comes
back digitally silent because a mask head was mis-wired. An f16 accumulator
overflows and poisons a handful of frames with NaN. A band edge shifts and the
result is plausible but wrong. None of those raise, and none of them show up in
an exit code, so everything here exists to turn them into a number.

Audio is handled as float arrays shaped (channels, samples); one-dimensional
input is treated as mono and (samples, channels) input is transposed, since a
real signal always has far more samples than channels.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, field

import numpy as np
from scipy.signal import stft

# Below this peak a stem is silence rather than quiet audio. Anything a
# separator legitimately emits sits far above it; a dead mask head sits at
# exactly 0.0.
SILENCE_PEAK = 1e-6

# Guards the logarithms. Small enough not to colour a real comparison, large
# enough that an all-zero stem yields a finite number instead of -inf.
EPS = 1e-12


def as_channels_first(audio: np.ndarray) -> np.ndarray:
    """Normalise any sane audio layout to (channels, samples) float64."""
    array = np.asarray(audio, dtype=np.float64)

    if array.ndim == 1:
        return array[np.newaxis, :]

    if array.ndim != 2:
        raise ValueError(f"Expected mono or 2-D audio, got shape {array.shape}")

    # A channel count above 8 is not a channel count. Anything ambiguous is
    # already square, where the layout does not change the result.
    if array.shape[0] > array.shape[1] and array.shape[1] <= 8:
        return array.T

    return array


def _common_length(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """Trim both signals to their shared span, reporting what was dropped."""
    channels = min(a.shape[0], b.shape[0])
    samples = min(a.shape[1], b.shape[1])
    dropped = max(a.shape[1], b.shape[1]) - samples
    return a[:channels, :samples], b[:channels, :samples], dropped


def si_sdr(estimate: np.ndarray, reference: np.ndarray) -> float:
    """Scale-invariant SDR in dB, the standard separation-quality measure.

    Scale invariance matters here because a runtime that applies a different
    output gain is not making a separation error, and a plain SNR would score
    it as one.
    """
    est = as_channels_first(estimate).reshape(-1)
    ref = as_channels_first(reference).reshape(-1)
    length = min(est.size, ref.size)
    est, ref = est[:length], ref[:length]

    ref_energy = float(np.dot(ref, ref))

    if ref_energy <= EPS:
        # A silent reference has no signal to be distorted; only an equally
        # silent estimate agrees with it.
        return math.inf if float(np.dot(est, est)) <= EPS else -math.inf

    scale = float(np.dot(est, ref)) / ref_energy
    projection = scale * ref
    projection_energy = float(np.dot(projection, projection))
    noise = est - projection
    noise_energy = float(np.dot(noise, noise))

    if projection_energy <= EPS:
        # None of the reference survives in the estimate. A silent stem lands
        # here, and it must score worst rather than dividing 0 by 0 and
        # reporting a perfect result.
        return -math.inf

    if noise_energy <= EPS:
        return math.inf

    return 10.0 * math.log10(projection_energy / noise_energy)


def correlation(estimate: np.ndarray, reference: np.ndarray) -> float:
    """Pearson correlation over all channels, the primary agreement gate."""
    est = as_channels_first(estimate).reshape(-1)
    ref = as_channels_first(reference).reshape(-1)
    length = min(est.size, ref.size)
    est, ref = est[:length], ref[:length]

    est = est - est.mean()
    ref = ref - ref.mean()

    denominator = math.sqrt(float(np.dot(est, est)) * float(np.dot(ref, ref)))

    if denominator <= EPS:
        # Two silences agree perfectly; silence against signal does not.
        both_silent = float(np.abs(est).max(initial=0.0)) <= EPS
        both_silent = both_silent and float(np.abs(ref).max(initial=0.0)) <= EPS
        return 1.0 if both_silent else 0.0

    return float(np.dot(est, ref)) / denominator


def log_spectral_distance(
    estimate: np.ndarray,
    reference: np.ndarray,
    sample_rate: int,
    n_fft: int = 2048,
    hop: int = 512,
) -> float:
    """RMS difference of log-magnitude spectra, in dB.

    Correlation is dominated by the loud parts of a signal. This catches a
    stem that tracks the reference overall while getting its quiet spectral
    detail - reverb tails, cymbal wash - measurably wrong.
    """
    est, ref, _ = _common_length(as_channels_first(estimate), as_channels_first(reference))

    if est.shape[1] < 2:
        return 0.0

    nperseg = min(n_fft, est.shape[1])
    noverlap = min(n_fft - hop, nperseg - 1)

    distances = []

    for channel in range(est.shape[0]):
        _, _, ze = stft(est[channel], fs=sample_rate, nperseg=nperseg, noverlap=noverlap)
        _, _, zr = stft(ref[channel], fs=sample_rate, nperseg=nperseg, noverlap=noverlap)

        log_est = 20.0 * np.log10(np.abs(ze) + EPS)
        log_ref = 20.0 * np.log10(np.abs(zr) + EPS)
        distances.append(float(np.sqrt(np.mean((log_est - log_ref) ** 2))))

    return float(np.mean(distances)) if distances else 0.0


def best_lag(estimate: np.ndarray, reference: np.ndarray, max_lag: int = 4096) -> int:
    """Sample offset that best aligns the two signals, within +/- max_lag.

    A non-zero lag is the signature of a padding or centring difference in an
    STFT - exactly the kind of thing an independent reimplementation gets
    wrong - and it would otherwise show up as a mysteriously poor correlation.
    """
    est = as_channels_first(estimate)[0]
    ref = as_channels_first(reference)[0]
    length = min(est.size, ref.size)
    est, ref = est[:length], ref[:length]

    if length == 0:
        return 0

    # Silence has no alignment. Reporting the argmax over a flat correlation
    # would add a spurious "STFT centring" diagnosis on top of the real one.
    if float(np.abs(est).max(initial=0.0)) <= SILENCE_PEAK:
        return 0

    if float(np.abs(ref).max(initial=0.0)) <= SILENCE_PEAK:
        return 0

    # Cross-correlate a bounded window rather than the whole track: the offset
    # is a constant, so a few seconds settle it far more cheaply.
    window = min(length, 1 << 19)
    est, ref = est[:window], ref[:window]
    limit = min(max_lag, window - 1)

    if limit <= 0:
        return 0

    size = 1 << int(math.ceil(math.log2(2 * window)))
    spectrum = np.fft.rfft(est, size) * np.conj(np.fft.rfft(ref, size))
    correlations = np.fft.irfft(spectrum, size)

    # Lags [-limit, +limit], with negative lags living at the tail.
    candidates = np.concatenate((correlations[-limit:], correlations[: limit + 1]))
    return int(np.argmax(candidates)) - limit


@dataclass
class StemMetrics:
    """Everything one stem comparison produced, plus why it passed or failed."""

    stem: str
    correlation: float
    si_sdr_db: float
    log_spectral_distance_db: float
    lag_samples: int
    peak_candidate: float
    peak_reference: float
    rms_candidate: float
    rms_reference: float
    samples_compared: int
    samples_dropped: int
    has_nan: bool
    has_inf: bool
    candidate_silent: bool
    reference_silent: bool
    failures: list[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not self.failures

    def to_dict(self) -> dict:
        data = asdict(self)
        data["passed"] = self.passed
        return data


def compare_stem(
    stem: str,
    candidate: np.ndarray,
    reference: np.ndarray,
    sample_rate: int,
    min_correlation: float = 0.99,
    min_si_sdr_db: float = 20.0,
    max_lag_samples: int = 0,
) -> StemMetrics:
    """Score one candidate stem against its reference and apply the gates."""
    cand = as_channels_first(candidate)
    ref = as_channels_first(reference)
    cand_trimmed, ref_trimmed, dropped = _common_length(cand, ref)

    has_nan = bool(np.isnan(cand).any())
    has_inf = bool(np.isinf(cand).any())

    # Metrics on poisoned samples are meaningless, so score the finite part and
    # let the NaN/Inf flags carry the failure.
    scored = np.nan_to_num(cand_trimmed, nan=0.0, posinf=0.0, neginf=0.0)

    peak_candidate = float(np.abs(scored).max(initial=0.0))
    peak_reference = float(np.abs(ref_trimmed).max(initial=0.0))
    rms_candidate = float(np.sqrt(np.mean(scored**2))) if scored.size else 0.0
    rms_reference = float(np.sqrt(np.mean(ref_trimmed**2))) if ref_trimmed.size else 0.0

    metrics = StemMetrics(
        stem=stem,
        correlation=correlation(scored, ref_trimmed),
        si_sdr_db=si_sdr(scored, ref_trimmed),
        log_spectral_distance_db=log_spectral_distance(scored, ref_trimmed, sample_rate),
        lag_samples=best_lag(scored, ref_trimmed),
        peak_candidate=peak_candidate,
        peak_reference=peak_reference,
        rms_candidate=rms_candidate,
        rms_reference=rms_reference,
        samples_compared=int(cand_trimmed.shape[1]),
        samples_dropped=int(dropped),
        has_nan=has_nan,
        has_inf=has_inf,
        candidate_silent=peak_candidate <= SILENCE_PEAK,
        reference_silent=peak_reference <= SILENCE_PEAK,
    )

    if has_nan:
        metrics.failures.append("candidate contains NaN")

    if has_inf:
        metrics.failures.append("candidate contains Inf")

    # Checked before correlation because a dead stem correlates as 0.0 and the
    # generic message would bury the far more specific diagnosis.
    if metrics.candidate_silent and not metrics.reference_silent:
        metrics.failures.append(
            f"candidate is silent (peak {peak_candidate:.2e}) while the reference is not"
        )

    if metrics.correlation < min_correlation:
        metrics.failures.append(
            f"correlation {metrics.correlation:.6f} below {min_correlation:.6f}"
        )

    if metrics.si_sdr_db < min_si_sdr_db:
        metrics.failures.append(f"SI-SDR {metrics.si_sdr_db:.2f} dB below {min_si_sdr_db:.2f} dB")

    if abs(metrics.lag_samples) > max_lag_samples:
        metrics.failures.append(
            f"aligned best at {metrics.lag_samples} samples, expected within "
            f"+/-{max_lag_samples} - suspect an STFT padding or centring difference"
        )

    return metrics
