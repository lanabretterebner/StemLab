"""Estimate whether an audio stem contains material worth splitting again."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np
import soundfile as sf

_EPS = 1.0e-9


@dataclass(frozen=True)
class AudioProfile:
    """Small, model-free summary of one audio file's apparent complexity."""

    rms: float
    estimated_source_count: int
    confidence: float


@dataclass(frozen=True)
class ChildAssessment:
    """Evidence used by the recursion policy to accept or reject a child stem."""

    confidence: float
    estimated_source_count: int
    energy_ratio: float


def _read_analysis_excerpt(
    path: Path, *, seconds_per_window: float = 8.0
) -> tuple[np.ndarray, int]:
    """Read a few representative windows instead of loading a whole song."""
    info = sf.info(str(path))
    sample_rate = int(info.samplerate)
    total_frames = int(info.frames)
    window_frames = max(1, int(seconds_per_window * sample_rate))

    if total_frames <= window_frames * 3:
        audio, _ = sf.read(str(path), dtype="float32", always_2d=True)
        return audio, sample_rate

    starts = (
        0,
        max(0, total_frames // 2 - window_frames // 2),
        max(0, total_frames - window_frames),
    )
    windows: list[np.ndarray] = []
    with sf.SoundFile(str(path), "r") as handle:
        for start in starts:
            handle.seek(start)
            block = handle.read(window_frames, dtype="float32", always_2d=True)
            if block.size:
                windows.append(block)

    if not windows:
        return np.zeros((1, max(1, info.channels)), dtype=np.float32), sample_rate

    return np.concatenate(windows, axis=0), sample_rate


def _frame_rms(mono: np.ndarray, frame: int = 2048, hop: int = 1024) -> np.ndarray:
    if mono.size < frame:
        return np.asarray([float(np.sqrt(np.mean(np.square(mono), dtype=np.float64) + _EPS))])

    count = 1 + (mono.size - frame) // hop
    out = np.empty(count, dtype=np.float64)
    for index in range(count):
        start = index * hop
        block = mono[start : start + frame]
        out[index] = np.sqrt(np.mean(np.square(block), dtype=np.float64) + _EPS)
    return out


def _spectral_flatness(mono: np.ndarray) -> float:
    if mono.size < 256:
        return 0.0

    frame = 2048
    max_frames = 96
    values: list[float] = []
    window = np.hanning(frame).astype(np.float32)

    # The array handed in is an excerpt stitched from the start, the middle
    # and the end of the file. A fixed hop of one frame spent the whole
    # 96-frame budget inside the first ~4.5 s of it at 44.1 kHz, so the
    # median below described the opening seconds of the track rather than the
    # track. Striding spends the same budget across all three windows.
    hop = max(frame, (mono.size - frame) // max(1, max_frames - 1))

    for start in range(0, max(1, mono.size - frame + 1), hop):
        block = mono[start : start + frame]
        if block.size < frame:
            break
        mag = np.abs(np.fft.rfft(block * window)).astype(np.float64) + _EPS
        geometric = float(np.exp(np.mean(np.log(mag))))
        arithmetic = float(np.mean(mag))
        values.append(geometric / max(_EPS, arithmetic))

    if not values:
        return 0.0
    return float(np.clip(np.median(values), 0.0, 1.0))


def analyse_audio(path: Path) -> AudioProfile:
    """Analyze representative excerpts and return a conservative source estimate."""
    path = Path(path)
    audio, _sample_rate = _read_analysis_excerpt(path)
    if audio.ndim != 2:
        audio = np.atleast_2d(audio).T

    if audio.size == 0:
        # np.mean over an empty array is NaN, and NaN spreads from rms into
        # the confidence written to the manifest - where it serialises as
        # the bare token NaN, which is not valid JSON and makes the plugin
        # reject an otherwise complete split.
        return AudioProfile(rms=0.0, estimated_source_count=1, confidence=0.0)

    mono = np.mean(audio, axis=1, dtype=np.float64)
    mono = np.nan_to_num(mono, nan=0.0, posinf=0.0, neginf=0.0)
    rms = float(np.sqrt(np.mean(np.square(mono), dtype=np.float64) + _EPS))
    peak = float(np.max(np.abs(np.nan_to_num(audio, nan=0.0, posinf=0.0, neginf=0.0))))

    frames = _frame_rms(mono)
    active_threshold = max(1.0e-5, rms * 0.18)
    active_fraction = float(np.mean(frames > active_threshold)) if frames.size else 0.0

    if audio.shape[1] >= 2:
        left = audio[:, 0].astype(np.float64, copy=False)
        right = audio[:, 1].astype(np.float64, copy=False)
        left -= np.mean(left)
        right -= np.mean(right)
        denom = float(np.linalg.norm(left) * np.linalg.norm(right))
        stereo_correlation = float(np.dot(left, right) / denom) if denom > _EPS else 1.0
        stereo_correlation = float(np.clip(stereo_correlation, -1.0, 1.0))
    else:
        stereo_correlation = 1.0

    flatness = _spectral_flatness(mono)
    crest = peak / max(_EPS, rms)
    crest_norm = float(np.clip((crest - 2.0) / 10.0, 0.0, 1.0))
    stereo_width = float(np.clip((1.0 - stereo_correlation) * 0.5, 0.0, 1.0))

    # This is deliberately a conservative *compositeness* estimate, not a
    # claim that we can count musicians from a spectrogram.  It only decides
    # whether offering another recursive split is worth the compute.
    complexity = float(
        np.clip(
            0.38 * active_fraction + 0.24 * flatness + 0.22 * stereo_width + 0.16 * crest_norm,
            0.0,
            1.0,
        )
    )

    if complexity < 0.30:
        estimated_count = 1
    elif complexity < 0.47:
        estimated_count = 2
    elif complexity < 0.62:
        estimated_count = 3
    elif complexity < 0.75:
        estimated_count = 4
    else:
        estimated_count = 5

    # Confidence drops for nearly silent material, where all of the other
    # descriptors become less meaningful.
    loudness_confidence = float(np.clip(rms / 0.02, 0.0, 1.0))
    confidence = float(np.clip(0.35 + 0.65 * loudness_confidence, 0.0, 1.0))

    # Degenerate input must never reach the manifest as NaN/inf: the split
    # policy compares confidences with '<', where every NaN comparison is
    # False and silently inverts the conservative gates.
    if not math.isfinite(confidence):
        confidence = 0.0

    if not math.isfinite(rms):
        rms = 0.0

    return AudioProfile(
        rms=rms,
        estimated_source_count=estimated_count,
        confidence=confidence,
    )


def _excerpt_mono(path: Path) -> np.ndarray:
    audio, _sample_rate = _read_analysis_excerpt(path, seconds_per_window=5.0)
    if audio.ndim != 2:
        audio = np.atleast_2d(audio).T
    mono = np.mean(audio, axis=1, dtype=np.float64)
    if mono.size:
        mono -= np.mean(mono)
    return mono


def _correlation(a: np.ndarray, b: np.ndarray) -> float:
    length = min(a.size, b.size)
    if length < 32:
        return 0.0
    a = a[:length]
    b = b[:length]
    denom = float(np.linalg.norm(a) * np.linalg.norm(b))
    if denom <= _EPS:
        return 0.0
    return float(np.clip(np.dot(a, b) / denom, -1.0, 1.0))


def assess_children(parent: Path, children: Iterable[Path]) -> dict[Path, ChildAssessment]:
    """Score child usefulness and discourage duplicate/ghost output stems."""
    parent_profile = analyse_audio(parent)
    child_paths = [Path(path) for path in children]
    profiles = {path: analyse_audio(path) for path in child_paths}
    excerpts = {path: _excerpt_mono(path) for path in child_paths}

    duplicate_penalty: dict[Path, float] = {path: 0.0 for path in child_paths}
    for index, first in enumerate(child_paths):
        for second in child_paths[index + 1 :]:
            corr = abs(_correlation(excerpts[first], excerpts[second]))
            if corr > 0.985:
                # Penalise the quieter member of a near-duplicate pair.
                quieter = first if profiles[first].rms <= profiles[second].rms else second
                duplicate_penalty[quieter] = max(duplicate_penalty[quieter], (corr - 0.985) / 0.015)

    result: dict[Path, ChildAssessment] = {}
    for path in child_paths:
        profile = profiles[path]
        energy_ratio = profile.rms / max(parent_profile.rms, _EPS)
        energy_score = float(np.clip(energy_ratio / 0.20, 0.0, 1.0))
        independence = 1.0 - float(np.clip(duplicate_penalty[path], 0.0, 1.0))
        confidence = float(
            np.clip(
                0.50 * profile.confidence + 0.30 * energy_score + 0.20 * independence,
                0.0,
                1.0,
            )
        )
        result[path] = ChildAssessment(
            confidence=confidence,
            estimated_source_count=profile.estimated_source_count,
            energy_ratio=float(energy_ratio),
        )

    return result
