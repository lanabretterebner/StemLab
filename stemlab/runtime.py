"""Shared process helpers for StemLab job runners and model backends."""

from __future__ import annotations

import re
import sys
from collections.abc import Callable
from typing import BinaryIO

_PERCENT_RE = re.compile(rb"(?<!\d)(\d{1,3}(?:\.\d+)?)%")


def configure_utf8_stdio() -> None:
    """Force UTF-8, line-buffered stdout/stderr for JUCE ChildProcess readers."""
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except Exception:
        pass


def last_progress_percent(raw: bytes) -> float | None:
    """Return the last ``NN%`` value in a subprocess output fragment, if any."""
    matches = _PERCENT_RE.findall(raw)
    if not matches:
        return None
    try:
        percent = float(matches[-1].decode("ascii"))
    except ValueError:
        return None
    if 0.0 <= percent <= 100.0:
        return percent
    return None


def drain_cr_lf_stream(stdout: BinaryIO, on_segment: Callable[[bytes], None]) -> None:
    """Read a pipe one byte at a time and emit each CR/LF-terminated fragment.

    tqdm-style backends rewrite the same line with carriage returns. Iterating
    ``stdout`` line-by-line would hide progress until a newline arrives.
    """
    segment = bytearray()
    while True:
        byte = stdout.read(1)
        if not byte:
            if segment:
                on_segment(bytes(segment))
            return
        if byte in (b"\r", b"\n"):
            if segment:
                on_segment(bytes(segment))
                segment.clear()
        else:
            segment.extend(byte)
