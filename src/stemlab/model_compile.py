"""Fill the compiled-kernel cache before a real separation needs it.

``compile_support`` compiles during a job, which means somebody pays for it.
Measured on CPU, the first forward pass costs 114.8 s against a cold cache
and 27.4 s against a warm one, where eager is 9.5 s; steady state is 4.09 s
compiled against 6.09 s eager. So the first compiled separation is a large
loss and every one after it is a win, and the whole point of warming is to
move that loss somewhere the user chose to spend it.

The warm-up runs a real separation over a few seconds of synthetic audio,
rather than building a model and feeding it a plausible tensor. That is
deliberate and is the load-bearing decision in this module: an inductor entry
is keyed on the graph, so a warm-up whose shapes differ from production fills
the cache with kernels no job ever asks for and looks exactly like compiling
having no effect. Driving the real path cannot get the shapes wrong, because
they are not ours to choose.
"""

from __future__ import annotations

import math
import os
import tempfile
import time
import wave
from array import array
from pathlib import Path
from typing import Callable

from .compile_support import compile_requested, compile_support_status, inductor_cache_dir
from .device import pick_best_device, resolve_torch_device
from .pretrained import DEFAULT_MODEL, build_roformer_command
from .runtime import CancellationToken, run_progress_process

ProgressCallback = Callable[[float, str], None]


class WarmUpUnavailable(RuntimeError):
    """Warming cannot help here, which is a state rather than a failure.

    Raised when there is no cache to warm for this model, when compiling is
    switched off, or when the machine has no toolchain for it. The caller
    turns these into something the interface can say calmly; a warm-up that
    was attempted and broke raises an ordinary error instead.
    """

# Only the RoFormer separators are patched by compile_support, so they are the
# only thing there is a cache to warm.
WARMABLE_MODELS = frozenset({"roformer"})

# BS-RoFormer's band splits are fixed in bins, so it only ever sees 44.1 kHz.
WARM_UP_RATE = 44_100

# Long enough to cross a chunk boundary more than once. Shapes settle after
# the first two passes and a shorter final chunk costs one more small
# compile, so a warm-up that stopped inside the first chunk would leave the
# real job to compile the very cases this exists to precompile.
WARM_UP_SECONDS = 25


def _write_warm_up_audio(path: Path) -> None:
    """Write a short stereo WAV for the warm-up to separate.

    Deliberately stdlib-only. Only the tensor shapes matter for what inductor
    caches, and a warm-up that needed numpy would be one more thing to fail on
    the half-built installs this feature exists to repair.

    Not silence: a separator fed digital black can take an early exit, and a
    graph that was never traced is a graph the real job still has to compile.
    Two detuned tones plus a slow sweep give every band something to do.
    """
    frames = WARM_UP_RATE * WARM_UP_SECONDS
    samples = array("h")

    for index in range(frames):
        t = index / WARM_UP_RATE

        # A sweep so the upper bands are excited too, and an offset between
        # the channels so a stereo model does not see one duplicated side.
        sweep = math.sin(2.0 * math.pi * (200.0 + 40.0 * t) * t)
        left = 0.30 * math.sin(2.0 * math.pi * 110.0 * t) + 0.20 * sweep
        right = 0.30 * math.sin(2.0 * math.pi * 110.5 * t) + 0.20 * sweep

        samples.append(int(max(-1.0, min(1.0, left)) * 32000))
        samples.append(int(max(-1.0, min(1.0, right)) * 32000))

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(WARM_UP_RATE)
        handle.writeframes(samples.tobytes())


def _arm_child_environment() -> None:
    """Set what the warm-up child needs, in this process's own environment.

    run_progress_process builds the child's environment from this one, so
    exporting here is how the settings reach it without changing a helper
    every separation depends on. Safe because warming runs inside the
    short-lived model-manager worker, which does nothing else afterwards.
    """
    # The child does the compiling, so it is told explicitly rather than left
    # to inherit an opt-in that may have come from a UI toggle.
    os.environ["STEMLAB_TORCH_COMPILE"] = "1"

    # The line that decides whether any of this was worth doing. Every
    # separation is a fresh process, so warming only pays if it writes where
    # the job later reads; compile_support owns that path and is asked for it
    # rather than second-guessed.
    os.environ["TORCHINDUCTOR_CACHE_DIR"] = str(inductor_cache_dir())


def warm_up(
    model_id: str,
    *,
    device: str = "auto",
    progress: ProgressCallback | None = None,
    cancellation: CancellationToken | None = None,
) -> float:
    """Precompile one model's kernels, returning the seconds it took.

    This is the backend ``model_manager.compile_model`` looks for. It reports
    through ``progress`` as a fraction with a stage label, and honours
    ``cancellation`` between phases and inside the child.
    """
    if model_id not in WARMABLE_MODELS:
        raise WarmUpUnavailable(f"There is no compiled cache to warm for {model_id}")

    if not compile_requested():
        raise WarmUpUnavailable(
            "Compiling is off - set STEMLAB_TORCH_COMPILE=1, or separations "
            "will not use what this builds"
        )

    # resolve_torch_device does not understand "auto" - it hands anything it
    # does not recognise straight back, so asking it would test the literal
    # string "auto" for an inductor backend and always conclude there is
    # none. pick_best_device is what "auto" means.
    requested = (device or "auto").strip().lower()
    resolved = pick_best_device() if requested in ("", "auto") else resolve_torch_device(requested)

    supported, reason = compile_support_status(resolved)

    if not supported:
        raise WarmUpUnavailable(f"This machine cannot compile: {reason}")

    def report(fraction: float, stage: str) -> None:
        if progress:
            progress(max(0.0, min(1.0, fraction)), stage)

    if cancellation:
        cancellation.raise_if_cancelled()

    cache = inductor_cache_dir()
    cache.mkdir(parents=True, exist_ok=True)

    began = time.monotonic()
    report(0.0, "Preparing warm-up audio")

    with tempfile.TemporaryDirectory(prefix="stemlab_warmup_") as directory:
        root = Path(directory)
        staging = root / "input"
        output = root / "output"
        staging.mkdir()
        output.mkdir()

        _write_warm_up_audio(staging / "warmup.wav")

        if cancellation:
            cancellation.raise_if_cancelled()

        _arm_child_environment()

        report(0.05, "Compiling kernels - the first pass is slow")

        # Literally the separation's own builder, so there is no chance of
        # warming a graph the real job will not reuse.
        command = build_roformer_command(staging, output, resolved, DEFAULT_MODEL)

        lines: list[str] = []

        def log(message: str) -> None:
            lines.append(message)

        # The child's own separation percentage is the honest signal here: the
        # compile happens inside its first pass, so its progress is the warm
        # -up's progress, mapped into what is left after preparing the audio.
        # run_progress_process reports 0 to 100, not 0 to 1: read as a
        # fraction the bar reached the end of the span at the child's first
        # percent and sat there for the whole compile.
        return_code = run_progress_process(
            command,
            log,
            lambda percent: report(0.05 + 0.95 * (percent / 100.0), "Compiling kernels"),
            log_progress_lines=False,
            cancellation=cancellation,
        )

    if return_code != 0:
        tail = " | ".join(lines[-3:]) if lines else "no output"
        raise RuntimeError(f"Warm-up separation failed ({return_code}): {tail}")

    elapsed = time.monotonic() - began
    report(1.0, "Kernels cached")

    return elapsed
