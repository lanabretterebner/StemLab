"""Opt-in ``torch.compile`` acceleration for the BS-RoFormer separator.

BS-RoFormer spends roughly 86% of a forward pass in dense matmuls spread over
many small layers, so inductor's fusion is worth real time - but only where the
toolchain it needs actually exists. Three things make this opt-in rather than
automatic:

* Inductor's CPU backend shells out to a host C++ compiler, and a missing one
  raises ``InductorError`` instead of degrading to eager. The portable Engine
  bundle is exactly the install with no toolchain.
* The GPU backends need Triton, which PyTorch ships on Linux but not in the
  Windows CUDA wheels, and which does not exist at all for ROCm on Windows.
* Compiling costs roughly two minutes the first time. That is only worth
  paying against a warm on-disk cache, which is populated elsewhere.

So nothing here compiles unless ``STEMLAB_TORCH_COMPILE`` asks for it, and even
then every failure path falls back to the eager module rather than failing the
separation.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Callable

# The separator classes worth compiling. Both are transformer stacks over
# band-split spectrograms; everything else in the runtime is either too small
# to matter or too short-lived to amortise a compile.
_TARGET_CLASS_NAMES = ("BSRoformer", "MelBandRoformer")

# Set on a class once patched, so exporting the same class from two modules
# cannot wrap ``forward`` twice.
_PATCH_MARKER = "_stemlab_compile_patched"

_TRUE_VALUES = {"1", "true", "yes", "on"}
_FALSE_VALUES = {"0", "false", "no", "off", ""}


def compile_requested() -> bool:
    """Whether the operator asked for compiled inference.

    Off by default: the win depends on a warm inductor cache, and the failure
    modes above are worse than the speedup is good on an install that cannot
    support it.
    """
    return os.environ.get("STEMLAB_TORCH_COMPILE", "").strip().lower() in _TRUE_VALUES


def inductor_cache_dir() -> Path:
    """Where inductor keeps generated kernels between runs.

    Every separation is a fresh subprocess, so a cache that does not outlive
    the process makes compiling a guaranteed loss. This is a stable path under
    StemLab's own managed directory precisely so a warm-up step can populate
    it ahead of the first real job.
    """
    override = os.environ.get("STEMLAB_TORCH_COMPILE_CACHE", "").strip()
    if override:
        return Path(override).expanduser()

    # Imported lazily to keep sqlite3 and the runtime helpers off this path
    # when compiling was never requested.
    from .paths import analysis_dir

    return analysis_dir() / "torchinductor"


# Which managed model a compiled class belongs to. Both transformer classes
# live behind the "roformer" entry in the model manager's registry, and both
# are only ever armed from bs_roformer_cli - the adaptive-split models run
# through audio-separator in a process that never calls arm_torch_compile, so
# nothing here can claim credit for a model it did not compile.
_MODEL_IDS_BY_CLASS = {"BSRoformer": "roformer", "MelBandRoformer": "roformer"}


def torch_build_id() -> str:
    """torch's version, spelled the same way in every process.

    Package metadata rather than ``torch.__version__``, deliberately, and
    metadata first even where torch is already imported. This string is
    written into the compile marker by a process that has torch loaded and
    compared against by the model manager's status probe, which refuses to
    import torch because it runs on every editor open. Reading a different
    source in each lets the two disagree over a build's local suffix, and a
    marker that fails that comparison is retired silently - a model that was
    compiled reports itself as not compiled, with nothing saying why.
    """
    try:
        from importlib.metadata import version

        return version("torch")
    except Exception:
        # No metadata to read - an unpacked tree, a vendored build. The
        # imported module is then the only answer available, and every
        # process falls back to it identically.
        module = sys.modules.get("torch")

        return str(getattr(module, "__version__", "")) if module is not None else ""


def compile_marker_path(model_id: str) -> Path:
    """Where the record that a model's kernels are cached lives.

    Beside the cache rather than inside it, so clearing the kernels and
    clearing the record stay separate decisions - the model manager clears
    both together when it empties the cache.
    """
    return inductor_cache_dir().parent / f"stemlab_warm_{model_id}.json"


def record_compiled(model_id: str, device: str, source: str) -> None:
    """Record that this model's kernels are cached, if they are not already.

    Compiling inside a real separation warms the same cache a deliberate
    warm-up would, so it is worth the same record: without this, a model
    could be compiled on every job and still report itself as never compiled,
    because only the warm-up wrote the marker.

    Best effort throughout. Failing to record a fact about a cache must never
    be the reason a separation fails, so every error here is swallowed.
    """
    import json

    try:
        marker = compile_marker_path(model_id)
        payload = {"torch": torch_build_id(), "device": device, "source": source}

        if marker.is_file():
            try:
                recorded = json.loads(marker.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                recorded = {}

            # Already true and already said so. Rewriting it on every job
            # would be a pointless write per separation.
            if recorded.get("torch") == payload["torch"]:
                return

        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps(payload, indent=1), encoding="utf-8")
    except Exception:
        return


def _cxx_compiler_available() -> bool:
    """Whether inductor's CPU backend can find a host C++ compiler."""
    try:
        # torch's own resolver is the authority on what it will accept. It is
        # private, so a failure here falls through to the PATH probe below
        # rather than deciding the answer.
        from torch._inductor import cpp_builder

        return bool(cpp_builder.get_cpp_compiler())
    except Exception:
        pass

    candidates = ("cl",) if sys.platform == "win32" else ("g++", "clang++")
    return any(shutil.which(name) for name in candidates)


