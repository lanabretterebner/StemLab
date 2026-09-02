"""Device selection helpers for PyTorch-backed separators."""

from __future__ import annotations

import importlib
import importlib.metadata
import json
import os
import shutil
import sys
import time
from pathlib import Path
from typing import Callable

# Environment switches that change a GPU probe's answer without a torch
# reinstall. HIP_VISIBLE_DEVICES matters because ROCm builds answer through
# the torch.cuda API (see pick_best_device).
_PROBE_ENV_VARS = ("CUDA_VISIBLE_DEVICES", "HIP_VISIBLE_DEVICES")

# A cached answer can also go stale for reasons no fingerprint can see (a
# driver update, a GPU swapped out); one live probe per day bounds how long
# any such staleness can last.
_PROBE_CACHE_TTL_SECONDS = 24 * 60 * 60


def _probe_cache_path() -> Path:
    # Imported lazily: analysis_cache brings sqlite3 and the runtime
    # helpers, a few milliseconds this hot path only pays on a cache
    # access, in exchange for one definition of the managed directory.
    from .analysis_cache import managed_analysis_dir

    return managed_analysis_dir() / "device_probe.json"


def _driver_markers() -> dict[str, bool]:
    # A driver installed or removed after the first probe must flip the
    # cache stale; these see the driver without touching torch. Intel has
    # no equally cheap universal marker - the TTL covers it.
    return {
        "nvidia": Path("/proc/driver/nvidia/version").exists()
        or shutil.which("nvidia-smi") is not None,
        "amdgpu": Path("/sys/module/amdgpu").exists(),
    }


def _probe_fingerprint() -> dict[str, object] | None:
    """Identity of the environment a cached probe answer belongs to.

    Reads torch's version from package metadata precisely so torch itself
    stays unimported. None means the metadata is unreadable (torch is not
    installed as a package); those environments take the live path, which
    already turns a failed import into "unavailable".
    """
    try:
        version = importlib.metadata.version("torch")
    except Exception:
        return None
    fingerprint: dict[str, object] = {"torch": version}
    for name in _PROBE_ENV_VARS:
        fingerprint[name] = os.environ.get(name)
    fingerprint["drivers"] = _driver_markers()
    return fingerprint


def _cached_answer(fingerprint: dict[str, object], device: str) -> bool | None:
    # A corrupt, stale, or unreadable cache file must behave like a miss:
    # the probe reproduces the answer, so no error here is worth surfacing.
    try:
        data = json.loads(_probe_cache_path().read_text(encoding="utf-8"))
        if data.get("fingerprint") != fingerprint:
            return None
        if time.time() - float(data.get("time", 0.0)) > _PROBE_CACHE_TTL_SECONDS:
            return None
        answer = data.get("probes", {}).get(device)
    except Exception:
        return None
    return answer if isinstance(answer, bool) else None


def _store_answer(fingerprint: dict[str, object], device: str, available: bool) -> None:
    # Best-effort: a failed write only means the next process probes again.
    path = _probe_cache_path()
    try:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            data = None
        if not isinstance(data, dict) or data.get("fingerprint") != fingerprint:
            data = {"fingerprint": fingerprint, "probes": {}}
        probes = data.get("probes")
        if not isinstance(probes, dict):
            probes = {}
            data["probes"] = probes
        probes[device] = available
        data["time"] = time.time()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    except Exception:
        pass


def _probe_available(device: str, probe: Callable[[], bool]) -> bool:
    """Run one availability probe, importing torch at most once per environment.

    Every separation job starts a fresh parent process, and a cold torch
    import costs seconds just to answer "is this backend usable". The answer
    only changes with the installed torch wheel or the GPU-visibility
    environment, so it is cached on disk under that fingerprint.
    """
    if "torch" in sys.modules:
        # An already-loaded torch makes the probe near-free, and an
        # in-process substitute (the test suite plants fakes in sys.modules)
        # must outrank any stored answer.
        try:
            return bool(probe())
        except Exception:
            return False

    fingerprint = _probe_fingerprint()
    if fingerprint is not None:
        cached = _cached_answer(fingerprint, device)
        if cached is not None:
            return cached

    try:
        available = bool(probe())
    except Exception:
        # A probe that raised has not answered "no". A torch import that broke
        # on a half-written wheel, or a CUDA init that threw while the driver
        # was being upgraded, both arrive here, and neither changes the
        # fingerprint - so writing "unavailable" would hold this job's CPU
        # fallback over every job for the next day, long after the cause was
        # gone. This run falls back; nothing is recorded.
        return False

    if fingerprint is not None:
        _store_answer(fingerprint, device, available)
    return available


def _cuda_probe() -> bool:
    torch = importlib.import_module("torch")
    return bool(torch.cuda.is_available())


def _xpu_probe() -> bool:
    torch = importlib.import_module("torch")
    return bool(getattr(torch, "xpu", None) is not None and torch.xpu.is_available())


def resolve_torch_device(
    requested: str,
    log: Callable[[str], None] | None = None,
) -> str:
    """Return a PyTorch device that this environment can actually use.

    StemLab prefers CUDA when it is available, but Windows development installs
    often use CPU-only PyTorch. In that case, falling back to CPU is slower but
    lets the separation finish instead of crashing during model loading.
    """
    device = (requested or "cpu").strip().lower()

    if device == "xpu":
        # Intel GPUs through PyTorch's built-in XPU backend (torch 2.5+,
        # official +xpu wheels for Linux and Windows). Unavailable means the
        # wheel or the Intel compute runtime is missing - fall back instead
        # of crashing during model loading.
        if _probe_available("xpu", _xpu_probe):
            return "xpu"

        if log:
            log(
                "XPU was requested, but this PyTorch install cannot use it. "
                "Falling back to CPU."
            )
        return "cpu"

    if device not in {"cuda", "gpu"}:
        # The normalised spelling, not the raw request: torch device strings
        # are case- and whitespace-sensitive, so "CPU" or " cpu" would reach
        # the model child verbatim and crash it during loading.
        return device

    if _probe_available(device, _cuda_probe):
        return "cuda"

    if log:
        log("CUDA was requested, but this PyTorch install cannot use CUDA. Falling back to CPU.")
    return "cpu"


def pick_best_device(log: Callable[[str], None] | None = None) -> str:
    """Best available torch device: cuda, then xpu, then cpu.

    "cuda" also covers AMD GPUs on Linux: the ROCm builds of PyTorch expose
    HIP through the torch.cuda API, so a ROCm install answers the same probe
    with no separate code path.
    """
    if resolve_torch_device("cuda") == "cuda":
        return "cuda"

    if resolve_torch_device("xpu") == "xpu":
        if log:
            log("Using the Intel XPU backend.")
        return "xpu"

    if log:
        log("No GPU backend is available - separating on CPU.")
    return "cpu"
