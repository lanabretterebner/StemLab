"""Experimental DSP splitter for a prominent lead and its backing bed."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, signal

_EPS = 1.0e-8


@dataclass(frozen=True)
class ForegroundSplit:
    """Paths and confidence returned by :func:`split_foreground`."""

    foreground: Path
    backing: Path
    confidence: float


def _mask_channel(mid: np.ndarray, side: np.ndarray | None) -> np.ndarray:
    nperseg = 2048
    noverlap = 1536
    _, _, mid_spec = signal.stft(
        mid,
        nperseg=nperseg,
        noverlap=noverlap,
        boundary="zeros",
        padded=True,
    )
    mid_mag = np.abs(mid_spec).astype(np.float32) + _EPS

    local_floor = ndimage.median_filter(mid_mag, size=(17, 1), mode="nearest") + _EPS
    prominence = mid_mag / local_floor
    prominence_score = np.clip((prominence - 1.0) / 3.0, 0.0, 1.0)

    if side is not None:
        _, _, side_spec = signal.stft(
            side,
            nperseg=nperseg,
            noverlap=noverlap,
            boundary="zeros",
            padded=True,
        )
        side_mag = np.abs(side_spec).astype(np.float32) + _EPS
        centre_score = mid_mag / (mid_mag + side_mag)
        mask = 0.68 * centre_score + 0.32 * prominence_score
    else:
        # Mono has no spatial evidence, so be more conservative.
        mask = 0.72 * prominence_score + 0.18

    mask = np.clip(mask, 0.0, 1.0) ** 1.7
    foreground_spec = mid_spec * mask
    _, foreground = signal.istft(
        foreground_spec,
        nperseg=nperseg,
        noverlap=noverlap,
        input_onesided=True,
        boundary=True,
    )
    return foreground.astype(np.float32, copy=False)


def _split_block(block: np.ndarray) -> tuple[np.ndarray, np.ndarray, float]:
    channels = block.shape[1]

    if channels >= 2:
        left = block[:, 0]
        right = block[:, 1]
        mid = (left + right) * 0.5
        side = (left - right) * 0.5
        foreground_mono = _mask_channel(mid, side)
        foreground_mono = foreground_mono[: block.shape[0]]
        if foreground_mono.shape[0] < block.shape[0]:
            foreground_mono = np.pad(
                foreground_mono,
                (0, block.shape[0] - foreground_mono.shape[0]),
            )
        foreground = np.zeros_like(block)
        foreground[:, 0] = foreground_mono
        foreground[:, 1] = foreground_mono
        if channels > 2:
            foreground[:, 2:] = 0.0

        denom = float(np.linalg.norm(mid) * np.linalg.norm(side))
        spatial_independence = 0.5
        if denom > _EPS:
            spatial_independence = 1.0 - min(1.0, abs(float(np.dot(mid, side) / denom)))
        confidence = float(np.clip(0.52 + 0.32 * spatial_independence, 0.0, 1.0))
    else:
        mono = block[:, 0]
        foreground_mono = _mask_channel(mono, None)[: block.shape[0]]
        if foreground_mono.shape[0] < block.shape[0]:
            foreground_mono = np.pad(
                foreground_mono,
                (0, block.shape[0] - foreground_mono.shape[0]),
            )
        foreground = foreground_mono[:, None].astype(np.float32)
        confidence = 0.42

    # Residual reconstruction is exact in the time domain, which avoids
    # inventing energy simply because the foreground mask is imperfect.
    backing = block - foreground
    return foreground, backing, confidence


def split_foreground(
    input_path: Path,
    output_dir: Path,
    *,
    progress=None,
) -> ForegroundSplit:
    """Experimental lead/foreground peel with chunked overlap-add.

    This is a role-oriented DSP backend, not a semantic instrument classifier.
    It works best when a lead is spectrally prominent and/or more centred than
    the accompaniment.  A learned backend can replace this function later
    without changing the StemLab tree/manifest contract.
    """
    input_path = Path(input_path)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    audio, sample_rate = sf.read(
        str(input_path),
        dtype="float32",
        always_2d=True,
    )
    if audio.size == 0:
        raise RuntimeError("Lead split source is empty")

    frame_count = audio.shape[0]
    chunk = max(4096, int(sample_rate * 12.0))
    overlap = max(1024, int(sample_rate * 0.5))
    hop = max(1, chunk - overlap)

    foreground_sum = np.zeros_like(audio, dtype=np.float32)
    backing_sum = np.zeros_like(audio, dtype=np.float32)
    weights = np.zeros((frame_count, 1), dtype=np.float32)
    confidences: list[float] = []

    starts = list(range(0, frame_count, hop))
    for block_index, start in enumerate(starts):
        end = min(frame_count, start + chunk)
        block = audio[start:end]
        foreground, backing, confidence = _split_block(block)
        confidences.append(confidence)

        weight = np.ones((end - start, 1), dtype=np.float32)
        fade = min(overlap, (end - start) // 2)
        if fade > 1:
            ramp = np.sin(np.linspace(0.0, np.pi * 0.5, fade, dtype=np.float32)) ** 2
            if start > 0:
                weight[:fade, 0] *= ramp
            if end < frame_count:
                weight[-fade:, 0] *= ramp[::-1]

        foreground_sum[start:end] += foreground[: end - start] * weight
        backing_sum[start:end] += backing[: end - start] * weight
        weights[start:end] += weight

        if progress:
            fraction = (block_index + 1) / max(1, len(starts))
            progress(12.0 + 78.0 * fraction, "Adaptive lead / foreground split")

    weights = np.maximum(weights, _EPS)
    foreground_audio = foreground_sum / weights
    backing_audio = backing_sum / weights

    foreground_path = output_dir / "lead_foreground.wav"
    backing_path = output_dir / "backing_bed.wav"
    sf.write(str(foreground_path), foreground_audio, sample_rate, subtype="FLOAT")
    sf.write(str(backing_path), backing_audio, sample_rate, subtype="FLOAT")

    return ForegroundSplit(
        foreground=foreground_path.resolve(),
        backing=backing_path.resolve(),
        confidence=float(np.median(confidences) if confidences else 0.4),
    )