def _triton_available() -> bool:
    """Whether inductor can generate GPU kernels.

    A spec lookup rather than an import: importing Triton costs real time on a
    path that may be about to decide against compiling anyway.
    """
    import importlib.util

    return importlib.util.find_spec("triton") is not None


def compile_support_status(device: str) -> tuple[bool, str]:
    """Return whether ``device`` can be compiled, and why not when it cannot.

    The reason is written to be read in a log by someone wondering why their
    separation was not compiled, so it names the missing piece.
    """
    kind = (device or "cpu").strip().lower().split(":")[0]

    if kind not in {"cpu", "cuda", "xpu"}:
        # Notably DirectML, which lives on torch's ``privateuseone`` slot and
        # has no inductor backend registered at all.
        return False, f"{kind!r} has no TorchInductor backend"

    try:
        import torch
    except Exception:
        return False, "torch is not importable"

    if kind == "cuda":
        # ROCm answers through the CUDA API, so torch.version.hip is what
        # separates an AMD card from an NVIDIA one here.
        if getattr(torch.version, "hip", None) and sys.platform == "win32":
            return False, "ROCm on Windows has no Triton backend"

    if kind in {"cuda", "xpu"} and not _triton_available():
        return False, "Triton is not installed (PyTorch does not ship it on Windows)"

    if kind == "cpu" and not _cxx_compiler_available():
        return False, "no host C++ compiler, which TorchInductor's CPU backend requires"

    return True, f"{kind} supports TorchInductor"


def _one_line(exc: BaseException, limit: int = 160) -> str:
    """Flatten an exception for a line-oriented log stream.

    The parent parses this subprocess's output line by line and reads a
    percentage next to a byte rate as download progress. InductorError text is
    long, multi-line, and quotes generated source, so it is collapsed, stripped
    of percent signs, and truncated before it reaches the log.
    """
    text = " ".join(str(exc).split()).replace("%", " pct")
    return text[:limit] + "..." if len(text) > limit else text


