"""Fuse RoFormer and Demucs estimates in the time-frequency domain."""

from __future__ import annotations

import math
from pathlib import Path
from typing import Callable

import numpy as np
from scipy.signal import istft, stft

from .audio import STEM_NAMES, find_stem_file, load_audio, peak_normalize, save_audio

# RoFormer preference when the two models disagree strongly. Agreement tends
# toward an even 50/50 fusion. These are intentionally conservative rather
# than pretending that either model is universally better.
ROFORMER_PREFERENCE = {
    "vocals": 0.64,
    "drums": 0.46,
    "bass": 0.54,
    "guitar": 0.60,
    "piano": 0.60,
    "other": 0.55,
}


def _pad_to_length(audio: np.ndarray, length: int) -> np.ndarray:
    if audio.shape[1] == length:
        return audio

    if audio.shape[1] > length:
        return audio[:, :length]

    pad = length - audio.shape[1]
    return np.pad(
        audio,
        ((0, 0), (0, pad)),
        mode="constant",
    )


def _fuse_channel(
    roformer: np.ndarray,
    demucs: np.ndarray,
    roformer_preference: float,
    n_fft: int = 2048,
    hop: int = 512,
) -> np.ndarray:
    if roformer.size == 0:
        return roformer.astype(np.float32, copy=True)

    noverlap = n_fft - hop

    _, _, zr = stft(
        roformer,
        nperseg=n_fft,
        noverlap=noverlap,
        window="hann",
        boundary="zeros",
        padded=True,
    )

    _, _, zd = stft(
        demucs,
        nperseg=n_fft,
        noverlap=noverlap,
        window="hann",
        boundary="zeros",
        padded=True,
    )

    mag_r = np.abs(zr)
    mag_d = np.abs(zd)

    eps = np.float32(1e-7)

    # 1 when the two models agree on local magnitude, approaching 0 as their
    # estimates diverge. The log-ratio makes equal relative disagreements act
    # similarly at quiet and loud levels.
    disagreement = np.abs(np.log((mag_r + eps) / (mag_d + eps)))
    agreement = np.exp(-disagreement).astype(np.float32)

    # Agreement -> 50/50 consensus.
    # Disagreement -> gently favor the model with the stronger prior for this
    # stem instead of blindly averaging contradictory estimates.
    weight_r = (0.5 * agreement + roformer_preference * (1.0 - agreement)).astype(np.float32)

    weight_d = 1.0 - weight_r

    mixed_complex = weight_r * zr + weight_d * zd

    target_magnitude = weight_r * mag_r + weight_d * mag_d

    # Straight complex averaging can accidentally cancel when the models have
    # small phase differences. Restore the intended weighted magnitude while
    # retaining the blended phase.
    mixed_magnitude = np.abs(mixed_complex)
    fused = mixed_complex * (target_magnitude / (mixed_magnitude + eps))

    _, audio = istft(
        fused,
        nperseg=n_fft,
        noverlap=noverlap,
        window="hann",
        input_onesided=True,
        boundary=True,
    )

    return np.asarray(
        audio[: roformer.shape[0]],
        dtype=np.float32,
    )


def _fuse_audio_chunk(
    roformer: np.ndarray,
    demucs: np.ndarray,
    stem: str,
) -> np.ndarray:
    channels = min(
        roformer.shape[0],
        demucs.shape[0],
    )

    outputs = []

    for channel in range(channels):
        outputs.append(
            _fuse_channel(
                roformer[channel],
                demucs[channel],
                ROFORMER_PREFERENCE.get(stem, 0.55),
            )
        )

    return np.stack(outputs, axis=0)


def fuse_stem_pair(
    roformer_path: Path,
    demucs_path: Path,
    output_path: Path,
    stem: str,
    chunk_seconds: float = 16.0,
    overlap_seconds: float = 0.35,
) -> None:
    """Fuse one matching pair of model outputs and write a normalized WAV."""
    roformer, sr = load_audio(
        roformer_path,
        stereo=True,
    )

    demucs, _ = load_audio(
        demucs_path,
        target_sr=sr,
        stereo=True,
    )

    length = max(
        roformer.shape[1],
        demucs.shape[1],
    )

    roformer = _pad_to_length(roformer, length)
    demucs = _pad_to_length(demucs, length)

    result = np.zeros_like(
        roformer,
        dtype=np.float32,
    )

    weight = np.zeros(
        length,
        dtype=np.float32,
    )

    chunk = max(
        4096,
        int(round(chunk_seconds * sr)),
    )

    overlap = max(
        0,
        min(
            chunk // 3,
            int(round(overlap_seconds * sr)),
        ),
    )

    step = max(
        1,
        chunk - overlap,
    )

    for start in range(0, length, step):
        end = min(
            length,
            start + chunk,
        )

        fused = _fuse_audio_chunk(
            roformer[:, start:end],
            demucs[:, start:end],
            stem,
        )

        local_length = end - start

        if fused.shape[1] < local_length:
            fused = _pad_to_length(
                fused,
                local_length,
            )
        else:
            fused = fused[:, :local_length]

        envelope = np.ones(
            local_length,
            dtype=np.float32,
        )

        if overlap > 0:
            fade = min(
                overlap,
                local_length // 2,
            )

            if start > 0 and fade > 0:
                phase = np.linspace(
                    0.0,
                    math.pi / 2.0,
                    fade,
                    endpoint=True,
                    dtype=np.float32,
                )
                envelope[:fade] = np.sin(phase) ** 2

            if end < length and fade > 0:
                phase = np.linspace(
                    math.pi / 2.0,
                    0.0,
                    fade,
                    endpoint=True,
                    dtype=np.float32,
                )
                envelope[-fade:] = np.sin(phase) ** 2

        result[:, start:end] += fused * envelope[None, :]

        weight[start:end] += envelope

        if end >= length:
            break

    result /= np.maximum(
        weight[None, :],
        1e-6,
    )

    result = peak_normalize(result, peak=0.999)

    save_audio(
        output_path,
        result,
        sr,
    )


def fuse_stem_folders(
    roformer_dir: str | Path,
    demucs_dir: str | Path,
    output_dir: str | Path,
    log_callback: Callable[[str], None] | None = None,
    progress_callback: Callable[[int, int, str], None] | None = None,
) -> list[Path]:
    """Fuse all six named stems from two model-output directories."""
    roformer_dir = Path(roformer_dir)
    demucs_dir = Path(demucs_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    files: list[Path] = []

    for index, stem in enumerate(STEM_NAMES, start=1):
        roformer_path = find_stem_file(
            roformer_dir,
            stem,
        )
        demucs_path = find_stem_file(
            demucs_dir,
            stem,
        )
        if roformer_path is None or demucs_path is None:
            missing = roformer_dir if roformer_path is None else demucs_dir
            raise FileNotFoundError(f"Could not find {stem} in {missing}")
        output_path = output_dir / f"{stem}.wav"

        if log_callback:
            log_callback(
                f"Hybrid fusion: {stem} "
                f"(RoFormer preference {ROFORMER_PREFERENCE.get(stem, 0.55):.2f})"
            )

        fuse_stem_pair(
            roformer_path=roformer_path,
            demucs_path=demucs_path,
            output_path=output_path,
            stem=stem,
        )

        files.append(output_path)

        if progress_callback:
            progress_callback(
                index,
                len(STEM_NAMES),
                stem,
            )

    return files
