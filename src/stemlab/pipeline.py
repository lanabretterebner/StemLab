"""Top-level separation pipeline shared by the CLI and JUCE worker."""

from __future__ import annotations

import os
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np

from .audio import STEM_NAMES, find_stem_file
from .device import pick_best_device, resolve_torch_device
from .demucs_backend import DEFAULT_DEMUCS_MODEL, DemucsBackend
from .hybrid import fuse_stem_folders
from .pretrained import DEFAULT_MODEL, RoFormerBackend
from .refinement.kick import KickRefinementConfig
from .refinement.pipeline import refine_stem_folder, reusable_stems
from .runtime import CancellationToken

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


def _format_clock(seconds: float) -> str:
    """``195.0`` -> ``"3:15"`` for status labels."""
    total = max(0, int(seconds + 0.5))
    return f"{total // 60}:{total % 60:02d}"


def _input_duration_seconds(path: Path) -> float | None:
    """Best-effort source duration for the status line; never fatal."""
    try:
        import soundfile as sf

        info = sf.info(str(path))
        if info.samplerate > 0:
            return float(info.frames) / float(info.samplerate)
    except Exception:
        return None
    return None


class _JobProgress:
    """One monotonic job bar and one job-level ETA over many stages.

    Each stage owns a band of the bar. Stage-local progress maps into the
    band; a stage-local ETA (a backend's own estimate) is extended across
    the stages still to come, assuming their time is proportional to their
    band width - the same assumption the bar itself makes. Stages whose
    backend reports no ETA (fusion, refinement, finalizing) infer one from
    their own elapsed time once they have moved far enough to trust it.

    That job-level number is what STEMLAB_ETA carries now. Previously it
    was the raw stage estimate, so the countdown hit zero at the end of
    every model stage and the display lurched at each boundary.
    """

    def __init__(
        self,
        emit: Callable[[float, str], None],
        emit_eta: Callable[[float], None],
    ) -> None:
        self._emit = emit
        self._emit_eta = emit_eta
        self._lo = 0.0
        self._hi = 100.0
        self._fraction = 0.0
        self._started = time.monotonic()

    def begin(self, lo: float, hi: float) -> None:
        self._lo = float(lo)
        self._hi = float(hi)
        self._fraction = 0.0
        self._started = time.monotonic()

    def stage(self, fraction: float, label: str) -> None:
        # Monotonic within the stage: a backend that rewinds (a second
        # model pass restarting its bar at 0%) must not rewind the file.
        self._fraction = max(self._fraction, max(0.0, min(1.0, fraction)))
        self._emit(self._lo + (self._hi - self._lo) * self._fraction, label)

    def stage_eta(self, stage_remaining: float) -> None:
        """Extend a backend's stage estimate into a job estimate.

        The stage's total time is simply what it has spent plus what its
        backend says is left. Deriving it from ``remaining / (1 - f)``
        instead - the obvious formula - collapses to zero at the end of a
        stage, which zeroed the whole job ETA while later stages remained.
        """
        stage_remaining = max(0.0, float(stage_remaining))
        width = max(1e-6, self._hi - self._lo)

        elapsed = max(0.0, time.monotonic() - self._started)
        stage_total = elapsed + stage_remaining

        seconds_per_percent = stage_total / width
        tail = (100.0 - self._hi) * seconds_per_percent

        self._emit_eta(stage_remaining + max(0.0, tail))

    def elapsed_eta(self) -> None:
        """Infer a job ETA from stage elapsed time (ETA-less stages)."""
        elapsed = time.monotonic() - self._started

        if self._fraction < 0.05 or elapsed < 1.0:
            return

        stage_total = elapsed / self._fraction
        width = max(1e-6, self._hi - self._lo)
        seconds_per_percent = stage_total / width
        tail = (100.0 - self._hi) * seconds_per_percent

        self._emit_eta(max(0.0, stage_total - elapsed) + max(0.0, tail))


@dataclass
class PipelineResult:
    """Locations and engine name produced by one separation run."""

    baseline_dir: Path
    final_dir: Path
    engine: str