def _wrap_forward(cls: type, log: Callable[[str], None]) -> None:
    """Replace ``cls.forward`` with one that compiles on first use.

    Deferred to the first call for two reasons: the gate can then read the
    real device off the input instead of guessing it from argv, and a model
    that is constructed but never run costs nothing.
    """
    import torch

    original = cls.forward

    def forward(self, *args, **kwargs):  # type: ignore[no-untyped-def]
        state = getattr(self, "_stemlab_compile_state", None)

        if state is None:
            state = {"fn": None, "active": False, "recorded": False, "device": "cpu"}
            self._stemlab_compile_state = state

            device = "cpu"
            for value in (*args, *kwargs.values()):
                if isinstance(value, torch.Tensor):
                    device = value.device.type
                    break

            state["device"] = device

            supported, reason = compile_support_status(device)
            if not supported:
                log(f"Running {cls.__name__} without torch.compile: {reason}.")
            else:
                try:
                    state["fn"] = torch.compile(original.__get__(self, cls))
                    state["active"] = True
                    log(
                        f"Compiling {cls.__name__} for {device} - the first pass is slow "
                        "while kernels are built or loaded from cache."
                    )
                except Exception as exc:
                    log(
                        f"torch.compile could not wrap {cls.__name__} ({_one_line(exc)}); using eager."
                    )

        if state["active"]:
            try:
                result = state["fn"](*args, **kwargs)
            except Exception as exc:
                # Inductor reports a missing toolchain by raising here, during
                # the first traced call, not from torch.compile itself. Retry
                # eagerly so the separation still produces stems: if the input
                # is genuinely bad, the eager call raises the honest error.
                state["active"] = False
                log(f"Compiled {cls.__name__} failed ({_one_line(exc)}); continuing eagerly.")
            else:
                # A pass that returned is the first moment the cache is known
                # to hold usable kernels, so it is the moment worth recording.
                # Outside the try above: recording is not part of the forward,
                # and a failure in it must not be read as a compile failure.
                if not state["recorded"]:
                    state["recorded"] = True

                    model_id = _MODEL_IDS_BY_CLASS.get(cls.__name__)

                    if model_id is not None:
                        record_compiled(model_id, state["device"], "separation")

                return result

        return original(self, *args, **kwargs)

    forward.__name__ = original.__name__
    forward.__qualname__ = original.__qualname__
    cls.forward = forward
    setattr(cls, _PATCH_MARKER, True)


def arm_torch_compile(
    log: Callable[[str], None] | None = None,
    modules: dict[str, object] | None = None,
) -> list[str]:
    """Arm compiled inference for separator classes already imported.

    ``modules`` names the namespace to scan, defaulting to everything
    imported. Tests pass a scoped mapping so arming cannot reach into the
    real separator classes this process may also have loaded.

    Returns the names of the classes patched, which is empty whenever
    compiling was not requested or the upstream module laid its model out
    somewhere this cannot see. Callers treat an empty list as "ran eagerly",
    never as an error.
    """
    emit = log or (lambda _message: None)

    if not compile_requested():
        return []

    try:
        import torch
        from torch import nn
    except Exception as exc:
        emit(f"torch.compile was requested but torch is unavailable ({_one_line(exc)}).")
        return []

    if not hasattr(torch, "compile"):
        emit("torch.compile was requested but this PyTorch build has no compile().")
        return []

    cache_dir = inductor_cache_dir()
    try:
        cache_dir.mkdir(parents=True, exist_ok=True)
        # Inductor reads this once, on first use; setting it before any
        # compilation is what makes kernels outlive this subprocess.
        #
        # Assigned rather than setdefault'ed. The warm-up assigns it, so an
        # inherited TORCHINDUCTOR_CACHE_DIR used to send the warm-up and the
        # real separation to two different caches: the warm-up filled ours,
        # the job that was supposed to benefit compiled from cold into the
        # inherited one, and the model manager went on reporting "Compiled".
        # Somewhere else to put the kernels is asked for through
        # STEMLAB_TORCH_COMPILE_CACHE, which inductor_cache_dir honours, and
        # which both processes then agree on.
        os.environ["TORCHINDUCTOR_CACHE_DIR"] = str(cache_dir)
    except OSError as exc:
        emit(
            f"Could not prepare the compile cache at {cache_dir} ({_one_line(exc)}); compiling anyway."
        )

    patched: list[str] = []
    seen: set[int] = set()

    # A snapshot: importing inside this loop would mutate sys.modules.
    for module in list((sys.modules if modules is None else modules).values()):
        if module is None:
            continue
        try:
            members = vars(module)
        except TypeError:
            continue

        for name in _TARGET_CLASS_NAMES:
            candidate = members.get(name)
            if (
                isinstance(candidate, type)
                and issubclass(candidate, nn.Module)
                and id(candidate) not in seen
                and not getattr(candidate, _PATCH_MARKER, False)
            ):
                seen.add(id(candidate))
                _wrap_forward(candidate, emit)
                patched.append(candidate.__name__)

    if not patched:
        emit("torch.compile was requested but no separator model class was found.")

    return patched
