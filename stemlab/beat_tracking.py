"""Offline Beat This! inference and robust musical-time interpretation."""

from __future__ import annotations

import hashlib
import os
import sys
import threading
from collections import Counter
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import torch

from .runtime import CancellationToken, JobCancelled

BEAT_THIS_VERSION = "1.1.0"
BEAT_ALGORITHM_VERSION = "beat-this-1.1.0-stemlab-1"


@dataclass(frozen=True)
class ModelSpec:
    name: str
    size: int
    sha256: str


MODEL_SPECS = {
    "fast": ModelSpec(
        "small0",
        8_451_101,
        "6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b",
    ),
    "accurate": ModelSpec(
        "final0",
        81_058_141,
        "8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331",
    ),
}


@dataclass(frozen=True)
class BeatAnalysis:
    bpm: float
    detected_bpm: float
    half_time_bpm: float
    double_time_bpm: float
    beats: tuple[float, ...]
    downbeats: tuple[float, ...]
    meter_numerator: int | None
    meter_denominator: int
    bar_one: float
    confidence: float
    model: str
    model_version: str
    device: str

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


_MODEL_CACHE: dict[tuple[str, str], torch.nn.Module] = {}
_MODEL_LOCK = threading.Lock()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _candidate_model_directories() -> list[Path]:
    candidates: list[Path] = []
    override = os.environ.get("STEMLAB_BEAT_THIS_MODEL_DIR")
    if override:
        candidates.append(Path(override).expanduser())

    executable_dir = Path(sys.executable).resolve().parent
    candidates.extend(
        (
            executable_dir / "Models" / "BeatThis",
            Path(sys.prefix).resolve() / "Models" / "BeatThis",
            Path(__file__).resolve().parents[1] / "models" / "beat_this",
            Path(__file__).resolve().parents[1] / ".portable-cache" / "beat-this-models",
        )
    )

    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(Path(local) / "StemLab" / "Models" / "BeatThis")
    return candidates


def resolve_packaged_model(mode: str, model_dir: str | Path | None = None) -> Path:
    """Resolve and validate a packaged checkpoint without any network fallback."""
    if mode not in MODEL_SPECS:
        raise ValueError(f"Unknown Beat This! mode: {mode}")
    spec = MODEL_SPECS[mode]
    directories = [Path(model_dir).expanduser()] if model_dir else _candidate_model_directories()

    checked: list[str] = []
    for directory in directories:
        path = directory.resolve() / f"{spec.name}.ckpt"
        checked.append(str(path))
        if not path.is_file():
            continue
        if path.stat().st_size != spec.size:
            raise RuntimeError(f"Packaged Beat This! model has the wrong size: {path}")
        digest = _sha256_file(path)
        if digest.lower() != spec.sha256:
            raise RuntimeError(f"Packaged Beat This! model failed SHA-256 validation: {path}")
        return path

    locations = "\n  ".join(checked)
    raise FileNotFoundError(
        f"The packaged Beat This! {spec.name} model is missing. Checked:\n  {locations}"
    )


def choose_device(requested: str = "auto") -> torch.device:
    """Use CUDA when available and otherwise return a safe CPU device."""
    requested = requested.strip().lower()
    if requested in {"", "auto"}:
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    if requested.startswith("cuda") and not torch.cuda.is_available():
        return torch.device("cpu")
    return torch.device(requested)


def _load_model_once(path: Path, device: torch.device) -> torch.nn.Module:
    key = (str(path), str(device))
    with _MODEL_LOCK:
        model = _MODEL_CACHE.get(key)
        if model is None:
            from beat_this.inference import load_model

            # The validated absolute path is deliberate: passing a short name
            # would make upstream Beat This! attempt a network download.
            model = load_model(str(path), device=device)
            _MODEL_CACHE[key] = model
        return model


def _robust_intervals(beats: np.ndarray) -> tuple[np.ndarray, float, float]:
    intervals = np.diff(np.asarray(beats, dtype=np.float64))
    intervals = intervals[np.isfinite(intervals) & (intervals >= 0.18) & (intervals <= 3.0)]
    if intervals.size < 2:
        raise RuntimeError("Beat This! did not detect enough stable beats")

    median = float(np.median(intervals))
    deviations = np.abs(intervals - median)
    mad = float(np.median(deviations))
    tolerance = max(0.035, median * 0.18, 3.5 * mad)
    inliers = intervals[deviations <= tolerance]
    if inliers.size < 2:
        inliers = intervals

    interval = float(np.median(inliers))
    robust_cv = float(np.median(np.abs(inliers - interval)) / max(interval, 1.0e-9))
    inlier_ratio = float(inliers.size / intervals.size)
    return inliers, robust_cv, inlier_ratio


def _estimate_meter(beats: np.ndarray, downbeats: np.ndarray) -> tuple[int | None, float]:
    if beats.size < 3 or downbeats.size < 2:
        return None, 0.0

    indices = []
    for downbeat in downbeats:
        index = int(np.argmin(np.abs(beats - downbeat)))
        if abs(float(beats[index] - downbeat)) <= 0.12:
            indices.append(index)
    differences = [b - a for a, b in zip(indices, indices[1:], strict=False) if 2 <= b - a <= 12]
    if not differences:
        return None, 0.0
    meter, count = Counter(differences).most_common(1)[0]
    return int(meter), float(count / len(differences))


