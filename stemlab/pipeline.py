"""Top-level separation pipeline shared by the CLI and JUCE worker."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .device import pick_best_device, resolve_torch_device
from .demucs_backend import DEFAULT_DEMUCS_MODEL, DemucsBackend
from .hybrid import fuse_stem_folders
from .pretrained import DEFAULT_MODEL, RoFormerBackend
from .refinement.kick import KickRefinementConfig
from .refinement.pipeline import refine_stem_folder

ENGINE_ROFORMER = "roformer"
ENGINE_DEMUCS = "demucs"
ENGINE_HYBRID = "hybrid"
ENGINE_CHOICES = (
    ENGINE_ROFORMER,
    ENGINE_DEMUCS,
    ENGINE_HYBRID,
)
DEFAULT_ENGINE = ENGINE_ROFORMER


def resolve_device(device: str) -> str:
    """Turn a requested device into one this machine can actually use.

    The plugin asks for "cuda" unconditionally. That was a safe assumption for
    the Windows release, and a bad one on a Linux machine with no NVIDIA GPU,
    a ROCm-only torch build, or a driver mismatch. Falling back to CPU with a
    clear log line beats handing the user a torch stack trace.

    "auto" is accepted for callers that would rather not guess at all, and
    picks the best available backend: cuda (which also covers ROCm), then
    Intel xpu, then cpu. The actual probes live in stemlab.device so the
    pipeline and the individual backends share one answer.
    """
    requested = str(device or "").strip().lower()

    if requested in ("", "auto"):
        return pick_best_device()

    return resolve_torch_device(requested)


@dataclass
class PipelineResult:
    """Locations and engine name produced by one separation run."""

    baseline_dir: Path
    final_dir: Path
    engine: str


def separate(
    input_path: str | Path,
    output_dir: str | Path,
    model: str = DEFAULT_MODEL,
    device: str = "cuda",
    refine: bool = True,
    engine: str = DEFAULT_ENGINE,
    demucs_model: str = DEFAULT_DEMUCS_MODEL,
    refinement_config: KickRefinementConfig | None = None,
    log_callback: Callable[[str], None] | None = None,
    progress_callback: Callable[[float, str], None] | None = None,
) -> PipelineResult:
    """Run the selected model engine and optional kick-bleed refinement.

    Progress callbacks receive a percentage from 0 to 100 and a short stage
    label suitable for the JUCE status display.
    """
    input_path = Path(input_path)
    output_dir = Path(output_dir)
    baseline_dir = output_dir / "baseline"
    final_dir = output_dir / ("refined" if refine else "baseline")

    engine = str(engine).strip().lower()

    if engine not in ENGINE_CHOICES:
        raise ValueError(
            f"Unknown separation engine: {engine}. Choose one of: {', '.join(ENGINE_CHOICES)}"
        )

    def log(message: str):
        if log_callback:
            log_callback(message)
        else:
            print(message, flush=True)

    def progress(percent: float, stage: str):
        percent = max(0.0, min(100.0, float(percent)))

        if progress_callback:
            progress_callback(percent, stage)

        log(f"STEMLAB_PROGRESS {percent:.1f} {stage}")

    def eta(seconds: float):
        # Estimated seconds left in the current model stage, straight from
        # the backend. The plugin counts it down between reports.
        log(f"STEMLAB_ETA {max(0.0, float(seconds)):.0f}")

    def make_download_progress(base_percent: float, stage_name: str):
        # A one-time model download is its own stage: it creeps the bar
        # through a narrow band instead of impersonating separation progress
        # (which follows and would otherwise start from a bar stuck at 80%).
        def on_download(percent: float):
            mapped = base_percent + 4.0 * (max(0.0, min(100.0, percent)) / 100.0)
            progress(mapped, f"Downloading {stage_name} model ({percent:.0f}%)")

        return on_download

    requested_device = str(device or "").strip().lower()
    device = resolve_device(device)

    progress(5.0, "Preparing")
    log(f"Input: {input_path}")
    log(f"Output: {output_dir}")
    log(f"Separation engine: {engine}")

    if requested_device in ("", "auto"):
        log(f"Device: {device} (auto-selected)")
    elif device != requested_device:
        log(f"Device: {device} (requested {requested_device}, which is not available here)")
    else:
        log(f"Device: {device}")

    if engine == ENGINE_ROFORMER:
        progress(10.0, "Loading BS-RoFormer")

        def on_roformer_progress(percent: float):
            mapped = 10.0 + (0.70 * percent)
            progress(
                mapped,
                f"BS-RoFormer ({percent:.0f}%)",
            )

        backend = RoFormerBackend(
            model=model,
            device=device,
            log_callback=log,
            progress_callback=on_roformer_progress,
            eta_callback=eta,
            download_callback=make_download_progress(10.0, "BS-RoFormer"),
        )

        backend.separate(
            input_path=input_path,
            output_dir=baseline_dir,
        )

    elif engine == ENGINE_DEMUCS:
        progress(10.0, "Loading Demucs")

        def on_demucs_progress(percent: float):
            mapped = 10.0 + (0.70 * percent)
            progress(
                mapped,
                f"Demucs ({percent:.0f}%)",
            )

        backend = DemucsBackend(
            model=demucs_model,
            device=device,
            log_callback=log,
            progress_callback=on_demucs_progress,
            eta_callback=eta,
            download_callback=make_download_progress(10.0, "Demucs"),
        )

        backend.separate(
            input_path=input_path,
            output_dir=baseline_dir,
        )

    else:
        roformer_dir = output_dir / "engines" / "roformer"
        demucs_dir = output_dir / "engines" / "demucs"

        progress(10.0, "Hybrid: loading BS-RoFormer")

        def on_hybrid_roformer(percent: float):
            mapped = 10.0 + (0.32 * percent)
            progress(
                mapped,
                f"Hybrid - BS-RoFormer ({percent:.0f}%)",
            )

        RoFormerBackend(
            model=model,
            device=device,
            log_callback=log,
            progress_callback=on_hybrid_roformer,
            eta_callback=eta,
            download_callback=make_download_progress(10.0, "BS-RoFormer"),
        ).separate(
            input_path=input_path,
            output_dir=roformer_dir,
        )

        progress(42.0, "Hybrid: loading Demucs")

        def on_hybrid_demucs(percent: float):
            mapped = 42.0 + (0.32 * percent)
            progress(
                mapped,
                f"Hybrid - Demucs ({percent:.0f}%)",
            )

        DemucsBackend(
            model=demucs_model,
            device=device,
            log_callback=log,
            progress_callback=on_hybrid_demucs,
            eta_callback=eta,
            download_callback=make_download_progress(42.0, "Demucs"),
        ).separate(
            input_path=input_path,
            output_dir=demucs_dir,
        )

        progress(74.0, "Hybrid: fusing model estimates")
        log(
            "Running StemLab hybrid spectral fusion. "
            "Models are executed sequentially to keep GPU memory usage lower."
        )

        def on_fusion_progress(index: int, total: int, stem: str):
            fraction = index / max(1, total)
            mapped = 74.0 + 6.0 * fraction
            progress(
                mapped,
                f"Hybrid fusion - {stem.title()} ({index}/{total})",
            )

        fuse_stem_folders(
            roformer_dir=roformer_dir,
            demucs_dir=demucs_dir,
            output_dir=baseline_dir,
            log_callback=log,
            progress_callback=on_fusion_progress,
        )

    progress(80.0, "Separation complete")

    if refine:
        progress(82.0, "Preparing refinement")
        log("Running StemLab adaptive refinement...")

        def on_refine_progress(index: int, total: int, stem: str):
            fraction = index / max(1, total)
            mapped = 82.0 + 13.0 * fraction
            progress(
                mapped,
                f"Refining {stem.title()} ({index}/{total})",
            )

        stats_by_stem = refine_stem_folder(
            input_dir=baseline_dir,
            output_dir=final_dir,
            cfg=refinement_config,
            progress_callback=on_refine_progress,
        )

        for stem, stats in stats_by_stem.items():
            log(
                f"{stem}: "
                f"events={stats.events_detected}, "
                f"attempted={stats.cancellations_attempted}, "
                f"applied={stats.cancellations_applied}, "
                f"match={stats.mean_confidence:.3f}"
            )

        log("Refinement complete.")
        progress(95.0, "Finalizing")
    else:
        log("Refinement disabled; baseline stems are final output.")
        progress(95.0, "Finalizing")

    return PipelineResult(
        baseline_dir=baseline_dir,
        final_dir=final_dir,
        engine=engine,
    )
