"""Inventory, fetch, warm up and remove the models StemLab depends on.

Until this module existed nothing in the engine could answer "is this
checkpoint on disk?" before a job started. Every model was discovered the
way a user discovers it: the separation begins, a child process either
downloads several hundred megabytes or fails, and the plugin finds out
afterwards. This is the preflight that was missing, and the thing the
plugin's Model Manager drives.

Three of the five model families are fetched by code we do not own - the
upstream ``bs-roformer-infer`` CLI, Demucs' own torch.hub call, and
audio-separator's in-process registry - so "download" here means invoking
those owners deliberately rather than reimplementing their transfers. Only
Beat This! is ours end to end, and it is the only one carrying a size and
digest we verify (see ``beat_tracking.MODEL_SPECS``).

On ``torch.compile``: the warm-up itself is owned elsewhere and is not
implemented here. What this module settles is everything around it - which
models can be warmed at all, how the plugin asks for one, and how "already
compiled" is decided - so the UI and the backend can land separately. See
``compile_model`` for the seam. Which models can be warmed is not this
module's judgement either: compile_support decides what gets patched, and
only the RoFormer separators are, so the rest report why rather than
offering an action that would do nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

from .analysis_cache import managed_analysis_dir
from .runtime import (
    CancellationToken,
    JobCancelled,
    child_process_env,
    configure_utf8_stdio,
)

ProgressCallback = Callable[[float, str], None]

# Exit code for a run that was asked for something it could not do, and did
# nothing else: compiling switched off, or a model nothing patches. Distinct
# from 0 so the caller does not report "complete" over the reason, and from 1
# so it is not dressed up as a failure - nothing broke.
NOT_APPLICABLE_EXIT_CODE = 3


# ---------------------------------------------------------------- compile


# compile_support arms torch.compile for this model, so warming its kernels
# ahead of a job is worth offering.
COMPILE_SUPPORTED = "supported"

# Nothing compiles this model today. Demucs and the audio-separator models
# run through backends compile_support does not patch: Demucs in its own
# process running upstream's uncompiled code, the adaptive models inside
# audio-separator, which owns the object and serves some architectures
# through ONNX Runtime, where there is no nn.Module to wrap at all.
COMPILE_UNWIRED = "not-wired"


def inductor_cache_dir() -> Path:
    """Where compiled kernels live - compile_support owns this path.

    Asking it rather than deciding here is load-bearing. Every separation is
    a fresh subprocess, so warming only pays if the warm-up writes to the
    directory the separation later reads; a second opinion about where that
    is would fill a cache nothing consults and look exactly like compiling
    having no effect.
    """
    from .compile_support import inductor_cache_dir as resolved

    return resolved()


def _locatable_inductor_cache_dir() -> Path | None:
    """inductor_cache_dir, or None when it cannot be placed.

    It resolves through the managed analysis directory, which falls back to
    the home directory - and that is not always nameable, see _home. Status
    must survive that; compiling is free to fail loudly, so only the reporting
    paths go through here.
    """
    try:
        return inductor_cache_dir()
    except (RuntimeError, OSError):
        return None


def _compile_marker(model_id: str) -> Path | None:
    """Records that a warm-up ran, beside the cache it warmed."""
    cache = _locatable_inductor_cache_dir()

    return cache.parent / f"stemlab_warm_{model_id}.json" if cache is not None else None


# ----------------------------------------------------------- the registry


@dataclass(frozen=True)
class ManagedModel:
    """One weight file the engine can need, and how to get and lose it."""

    id: str
    label: str
    purpose: str
    # Bytes on disk when known. Only Beat This! records this in code; the
    # other two figures come from docs/engine-measurements.md, and the
    # recursive models are not recorded anywhere, so they read 0 and the UI
    # shows the real size once the file exists.
    approx_bytes: int
    compile_support: str
    compile_note: str = ""


MODELS: tuple[ManagedModel, ...] = (
    ManagedModel(
        id="roformer",
        label="BS-RoFormer",
        purpose="Separation - RoFormer and Hybrid engines",
        approx_bytes=699_412_152,
        compile_support=COMPILE_SUPPORTED,
    ),
    ManagedModel(
        id="demucs",
        label="Demucs htdemucs_6s",
        purpose="Separation - Demucs and Hybrid engines",
        approx_bytes=54_996_327,
        compile_support=COMPILE_UNWIRED,
        compile_note="Demucs runs its own uncompiled process",
    ),
    ManagedModel(
        id="beat-this-fast",
        label="Beat This! small",
        purpose="Key & BPM analysis - Fast",
        approx_bytes=8_451_101,
        compile_support=COMPILE_UNWIRED,
        compile_note="Beat This! is not among the patched models",
    ),
    ManagedModel(
        id="beat-this-accurate",
        label="Beat This! final",
        purpose="Key & BPM analysis - Accurate",
        approx_bytes=81_058_141,
        compile_support=COMPILE_UNWIRED,
        compile_note="Beat This! is not among the patched models",
    ),
    ManagedModel(
        id="recursive-vocals",
        label="UVR-BVE lead/backing",
        purpose="Adaptive split - vocal layers",
        approx_bytes=0,
        compile_support=COMPILE_UNWIRED,
        compile_note="Loaded by audio-separator",
    ),
    ManagedModel(
        id="recursive-drums",
        label="MDX23C DrumSep",
        purpose="Adaptive split - drum components",
        approx_bytes=0,
        compile_support=COMPILE_UNWIRED,
        compile_note="Loaded by audio-separator",
    ),
    ManagedModel(
        id="recursive-deverb",
        label="Mel-Band de-reverb",
        purpose="Adaptive split - vocal de-reverb",
        approx_bytes=0,
        compile_support=COMPILE_UNWIRED,
        compile_note="Loaded by audio-separator",
    ),
)

MODELS_BY_ID = {model.id: model for model in MODELS}

_BEAT_THIS_MODES = {"beat-this-fast": "fast", "beat-this-accurate": "accurate"}


# Mirrors of constants the engine defines elsewhere.
#
# Locating a model must work when the engine's optional dependencies are
# missing or broken, because that is precisely when someone opens a Model
# Manager: a half-installed environment is the case it exists to repair.
# Importing beat_tracking costs numpy, and recursive costs the whole adaptive
# DSP stack, so neither can be reached from a status probe.
#
# The duplication is deliberate but not unwatched - test_model_manager asserts
# each of these still equals the engine's own definition whenever that module
# imports cleanly, so a rename there fails a test here rather than quietly
# making every model read as missing.

ROFORMER_MODEL_ID = "roformer-model-bs-roformer-sw-by-jarredou"
DEMUCS_MODEL_NAME = "htdemucs_6s"
DEMUCS_CHECKPOINT = "5c90dfd2-34c22ccb.th"

# demucs.pretrained.get_model tries the HuggingFace hub before the legacy
# checkpoint repo, and adefossez/HTDemucs-6s exists, so on any machine with a
# reachable hub the weights arrive as safetensors in the HF cache and the .th
# above is never written. Looking only for the .th made a successful download
# report as "still is not on disk", and made a working Demucs read as missing.
# Mirrors demucs.hf: f"{DEFAULT_NAMESPACE}/{hf_repo_name(name)}", flattened
# the way huggingface_hub names cache directories.
DEMUCS_HF_DIRECTORY = "models--adefossez--HTDemucs-6s"

BEAT_THIS_FILENAMES = {"beat-this-fast": "small0.ckpt", "beat-this-accurate": "final0.ckpt"}

RECURSIVE_FILENAMES = {
    "recursive-vocals": "UVR-BVE-4B_SN-44100-2.pth",
    "recursive-drums": "MDX23C-DrumSep-aufr33-jarredou.ckpt",
    "recursive-deverb": (
        "dereverb_mel_band_roformer_less_aggressive_anvuew_sdr_18.8050.ckpt"
    ),
}


def _beat_this_directories() -> list[Path]:
    """Mirror of beat_tracking._candidate_model_directories, in the same order.

    Order matters: it is the order the analysis itself searches, so the first
    hit here is the file the analysis would load.
    """
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


def _recursive_model_dir() -> Path | None:
    """Mirror of recursive.default_model_dir."""
    packaged = os.environ.get("STEMLAB_RECURSIVE_MODEL_DIR")
    if packaged:
        return Path(packaged).expanduser()

    local = os.environ.get("LOCALAPPDATA")
    if local:
        return Path(local) / "StemLab" / "Models" / "Recursive"

    home = _home()
    return home / ".stemlab" / "models" / "recursive" if home is not None else None


# --------------------------------------------------------------- locating


def _home() -> Path | None:
    """The user's home, or None where the platform cannot name one.

    Path.home() raises on Windows when neither USERPROFILE nor
    HOMEDRIVE/HOMEPATH is set - a service account, or any process handed a
    stripped environment. Locating must not raise there. This module's whole
    promise is that it answers on a broken install, and "there is no home, so
    nothing of ours is in it" is a perfectly good answer.
    """
    try:
        return Path.home()
    except (RuntimeError, OSError):
        return None


def _roformer_directory() -> Path | None:
    packaged = os.environ.get("BS_ROFORMER_MODELS_PATH")
    if packaged:
        return Path(packaged).expanduser()

    home = _home()
    return home / ".cache" / "bs-roformer-infer" if home is not None else None


def _torch_hub_checkpoints() -> Path | None:
    override = os.environ.get("TORCH_HOME")
    if override:
        return Path(override).expanduser() / "hub" / "checkpoints"

    home = _home()
    return home / ".cache" / "torch" / "hub" / "checkpoints" if home is not None else None


def _largest_checkpoint(directory: Path | None) -> Path | None:
    """The biggest weight file under a directory, or None if there is none."""
    if directory is None or not directory.is_dir():
        return None

    candidates = [
        path
        for path in directory.rglob("*")
        if path.is_file()
        and path.suffix.lower() in {".ckpt", ".pth", ".th", ".onnx", ".bin", ".safetensors"}
    ]

    if not candidates:
        return None

    return max(candidates, key=lambda path: path.stat().st_size)


def locate(model_id: str) -> Path | None:
    """Where this model's weights are, or None when they are not fetched.

    Stdlib only, deliberately: see the note above the mirrored constants.
    """
    if model_id in BEAT_THIS_FILENAMES:
        filename = BEAT_THIS_FILENAMES[model_id]

        for directory in _beat_this_directories():
            candidate = directory / filename
            if candidate.is_file():
                return candidate

        return None

    if model_id == "roformer":
        directory = _roformer_directory()

        return _largest_checkpoint(
            directory / ROFORMER_MODEL_ID if directory is not None else None
        )

    if model_id == "demucs":
        packaged = os.environ.get("STEMLAB_DEMUCS_MODEL_REPO")

        # htdemucs_6s is a bag of models, so several signature files land in
        # the torch hub cache. The one the packaged path names is the marker
        # for the set being present.
        for directory in (
            Path(packaged).expanduser() if packaged else None,
            _torch_hub_checkpoints(),
        ):
            if directory is None:
                continue

            candidate = directory / DEMUCS_CHECKPOINT

            if candidate.is_file():
                return candidate

        # And the HuggingFace copy, which is what a fresh download actually
        # produces - see the note beside DEMUCS_HF_DIRECTORY.
        hub = _huggingface_hub_cache()

        return _largest_checkpoint(hub / DEMUCS_HF_DIRECTORY if hub is not None else None)

    if model_id in RECURSIVE_FILENAMES:
        directory = _recursive_model_dir()

        if directory is None:
            return None

        candidate = directory / RECURSIVE_FILENAMES[model_id]
        return candidate if candidate.is_file() else None

    raise KeyError(f"Unknown model id: {model_id}")


def _compile_state(model_id: str) -> dict[str, object]:
    """What the last successful warm-up recorded, if it still applies."""
    model = MODELS_BY_ID[model_id]

    if model.compile_support != COMPILE_SUPPORTED:
        return {"compiled": False, "reason": model.compile_note}

    marker = _compile_marker(model_id)

    if marker is None or not marker.is_file():
        return {"compiled": False, "reason": ""}

    try:
        recorded = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"compiled": False, "reason": ""}

    # Inductor keys its artifacts on the torch build among other things, so a
    # marker written by a different torch no longer describes a usable cache.
    if recorded.get("torch") != _torch_version():
        return {"compiled": False, "reason": "Compiled with a different torch build"}

    return {
        "compiled": True,
        "reason": "",
        "device": recorded.get("device", ""),
        "seconds": recorded.get("seconds", 0.0),
    }


def _torch_version() -> str:
    """torch's version without importing it when it is not already loaded."""
    # Status runs on every editor open, and a cold torch import costs
    # seconds; device.py avoids it the same way for the same reason.
    module = sys.modules.get("torch")
    if module is not None:
        return str(getattr(module, "__version__", ""))

    try:
        from importlib.metadata import version

        return version("torch")
    except Exception:
        return ""


