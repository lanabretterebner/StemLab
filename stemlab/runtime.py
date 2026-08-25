"""Shared process helpers for StemLab job runners and model backends."""

from __future__ import annotations

import locale
import re
import subprocess
import sys
from collections.abc import Callable, Sequence
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


def run_progress_process(
    command: Sequence[str],
    log: Callable[[str], None],
    progress: Callable[[float], None],
    *,
    log_progress_lines: bool = True,
) -> int:
    """Run a model CLI while forwarding CR/LF progress output."""
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    assert process.stdout is not None

    last_reported = -1
    encoding = locale.getpreferredencoding(False) or "utf-8"

    def consume_segment(raw: bytes) -> None:
        nonlocal last_reported
        if not raw:
            return

        percent = last_progress_percent(raw)
        if percent is not None and int(percent) != last_reported:
            last_reported = int(percent)
            progress(percent)

        try:
            text = raw.decode(encoding, errors="replace").strip()
        except LookupError:
            text = raw.decode("utf-8", errors="replace").strip()
        if text and (log_progress_lines or percent is None):
            log(text)

    drain_cr_lf_stream(process.stdout, consume_segment)
    return process.wait()