def derive_musical_time(
    beats: np.ndarray,
    downbeats: np.ndarray,
    model_confidence: float,
    *,
    model: str,
    device: str,
) -> BeatAnalysis:
    """Derive tempo, meter, and bar alignment from Beat This! event positions."""
    beats = np.unique(np.asarray(beats, dtype=np.float64))
    downbeats = np.unique(np.asarray(downbeats, dtype=np.float64))
    _intervals, robust_cv, inlier_ratio = _robust_intervals(beats)
    detected_bpm = 60.0 / float(np.median(_intervals))
    meter, meter_confidence = _estimate_meter(beats, downbeats)
    stability = float(np.exp(-8.0 * robust_cv))
    confidence = float(
        np.clip(
            0.50 * stability
            + 0.30 * inlier_ratio
            + 0.15 * model_confidence
            + 0.05 * meter_confidence,
            0.0,
            1.0,
        )
    )
    bar_one = float(downbeats[0] if downbeats.size else beats[0])
    return BeatAnalysis(
        bpm=round(detected_bpm, 3),
        detected_bpm=round(detected_bpm, 3),
        half_time_bpm=round(detected_bpm * 0.5, 3),
        double_time_bpm=round(detected_bpm * 2.0, 3),
        beats=tuple(round(float(value), 6) for value in beats),
        downbeats=tuple(round(float(value), 6) for value in downbeats),
        meter_numerator=meter,
        meter_denominator=4,
        bar_one=round(bar_one, 6),
        confidence=round(confidence, 5),
        model=model,
        model_version=BEAT_THIS_VERSION,
        device=device,
    )


def analyse_beats(
    audio: np.ndarray,
    sample_rate: int,
    *,
    mode: str = "fast",
    device: str = "auto",
    model_dir: str | Path | None = None,
    cancellation: CancellationToken | None = None,
    progress: Callable[[float, str], None] | None = None,
) -> BeatAnalysis:
    """Run packaged Beat This! inference with progress between real chunks."""
    from beat_this.inference import aggregate_prediction, split_piece
    from beat_this.model.postprocessor import Postprocessor
    from beat_this.preprocessing import LogMelSpect
    import soxr

    token = cancellation or CancellationToken()
    token.raise_if_cancelled()
    spec = MODEL_SPECS[mode]
    model_path = resolve_packaged_model(mode, model_dir)
    selected_device = choose_device(device)
    if progress:
        progress(0.05, f"Loading Beat This! {spec.name} on {selected_device.type}")
    try:
        model = _load_model_once(model_path, selected_device)
    except RuntimeError:
        if selected_device.type != "cuda":
            raise
        torch.cuda.empty_cache()
        selected_device = torch.device("cpu")
        if progress:
            progress(0.05, f"CUDA unavailable; loading Beat This! {spec.name} on CPU")
        model = _load_model_once(model_path, selected_device)
    token.raise_if_cancelled()

    mono = np.asarray(audio, dtype=np.float32)
    if mono.ndim == 2:
        mono = np.mean(mono, axis=1)
    if sample_rate != 22_050:
        mono = soxr.resample(mono, in_rate=sample_rate, out_rate=22_050).astype(np.float32)

    if progress:
        progress(0.12, "Computing Beat This! spectrogram")
    signal = torch.as_tensor(mono, dtype=torch.float32, device=selected_device)
    spectrogram = LogMelSpect(device=selected_device)(signal)
    chunks, starts = split_piece(spectrogram, 1500, border_size=6, avoid_short_end=True)
    predictions = []
    use_float16 = selected_device.type == "cuda"

    try:
        with torch.inference_mode():
            for index, chunk in enumerate(chunks):
                token.raise_if_cancelled()
                with torch.autocast(enabled=use_float16, device_type=selected_device.type):
                    prediction = model(chunk.unsqueeze(0))
                predictions.append(
                    {"beat": prediction["beat"][0], "downbeat": prediction["downbeat"][0]}
                )
                if progress:
                    progress(
                        0.15 + 0.72 * ((index + 1) / max(1, len(chunks))),
                        f"Beat This! chunk {index + 1}/{len(chunks)}",
                    )
        token.raise_if_cancelled()
        beat_logits, downbeat_logits = aggregate_prediction(
            predictions,
            starts,
            spectrogram.shape[0],
            1500,
            6,
            "keep_first",
            selected_device,
        )
        beats, downbeats = Postprocessor(type="minimal")(
            beat_logits.float(), downbeat_logits.float()
        )
        beat_frames = np.clip(
            np.rint(np.asarray(beats) * 50).astype(int), 0, beat_logits.numel() - 1
        )
        probabilities = torch.sigmoid(beat_logits[beat_frames]).detach().cpu().numpy()
        model_confidence = float(np.mean(probabilities)) if probabilities.size else 0.0
        if progress:
            progress(0.92, "Interpreting tempo, meter, and downbeats")
        return derive_musical_time(
            np.asarray(beats),
            np.asarray(downbeats),
            model_confidence,
            model=spec.name,
            device=selected_device.type,
        )
    except JobCancelled:
        if selected_device.type == "cuda":
            torch.cuda.empty_cache()
        raise