# ----------------------------------------------------------------- caches


@dataclass(frozen=True)
class ManagedCache:
    id: str
    label: str
    path: Path
    # Set when deleting costs the user something they cannot re-download.
    warning: str = ""


def _huggingface_cache() -> Path | None:
    override = os.environ.get("HF_HOME")
    if override:
        return Path(override).expanduser()

    home = _home()
    return home / ".cache" / "huggingface" if home is not None else None


def _huggingface_hub_cache() -> Path | None:
    """Where huggingface_hub stores repositories, which is HF_HOME/hub.

    HF_HUB_CACHE wins if set, matching huggingface_hub's own precedence.
    """
    override = os.environ.get("HF_HUB_CACHE")
    if override:
        return Path(override).expanduser()

    home = _huggingface_cache()
    return home / "hub" if home is not None else None


def _analysis_dir() -> Path | None:
    """StemLab's analysis directory, or None when it cannot be placed.

    managed_analysis_dir falls back to the home directory, which is not
    always nameable - see _home. A cache we cannot locate is one we can
    neither size nor clear, so it is left out rather than reported wrongly.
    """
    try:
        return managed_analysis_dir()
    except (RuntimeError, OSError):
        return None


def caches() -> tuple[ManagedCache, ...]:
    analysis = _analysis_dir()
    roformer = _roformer_directory()

    # Anything whose path cannot be resolved is omitted: the interface offers
    # a size and a Clear for every row, and it can honestly offer neither.
    candidates = (
        ("compile", "Compiled kernels", _locatable_inductor_cache_dir(), ""),
        ("huggingface", "HuggingFace hub", _huggingface_cache(), ""),
        ("torch-hub", "Torch hub", _torch_hub_checkpoints(), ""),
        (
            "bs-roformer",
            "BS-RoFormer",
            roformer / ROFORMER_MODEL_ID if roformer is not None else None,
            "",
        ),
        (
            "analysis",
            "Key & BPM results",
            analysis / "analysis.sqlite3" if analysis is not None else None,
            "Also removes saved BPM, key and meter corrections",
        ),
        (
            "device-probe",
            "Device probe",
            analysis / "device_probe.json" if analysis is not None else None,
            "",
        ),
        (
            "midi-staging",
            "MIDI drag staging",
            analysis / "MidiDrag" if analysis is not None else None,
            "",
        ),
    )

    return tuple(
        ManagedCache(identifier, label, path, warning=warning)
        for identifier, label, path, warning in candidates
        if path is not None
    )


