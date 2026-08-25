"""Device selection helpers for PyTorch-backed separators."""

from __future__ import annotations

import importlib
from typing import Callable


def resolve_torch_device(
    requested: str,
    log: Callable[[str], None] | None = None,
) -> str:
    """Return a PyTorch device that this environment can actually use.

    FI-STEM prefers CUDA when it is available, but Windows development installs
    often use CPU-only PyTorch. In that case, falling back to CPU is slower but
    lets the separation finish instead of crashing during model loading.
    """
    device = (requested or "cpu").strip().lower()

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
