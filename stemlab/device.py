"""Device selection helpers for PyTorch-backed separators."""

from __future__ import annotations

import importlib
from typing import Callable


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
        try:
            torch = importlib.import_module("torch")
            xpu_available = bool(
                getattr(torch, "xpu", None) is not None and torch.xpu.is_available()
            )
        except Exception:
            xpu_available = False

        if xpu_available:
            return "xpu"

        if log:
            log(
                "XPU was requested, but this PyTorch install cannot use it. "
                "Falling back to CPU."
            )
        return "cpu"

    if device not in {"cuda", "gpu"}:
        return requested

    try:
        torch = importlib.import_module("torch")
        cuda_available = bool(torch.cuda.is_available())
    except Exception:
        cuda_available = False

    if cuda_available:
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