def _discard_intermediates(
    output_dir: Path, baseline_dir: Path, final_dir: Path, log
) -> None:
    """Delete the stem sets that were only steps on the way to the final one.

    A hybrid run with refinement writes four full sets of stems: one per
    model under engines/, the fused baseline/, and the refined/ output. Only
    the last is the answer; the rest are working files that quietly multiplied
    the size of every job by four.

    The safety rule is the whole of this function: nothing is removed until
    the final folder is known to hold stems. Refinement writing nothing - a
    bug, a backend that failed politely - must not end with this deleting the
    only copy that survived.

    Set STEMLAB_KEEP_INTERMEDIATES=1 to keep them, which is how you compare
    what fusion did against what each model gave it.
    """
    if os.environ.get("STEMLAB_KEEP_INTERMEDIATES", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "on",
    }:
        return

    try:
        survivors = [
            stem for stem in STEM_NAMES if find_stem_file(final_dir, stem) is not None
        ]

        if not survivors:
            log("Keeping the working folders: the final folder has no stems in it.")
            return

        # engines/ is hybrid's per-model output and is never the answer;
        # baseline/ is the answer only when refinement did not run, in which
        # case it is final_dir and is not in this list at all.
        removable = [output_dir / "engines"]

        if baseline_dir != final_dir:
            removable.append(baseline_dir)

        for path in removable:
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
    except Exception as exc:
        # Tidying up is never worth failing a finished separation over.
        log(f"Could not remove the working folders ({exc}); they are still there.")