def _directory_bytes(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size

    if not path.is_dir():
        return 0

    total = 0
    for entry in path.rglob("*"):
        try:
            if entry.is_file():
                total += entry.stat().st_size
        except OSError:
            # A file that vanished or that we cannot stat is not worth
            # failing a size report over.
            continue
    return total


# ----------------------------------------------------------------- status


def _best_device() -> str:
    """The device a job would run on, for asking whether it can be compiled."""
    try:
        from .device import pick_best_device

        return pick_best_device()
    except Exception:
        return "cpu"


def _compile_environment(probe: bool) -> tuple[bool, bool, str]:
    """Whether compiling was asked for, whether this machine can, and why not.

    compile_support decides both, and answering the second means importing
    torch, which costs seconds. Status runs on every editor open, so that is
    paid only when ``probe`` asks for it - which the plugin does when someone
    turns compiling on, not on every refresh.
    """
    try:
        from . import compile_support
    except Exception as exc:
        return False, False, f"compile support is unavailable: {exc}"

    requested = compile_support.compile_requested()

    if not requested:
        return False, False, "Set STEMLAB_TORCH_COMPILE=1 to compile separations"

    if not probe and "torch" not in sys.modules:
        # Unprobed, and deliberately not dressed up as a finding: saying so
        # would put "not been probed" in front of every user who turned
        # compiling on. The caller that needs the answer asks for it.
        return True, True, ""

    supported, reason = compile_support.compile_support_status(_best_device())
    return True, supported, reason


def status(*, probe_compile: bool = False) -> dict[str, object]:
    """Everything the plugin needs to draw the Model Manager, as plain data."""
    models = []

    for model in MODELS:
        path = locate(model.id)
        compile_state = _compile_state(model.id)

        models.append(
            {
                "id": model.id,
                "label": model.label,
                "purpose": model.purpose,
                "present": path is not None,
                "path": str(path) if path is not None else "",
                "bytes": path.stat().st_size if path is not None else 0,
                "approxBytes": model.approx_bytes,
                "compileSupport": model.compile_support,
                **compile_state,
            }
        )

    cache_entries = [
        {
            "id": cache.id,
            "label": cache.label,
            "path": str(cache.path),
            "bytes": _directory_bytes(cache.path),
            "warning": cache.warning,
        }
        for cache in caches()
    ]

    missing = [entry for entry in models if not entry["present"]]
    compilable = [
        entry for entry in models if entry["compileSupport"] == COMPILE_SUPPORTED
    ]

    requested, supported, support_reason = _compile_environment(probe_compile)

    return {
        "models": models,
        "caches": cache_entries,
        # Straight from compile_support, so the Model Manager can say why
        # compiling is off - an unset opt-in and a missing C++ compiler look
        # identical from the outside and need very different advice.
        "compileRequested": requested,
        "compileSupported": supported,
        "compileReason": support_reason,
        # The plugin's auto-show condition, decided here so the rule lives in
        # one place rather than being re-derived in C++.
        "anyModelMissing": bool(missing),
        "anyCompilePending": bool(requested and supported)
        and any(entry["present"] and not entry["compiled"] for entry in compilable),
        "offlineForced": os.environ.get("HF_HUB_OFFLINE") == "1",
        "torch": _torch_version(),
    }


# -------------------------------------------------------------- downloads


def _run_child(
    command: Sequence[str],
    label: str,
    progress: ProgressCallback | None,
    cancellation: CancellationToken | None,
    span: tuple[float, float],
) -> None:
    """Run a downloader and map whatever it prints onto our progress span.

    The owners of these transfers report through tqdm, and runtime.py already
    knows how to recognise a byte-rate bar. Rather than re-parse it here the
    span simply advances on activity, because a download that is moving is
    the only thing the user needs to see and an exact percentage from three
    different bar formats is not worth the fragility.

    The last few lines are kept even so. They are not shown while the transfer
    is healthy - a progress bar redrawn a thousand times is noise - but when
    the child dies they are the only account of why, and an exit code on its
    own has sent more than one person reading source code.
    """
    start, end = span

    if progress:
        progress(start, label)

    environment = child_process_env()

    # A packaged release sets HF_HUB_OFFLINE=1 at import time so ordinary
    # jobs never reach the network. Downloading is the one operation that
    # must, and it is explicit rather than incidental, so the switch is
    # lifted for exactly this child.
    environment.pop("HF_HUB_OFFLINE", None)
    environment.pop("TRANSFORMERS_OFFLINE", None)

    process = subprocess.Popen(
        list(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
    )

    fraction = start
    tail: deque[str] = deque(maxlen=5)
    try:
        assert process.stdout is not None
        for raw in process.stdout:
            if cancellation is not None and cancellation.requested:
                process.terminate()
                raise JobCancelled("Model download cancelled")

            text = raw.decode("utf-8", errors="replace").rstrip()
            if not text:
                continue

            tail.append(text)

            # Creep towards the end of the span on every line the child
            # emits, never reaching it: the span closes when the child does.
            fraction = min(end - 0.01, fraction + (end - start) * 0.02)
            if progress:
                progress(fraction, label)
    finally:
        process.stdout.close() if process.stdout else None
        process.wait()

    if process.returncode != 0:
        said = " | ".join(tail) if tail else "no output"
        raise RuntimeError(f"{label} failed with exit code {process.returncode}: {said}")

    if progress:
        progress(end, label)


def bs_roformer_download_command(*arguments: str) -> list[str]:
    """The command that runs upstream's downloader on this machine.

    Never the pip launcher beside the interpreter, even though it is right
    there and looks runnable: a shipped Engine was built somewhere else, so
    the interpreter baked into that launcher does not exist here and exec'ing
    it reports the launcher itself as missing. ``console_entry`` explains the
    failure; this goes through the module that sidesteps it.
    """
    return [sys.executable, "-m", "stemlab.bs_roformer_download_cli", *arguments]


def download(
    model_id: str,
    *,
    progress: ProgressCallback | None = None,
    cancellation: CancellationToken | None = None,
    span: tuple[float, float] = (0.0, 1.0),
) -> Path:
    """Fetch one model, by asking whoever owns it to fetch it."""
    model = MODELS_BY_ID[model_id]
    start, end = span

    if model_id in _BEAT_THIS_MODES:
        from . import beat_tracking

        # The only transfer we own, and the only one whose bytes are checked
        # against a recorded length and digest before they count.
        def relay(fraction: float, _stage: str = "") -> None:
            if progress:
                progress(start + (end - start) * fraction, f"Downloading {model.label}")

        return beat_tracking.download_packaged_model(
            _BEAT_THIS_MODES[model_id], progress=relay, cancellation=cancellation
        )

    if model_id == "roformer":
        _run_child(
            # --model, not a bare slug: upstream's parser takes the model as a
            # repeatable option and rejects a positional with exit code 2.
            bs_roformer_download_command("--model", ROFORMER_MODEL_ID),
            f"Downloading {model.label}",
            progress,
            cancellation,
            span,
        )

    elif model_id == "demucs":
        _run_child(
            [
                sys.executable,
                "-c",
                "import sys;from demucs.pretrained import get_model;get_model(sys.argv[1])",
                DEMUCS_MODEL_NAME,
            ],
            f"Downloading {model.label}",
            progress,
            cancellation,
            span,
        )

    elif model_id in RECURSIVE_FILENAMES:
        directory = _recursive_model_dir()
        directory.mkdir(parents=True, exist_ok=True)

        # audio-separator downloads inside load_model, in-process. Doing it in
        # a child keeps torch and onnxruntime out of this one, which is what
        # lets --status stay cheap enough to run on every editor open.
        _run_child(
            [
                sys.executable,
                "-c",
                (
                    "import sys;"
                    "from audio_separator.separator import Separator;"
                    "s=Separator(model_file_dir=sys.argv[1]);"
                    "s.load_model(model_filename=sys.argv[2])"
                ),
                str(directory),
                RECURSIVE_FILENAMES[model_id],
            ],
            f"Downloading {model.label}",
            progress,
            cancellation,
            span,
        )

    else:
        raise KeyError(f"Unknown model id: {model_id}")

    located = locate(model_id)

    if located is None:
        raise RuntimeError(f"{model.label} still is not on disk after downloading it")

    return located


# --------------------------------------------------------------- compiling


class CompileUnavailable(RuntimeError):
    """Raised when a warm-up is asked for and no backend provides one."""


def compile_model(
    model_id: str,
    *,
    device: str = "auto",
    progress: ProgressCallback | None = None,
    cancellation: CancellationToken | None = None,
    span: tuple[float, float] = (0.0, 1.0),
) -> float:
    """Warm torch.compile for one model, returning the seconds it took.

    THE WARM-UP ITSELF IS DELIBERATELY NOT IMPLEMENTED HERE. It is owned
    elsewhere; this is the seam it plugs into, and the surrounding contract -
    which models can be warmed, how the plugin asks, and how "already
    compiled" is decided - is settled so that the UI and the implementation
    can land independently.

    ``compile_support`` added the compiling itself and deliberately left
    warming out of scope, so the cache is only ever populated by real jobs
    paying the cold cost - measured there at 114.8 s against 27.4 s warm.
    Warming is what closes that, and it is the piece still missing.

    To provide it, define ``stemlab.model_compile`` exposing:

        warm_up(model_id, device, progress, cancellation) -> float

    returning the elapsed seconds. It must write into ``inductor_cache_dir()``
    - compile_support's own path, which is why this module asks rather than
    deciding - and warm the shapes production runs, since an inductor entry
    is keyed on the graph and a differently shaped dummy fills the cache with
    something no real run asks for.

    On success this writes the marker ``_compile_state`` reads, so a backend
    only has to do the warming and does not have to know how state is
    recorded. The marker carries the torch version because inductor artifacts
    do not survive a torch upgrade, and ``_compile_state`` retires the marker
    when that version no longer matches.
    """
    model = MODELS_BY_ID[model_id]
    start, end = span

    if model.compile_support != COMPILE_SUPPORTED:
        raise CompileUnavailable(f"{model.label} cannot be compiled: {model.compile_note}")

    if locate(model_id) is None:
        raise RuntimeError(f"{model.label} is not downloaded yet")

    try:
        from . import model_compile  # type: ignore[attr-defined]
    except ImportError as exc:
        raise CompileUnavailable(
            "No compile backend is installed: stemlab.model_compile is missing"
        ) from exc

    inductor_cache_dir().mkdir(parents=True, exist_ok=True)

    if progress:
        progress(start, f"Compiling {model.label}")

    began = time.monotonic()

    try:
        elapsed = model_compile.warm_up(
            model_id,
            device=device,
            progress=(
                (lambda fraction, stage: progress(start + (end - start) * fraction, stage))
                if progress
                else None
            ),
            cancellation=cancellation,
        )
    except model_compile.WarmUpUnavailable as exc:
        # A state, not a failure: compiling switched off, or a machine with no
        # toolchain. Reported the same way an unpatched model is, so a run
        # whose downloads all succeeded does not come back red.
        raise CompileUnavailable(str(exc)) from exc

    if not isinstance(elapsed, (int, float)) or elapsed <= 0.0:
        elapsed = time.monotonic() - began

    marker = _compile_marker(model_id)

    if marker is None:
        raise RuntimeError("There is nowhere to record the warm-up on this machine")

    marker.write_text(
        json.dumps(
            {
                "torch": _torch_version(),
                "device": device,
                "seconds": round(float(elapsed), 2),
                "model": str(locate(model_id) or ""),
            },
            indent=1,
        ),
        encoding="utf-8",
    )

    if progress:
        progress(end, f"Compiled {model.label}")

    return float(elapsed)


# ---------------------------------------------------------------- removal


def delete_model(model_id: str) -> int:
    """Remove one model's weights, returning the bytes reclaimed."""
    path = locate(model_id)

    if path is None:
        return 0

    freed = path.stat().st_size
    path.unlink()

    # Beat This! records a verification sidecar beside the checkpoint so a
    # re-hash can be skipped; leaving it behind would describe a file that no
    # longer exists.
    sidecar = path.with_suffix(path.suffix + ".verified.json")
    if sidecar.is_file():
        freed += sidecar.stat().st_size
        sidecar.unlink()

    marker = _compile_marker(model_id)
    if marker is not None and marker.is_file():
        marker.unlink()

    return freed


def delete_cache(cache_id: str) -> int:
    """Remove one cache, returning the bytes reclaimed."""
    entry = next((cache for cache in caches() if cache.id == cache_id), None)

    if entry is None:
        raise KeyError(f"Unknown cache id: {cache_id}")

    freed = _directory_bytes(entry.path)

    if entry.path.is_file():
        entry.path.unlink()
    elif entry.path.is_dir():
        shutil.rmtree(entry.path, ignore_errors=True)

    return freed


# --------------------------------------------------------------------- CLI


def _emit_progress(fraction: float, stage: str) -> None:
    print(f"STEMLAB_PROGRESS {max(0.0, min(100.0, fraction * 100.0)):.0f} {stage}", flush=True)


def _spans(count: int) -> list[tuple[float, float]]:
    """Split 0..1 into one contiguous span per item of work."""
    if count <= 0:
        return []
    step = 1.0 / count
    return [(index * step, (index + 1) * step) for index in range(count)]


def _resolve_model_ids(requested: Sequence[str], *, missing_only: bool) -> list[str]:
    if missing_only:
        return [model.id for model in MODELS if locate(model.id) is None]

    for model_id in requested:
        if model_id not in MODELS_BY_ID:
            raise SystemExit(f"Unknown model id: {model_id}")

    return list(requested)


def _human_bytes(value: int) -> str:
    if value <= 0:
        return "0 B"
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{value:.1f} GB"


def _main() -> None:
    configure_utf8_stdio()

    parser = argparse.ArgumentParser(
        prog="stemlab-model-manager",
        description="Inventory, fetch and remove the models StemLab depends on.",
        epilog=(
            "--status writes a JSON inventory and prints its path as "
            "STEMLAB_MODEL_INVENTORY. Every other mode reports STEMLAB_PROGRESS "
            "while it works and STEMLAB_ERROR on failure."
        ),
    )

    parser.add_argument("--status", action="store_true", help="report what is on disk")
    parser.add_argument(
        "--probe-compile",
        action="store_true",
        help="also ask whether this machine can compile, which imports torch",
    )
    parser.add_argument("--output", help="where --status writes its JSON")
    parser.add_argument("--download", action="append", default=[], metavar="ID")
    parser.add_argument("--download-missing", action="store_true")
    parser.add_argument("--compile", action="append", default=[], metavar="ID")
    parser.add_argument("--delete-model", action="append", default=[], metavar="ID")
    parser.add_argument("--delete-cache", action="append", default=[], metavar="ID")
    parser.add_argument("--device", default="auto")
    parser.add_argument("--cancel-file", help="sentinel whose existence cancels this run")

    args = parser.parse_args()

    token = CancellationToken(Path(args.cancel_file) if args.cancel_file else None)

    if args.status:
        payload = status(probe_compile=args.probe_compile)
        text = json.dumps(payload, indent=1)

        if args.output:
            destination = Path(args.output).expanduser()
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(text, encoding="utf-8")
            print(f"STEMLAB_MODEL_INVENTORY {destination}", flush=True)
        else:
            print(text, flush=True)
        return

    downloads = _resolve_model_ids(args.download, missing_only=args.download_missing)
    compiles = _resolve_model_ids(args.compile, missing_only=False)
    removals = _resolve_model_ids(args.delete_model, missing_only=False)

    work = len(downloads) + len(compiles) + len(removals) + len(args.delete_cache)

    if work == 0:
        parser.error("nothing to do: pass --status, --download, --compile, --delete-model "
                     "or --delete-cache")

    spans = _spans(work)
    index = 0
    freed = 0
    refused = 0

    for model_id in downloads:
        token.raise_if_cancelled()
        download(model_id, progress=_emit_progress, cancellation=token, span=spans[index])
        index += 1

    for model_id in compiles:
        token.raise_if_cancelled()
        try:
            compile_model(
                model_id,
                device=args.device,
                progress=_emit_progress,
                cancellation=token,
                span=spans[index],
            )
        except CompileUnavailable as exc:
            # Not a failure of the run: the user asked for something this
            # build cannot do, and saying so beats a red error on a job whose
            # downloads all succeeded.
            print(f"STEMLAB_PROGRESS {spans[index][1] * 100:.0f} {exc}", flush=True)
            refused += 1
        index += 1

    for model_id in removals:
        token.raise_if_cancelled()
        freed += delete_model(model_id)
        _emit_progress(spans[index][1], f"Removed {MODELS_BY_ID[model_id].label}")
        index += 1

    for cache_id in args.delete_cache:
        token.raise_if_cancelled()
        freed += delete_cache(cache_id)
        _emit_progress(spans[index][1], f"Cleared {cache_id}")
        index += 1

    if refused and refused == work:
        # Everything asked for was refused, so the reason printed above is the
        # whole outcome. Leaving it as the last word beats overwriting it with
        # a completion the user would reasonably read as "it compiled".
        raise SystemExit(NOT_APPLICABLE_EXIT_CODE)

    if freed > 0:
        print(f"STEMLAB_PROGRESS 100 Reclaimed {_human_bytes(freed)}", flush=True)
    else:
        _emit_progress(1.0, "Done")


def main() -> None:
    """Entry point. The wrapping lives here, not under __main__.

    A console-script entry point calls this directly and never executes the
    __main__ block, so anything below it would be skipped in an installed
    environment - the same reason recursive_job and plugin_job wrap here.
    """
    try:
        _main()
    except JobCancelled:
        # A cancel is a user action, not a failure: report it as such and
        # leave the "Failed - ..." status for real errors.
        print("STEMLAB_CANCELLED", flush=True)
        raise SystemExit(130) from None
    except Exception as exc:
        # The plugin reads the failure reason from this line, and the
        # traceback the re-raise prints is what "copy diagnostics" collects.
        print(f"STEMLAB_ERROR {exc}", flush=True)
        raise


if __name__ == "__main__":
    main()
