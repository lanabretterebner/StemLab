"""Device selection helpers for PyTorch-backed separators."""

from __future__ import annotations

import importlib
import importlib.metadata
import json
import os
import sys
from pathlib import Path
from typing import Callable

# Environment switches that change a GPU probe's answer without a torch
# reinstall. HIP_VISIBLE_DEVICES matters because ROCm builds answer through
# the torch.cuda API (see pick_best_device).
_PROBE_ENV_VARS = ("CUDA_VISIBLE_DEVICES", "HIP_VISIBLE_DEVICES")


def _probe_cache_path() -> Path:
    # Same resolution as analysis_cache.managed_analysis_dir(), duplicated
    # rather than imported: that module carries sqlite3 and the process
    # runtime helpers, and this one is imported by every job parent - the
    # exact startup weight the probe cache exists to avoid.
    override = os.environ.get("STEMLAB_ANALYSIS_HOME")
    if override:
        return Path(override).expanduser().resolve() / "device_probe.json"
    local = os.environ.get("LOCALAPPDATA")
    if local:
        return Path(local) / "StemLab" / "Analysis" / "device_probe.json"
    return Path.home() / ".stemlab" / "analysis" / "device_probe.json"


def _probe_fingerprint() -> dict[str, str | None] | None:
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
    fingerprint: dict[str, str | None] = {"torch": version}
    for name in _PROBE_ENV_VARS:
        fingerprint[name] = os.environ.get(name)
    return fingerprint


def _cached_answer(fingerprint: dict[str, str | None], device: str) -> bool | None:
    # A corrupt, stale, or unreadable cache file must behave like a miss:
    # the probe reproduces the answer, so no error here is worth surfacing.
    try:
        data = json.loads(_probe_cache_path().read_text(encoding="utf-8"))
        if data.get("fingerprint") != fingerprint:
            return None
        answer = data.get("probes", {}).get(device)
    except Exception:
        return None
    return answer if isinstance(answer, bool) else None


def _store_answer(fingerprint: dict[str, str | None], device: str, available: bool) -> None:
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
        available = False

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
