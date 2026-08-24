"""Audio load/save helpers used by fusion, refinement, and analysis."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

# Logical stem set shared by RoFormer, Demucs 6s, hybrid fusion, and the plugin.
STEM_NAMES = ("vocals", "drums", "bass", "guitar", "piano", "other")


def load_audio(
    path: str | Path,
    target_sr: int | None = None,
    stereo: bool = True,
) -> tuple[np.ndarray, int]:
    """Load audio as contiguous float32 ``[channels, samples]``.

    Args:
        path: File to read.
        target_sr: If set, resample with ``resample_poly``.
        stereo: If true, duplicate mono to two channels.
    """
    audio, sr = sf.read(str(path), dtype="float32", always_2d=True)

    if stereo and audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)

    if target_sr is not None and sr != target_sr:
        g = math.gcd(sr, target_sr)
        audio = resample_poly(
            audio,
            up=target_sr // g,
            down=sr // g,
            axis=0,
        ).astype(np.float32)
        sr = target_sr

    return np.ascontiguousarray(audio.T), sr


def save_audio(path: str | Path, audio: np.ndarray, sample_rate: int) -> None:
    """Write ``[channels, samples]`` float32 audio as a 32-bit float WAV."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(
        str(path),
        np.asarray(audio, dtype=np.float32).T,
        sample_rate,
        subtype="FLOAT",
    )


def peak_normalize(audio: np.ndarray, peak: float = 0.999) -> np.ndarray:
    """Scale so the absolute peak does not exceed ``peak``; leave quieter audio."""
    current = float(np.max(np.abs(audio))) if audio.size else 0.0
    if current <= peak or current <= 1e-12:
        return audio
    return audio * (peak / current)