def separate(
    input_path: str | Path,
    output_dir: str | Path,
    model: str = DEFAULT_MODEL,
    device: str = "cuda",
    refine: bool = True,
    normalize_fused: bool = False,
    engine: str = DEFAULT_ENGINE,
    demucs_model: str = DEFAULT_DEMUCS_MODEL,
    refinement_config: KickRefinementConfig | None = None,
    log_callback: Callable[[str], None] | None = None,
    progress_callback: Callable[[float, str], None] | None = None,
    cancellation: CancellationToken | None = None,
) -> PipelineResult:
    """Run the selected model engine and optional kick-bleed refinement.

    Progress callbacks receive a percentage from 0 to 100 and a short stage
    label suitable for the JUCE status display.
    """
    input_path = Path(input_path)
    output_dir = Path(output_dir)
    baseline_dir = output_dir / "baseline"
    final_dir = output_dir / ("refined" if refine else "baseline")

    # Filled by the hybrid fusion stage with {stem: (audio, sr)} so the
    # refinement stage can reuse the arrays fusion just wrote instead of
    # decoding those files straight back off disk.
    fused_stems: dict[str, tuple[np.ndarray, int]] | None = None

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
        if cancellation:
            cancellation.raise_if_cancelled()

        percent = max(0.0, min(100.0, float(percent)))

        if progress_callback:
            progress_callback(percent, stage)

        log(f"STEMLAB_PROGRESS {percent:.1f} {stage}")

    def eta(seconds: float):
        # Estimated seconds left in the whole job (see _JobProgress); the
        # plugin counts it down between reports.
        log(f"STEMLAB_ETA {max(0.0, float(seconds)):.0f}")

    announced: set[str] = set()

    def stem_ready(stem: str, path: Path):
        """Publish one finished stem as ``STEMLAB_STEM_READY <stem> <path>``.

        The path must be the one the plugin reads for the rest of the job,
        and the line may only follow the write that made that file final.
        Hence never a baseline/ path on a refine run: the plugin prefers
        job/refined the moment that directory exists, and refinement
        creates it before writing anything, so a baseline line would name a
        file every other consumer has already stopped resolving to, and
        would swap the file under a user mid-audition once superseded.

        An announcement cannot be retracted - the cancel path leaves the
        process through os._exit/SystemExit with no cleanup hook - and is
        not meant to be: "ready" states that this file is complete on disk,
        not that the job will finish. The plugin drops every announced lane
        when a job ends without success.

        Reporting must never abort a separation, so the whole body is best
        effort.
        """
        try:
            name = str(stem).strip().lower()

            if name in announced or name not in STEM_NAMES:
                return

            resolved = str(Path(path).resolve())

            # One line, "first token is the stem, the rest is the path": a
            # control character in the name would split or truncate it. The
            # plugin's own end-of-job scan covers those stems instead.
            if any(ord(char) < 0x20 for char in resolved):
                return

            announced.add(name)

            log(f"STEMLAB_STEM_READY {name} {resolved}")
        except Exception:
            return

    def announce_folder(folder: Path):
        """Announce every stem that resolves to a real file under ``folder``.

        Extensions and naming vary by backend, so each stem is resolved the
        way every other consumer resolves it rather than assumed to be
        ``<stem>.wav``.
        """
        for stem in STEM_NAMES:
            try:
                path = find_stem_file(folder, stem)
            except Exception:
                continue

            if path is not None:
                stem_ready(stem, path)

    tracker = _JobProgress(progress, eta)

    requested_device = str(device or "").strip().lower()
    device = resolve_device(device)

    """
    The bar is a plan, laid out before anything runs: which stages this
    engine/refinement combination will pass through and how much of the
    bar each one owns. Without the plan, turning refinement off left the
    bar parked at 80% while the last fifth of the job flew by, and the
    hybrid stages were sized identically to a single-model run.

    Stage tuples: (key, lo, hi, step label for the status line).
    """
    steps: list[str] = []

    if engine == ENGINE_HYBRID:
        steps += ["BS-RoFormer", "Demucs", "Fusion"]
    elif engine == ENGINE_DEMUCS:
        steps += ["Demucs"]
    else:
        steps += ["BS-RoFormer"]

    if refine:
        steps += ["Refinement"]

    steps += ["Export"]

    step_count = len(steps)

    def step_prefix(name: str) -> str:
        return f"Step {steps.index(name) + 1}/{step_count}"

    if engine == ENGINE_HYBRID:
        bands = (
            {"roformer": (6.0, 33.0), "demucs": (33.0, 60.0), "fusion": (60.0, 72.0),
             "refine": (72.0, 93.0), "final": (93.0, 95.0)}
            if refine
            else {"roformer": (6.0, 39.0), "demucs": (39.0, 72.0), "fusion": (72.0, 92.0),
                  "final": (92.0, 95.0)}
        )
    else:
        bands = (
            {"model": (6.0, 74.0), "refine": (74.0, 93.0), "final": (93.0, 95.0)}
            if refine
            else {"model": (6.0, 92.0), "final": (92.0, 95.0)}
        )

    # ------------------------------------------------------------ prepare

    duration = _input_duration_seconds(input_path)

    source_blurb = f"{_format_clock(duration)} of audio" if duration else input_path.name

    progress(2.0, f"Preparing {source_blurb} for {engine} on {device}")
    log(f"Input: {input_path}")
    log(f"Output: {output_dir}")
    log(f"Separation engine: {engine}")

    if duration:
        log(f"Source duration: {_format_clock(duration)}")

    # "gpu" is an accepted alias for cuda, so resolving it is not a
    # downgrade - reporting "not available here" for a working GPU sent
    # users troubleshooting a healthy install.
    equivalent_request = {"gpu": "cuda"}.get(requested_device, requested_device)

    if requested_device in ("", "auto"):
        log(f"Device: {device} (auto-selected)")
    elif device != equivalent_request:
        log(f"Device: {device} (requested {requested_device}, which is not available here)")
    else:
        log(f"Device: {device}")

    def make_model_stage(band_key: str, step_name: str, display: str):
        """Adapters wiring one model backend into its band of the plan."""
        lo, hi = bands[band_key]
        tracker.begin(lo, hi)

        prefix = step_prefix(step_name)

        tracker.stage(0.0, f"{prefix} · Loading {display} on {device}")

        def on_progress(percent: float):
            # Separation owns the band above the download sliver, so a
            # first-run download never overlaps it (the bar used to freeze
            # through the first stretch of every model stage after one).
            tracker.stage(0.06 + 0.94 * (percent / 100.0),
                          f"{prefix} · {display} separating ({percent:.0f}%)")

        def on_download(percent: float):
            # A one-time model download: name it, and creep the first
            # sliver of the band instead of impersonating separation.
            tracker.stage(0.06 * (max(0.0, min(100.0, percent)) / 100.0),
                          f"{prefix} · Downloading the {display} model ({percent:.0f}%)")

        return on_progress, on_download, tracker.stage_eta

    if engine == ENGINE_ROFORMER:
        on_progress, on_download, on_eta = make_model_stage("model", "BS-RoFormer", "BS-RoFormer")

        RoFormerBackend(
            model=model,
            device=device,
            log_callback=log,
            progress_callback=on_progress,
            eta_callback=on_eta,
            download_callback=on_download,
            cancellation=cancellation,
        ).separate(
            input_path=input_path,
            output_dir=baseline_dir,
        )

        if not refine:
            # Only without refinement is baseline/ the final location; a
            # refine run publishes from the refinement loop instead.
            announce_folder(baseline_dir)

    elif engine == ENGINE_DEMUCS:
        on_progress, on_download, on_eta = make_model_stage("model", "Demucs", "Demucs")

        DemucsBackend(
            model=demucs_model,
            device=device,
            log_callback=log,
            progress_callback=on_progress,
            eta_callback=on_eta,
            download_callback=on_download,
            cancellation=cancellation,
        ).separate(
            input_path=input_path,
            output_dir=baseline_dir,
        )

        if not refine:
            announce_folder(baseline_dir)

    else:
        roformer_dir = output_dir / "engines" / "roformer"
        demucs_dir = output_dir / "engines" / "demucs"

        on_progress, on_download, on_eta = make_model_stage(
            "roformer", "BS-RoFormer", "Hybrid 1/2: BS-RoFormer")

        RoFormerBackend(
            model=model,
            device=device,
            log_callback=log,
            progress_callback=on_progress,
            eta_callback=on_eta,
            download_callback=on_download,
            cancellation=cancellation,
        ).separate(
            input_path=input_path,
            output_dir=roformer_dir,
        )

        on_progress, on_download, on_eta = make_model_stage(
            "demucs", "Demucs", "Hybrid 2/2: Demucs")

        DemucsBackend(
            model=demucs_model,
            device=device,
            log_callback=log,
            progress_callback=on_progress,
            eta_callback=on_eta,
            download_callback=on_download,
            cancellation=cancellation,
        ).separate(
            input_path=input_path,
            output_dir=demucs_dir,
        )

        fusion_lo, fusion_hi = bands["fusion"]
        tracker.begin(fusion_lo, fusion_hi)

        fusion_prefix = step_prefix("Fusion")

        tracker.stage(0.0, f"{fusion_prefix} · Fusing the two model estimates")
        log(
            "Running StemLab hybrid spectral fusion. "
            "Models are executed sequentially to keep GPU memory usage lower."
        )

        def on_fusion_progress(index: int, total: int, stem: str):
            total = max(1, total)

            if stem:
                # Reported at the START of each stem: the label names the
                # work happening now, the bar carries the work done so far.
                tracker.stage(index / total,
                              f"{fusion_prefix} · Fusing {stem.title()} "
                              f"({index + 1}/{total})")
            else:
                tracker.stage(1.0, f"{fusion_prefix} · Fusion complete")

            tracker.elapsed_eta()

        # Fusion hands each stem over as it lands and keeps nothing itself,
        # so what outlives the stage is exactly what is caught here. Only
        # the stems refinement decodes are worth catching: any other one is
        # byte-copied from the file fusion already wrote, so keeping it
        # would pin a full-length array nothing ever reads.
        keep = reusable_stems()

        fused_stems = {} if refine else None

        def on_fused(stem: str, audio: np.ndarray, sr: int) -> None:
            if stem in keep:
                fused_stems[stem] = (audio, sr)

        fuse_stem_folders(
            roformer_dir=roformer_dir,
            demucs_dir=demucs_dir,
            output_dir=baseline_dir,
            log_callback=log,
            progress_callback=on_fusion_progress,
            # Fusion writes the final files only when nothing refines them
            # afterwards; otherwise refinement is what publishes.
            ready_callback=None if refine else stem_ready,
            # Nothing downstream reads a fused stem when refinement is off,
            # so declining the handover frees each one as it is written.
            fused_callback=on_fused if refine else None,
            normalize=normalize_fused,
        )

    if refine:
        refine_lo, refine_hi = bands["refine"]
        tracker.begin(refine_lo, refine_hi)

        refine_prefix = step_prefix("Refinement")

        tracker.stage(0.0, f"{refine_prefix} · Analyzing drums for kick events")
        log("Running StemLab adaptive refinement...")

        def on_refine_stage(label: str):
            # The pre-loop analysis (drum load, kick detection, reference
            # build) is the slow silent start of refinement; name it.
            tracker.stage(0.02, f"{refine_prefix} · {label}")
            tracker.elapsed_eta()

        def on_refine_progress(index: int, total: int, stem: str):
            total = max(1, total)

            # 10% of the band belongs to the kick analysis before the loop.
            fraction = 0.10 + 0.90 * (index / total)

            if stem:
                tracker.stage(fraction,
                              f"{refine_prefix} · Refining {stem.title()} "
                              f"({index + 1}/{total})")
            else:
                tracker.stage(1.0, f"{refine_prefix} · Refinement complete")

            tracker.elapsed_eta()

        stats_by_stem = refine_stem_folder(
            input_dir=baseline_dir,
            output_dir=final_dir,
            cfg=refinement_config,
            progress_callback=on_refine_progress,
            stage_callback=on_refine_stage,
            ready_callback=stem_ready,
            preloaded=fused_stems,
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
    else:
        log("Refinement disabled; baseline stems are final output.")

    final_lo, final_hi = bands["final"]
    tracker.begin(final_lo, final_hi)

    export_prefix = step_prefix("Export")

    tracker.stage(1.0, f"{export_prefix} · Collecting the finished stems")

    # Backstop: whatever the stages above published, the plugin must know
    # about every stem sitting in the final folder before the job ends.
    # Already-announced stems are dropped by the dedupe in stem_ready.
    announce_folder(final_dir)

    _discard_intermediates(output_dir, baseline_dir, final_dir, log)

    return PipelineResult(
        baseline_dir=baseline_dir,
        final_dir=final_dir,
        engine=engine,
    )
