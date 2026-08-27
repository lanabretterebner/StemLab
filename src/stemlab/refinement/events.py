"""Detect low-frequency transient events in a drum stem."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy.ndimage import uniform_filter1d
from scipy.signal import butter, find_peaks, sosfiltfilt


@dataclass(frozen=True)
class Event:
    """A detected transient's sample position and normalized confidence."""

    sample: int
    confidence: float


def _mono(audio: np.ndarray) -> np.ndarray:
    if audio.ndim == 1:
        return audio.astype(np.float32, copy=False)
    return audio.mean(axis=0).astype(np.float32, copy=False)


def _lowpass(x: np.ndarray, sr: int, hz: float = 180.0) -> np.ndarray:
    sos = butter(
        4,
        min(hz / (sr / 2.0), 0.99),
        btype="low",
        output="sos",
    )
    return sosfiltfilt(sos, x).astype(np.float32)


def detect_kick_events(
    drums: np.ndarray,
    sr: int,
    min_interval_ms: float = 90.0,
    threshold_std: float = 1.15,
) -> list[Event]:
    """Detect low-frequency transient onsets in a drum stem.

    This is intentionally signal-processing-first, not ML. It gives us event
    locations for the adaptive cancellation experiment and is easy to inspect.
    """
    x = _mono(drums)

    # sosfiltfilt needs more samples than its edge padding, and the envelope
    # work below indexes env[0]; neither survives an empty or very short
    # stem. Nothing to refine there anyway.
    if x.shape[0] < 64:
        return []

    low = _lowpass(x, sr)

    # Short RMS-ish low-frequency energy envelope. The O(N) running mean
    # equals a normalized ``ones(win)/win`` boxcar in "same" mode:
    # ``mode="constant"``/``cval=0`` reproduces the zero padding and the
    # window is centred identically. Summing runs in float64 because the
    # moving-sum accumulator would drift in float32 across a whole song.
    win = max(8, int(sr * 0.008))
    energy = uniform_filter1d(
        np.square(low, dtype=np.float64),
        size=win,
        mode="constant",
        cval=0.0,
    ).astype(np.float32)
    env = np.sqrt(np.maximum(energy, 0.0))

    # Emphasize attack rather than sustained sub/bass.
    derivative = np.maximum(np.diff(env, prepend=env[0]), 0.0)

    med = float(np.median(derivative))
    std = float(np.std(derivative)) + 1e-12
    threshold = med + threshold_std * std

    min_distance = max(1, int(sr * min_interval_ms / 1000.0))
    peaks, props = find_peaks(
        derivative,
        height=threshold,
        distance=min_distance,
    )

    heights = props.get("peak_heights", np.zeros_like(peaks, dtype=float))
    if len(heights) == 0:
        return []

    # Robust confidence scaling. 1.0 means a very strong low-frequency onset.
    scale = float(np.percentile(heights, 90)) + 1e-12
    events = [
        Event(
            sample=int(p),
            confidence=float(np.clip(h / scale, 0.0, 1.0)),
        )
        for p, h in zip(peaks, heights, strict=True)
    ]

    return events
