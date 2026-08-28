"""Shared process helpers for FI-STEM job runners and model backends."""

from __future__ import annotations

import os
import queue
import re
import subprocess
import sys
import threading
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

_PERCENT_RE = re.compile(rb"(?<!\d)(\d{1,3}(?:\.\d+)?)%")


def _configure_packaged_models() -> None:
    """Point third-party backends at release-local model caches when present."""
    engine = Path(sys.executable).resolve().parent
    caches = engine / "ModelCaches"
    if not caches.is_dir():
        return
    os.environ.setdefault("BS_ROFORMER_MODELS_PATH", str(caches / "bs-roformer-infer"))
    demucs_repo = caches / "demucs"
    if (demucs_repo / "5c90dfd2-34c22ccb.th").is_file():
        os.environ.setdefault("STEMLAB_DEMUCS_MODEL_REPO", str(demucs_repo))
    os.environ.setdefault("HF_HOME", str(caches / "huggingface"))
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
    os.environ.setdefault("STEMLAB_RECURSIVE_MODEL_DIR", str(engine / "Models" / "Recursive"))


_configure_packaged_models()


class JobCancelled(RuntimeError):
    """Raised when the user cancels one specific FI-STEM worker job."""


@dataclass(frozen=True)
class CancellationToken:
    """Cooperative cancellation backed by a per-job sentinel file."""

    path: Path | None = None

    @property
    def requested(self) -> bool:
        return self.path is not None and self.path.exists()

    def raise_if_cancelled(self) -> None:
        if self.requested:
            raise JobCancelled("FI-STEM job cancelled")


def configure_utf8_stdio() -> None:
    """Force UTF-8, line-buffered stdout/stderr for JUCE ChildProcess readers."""
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except Exception:
        pass


def child_process_env() -> dict[str, str]:
    """Return an inherited environment that forces Python child CLIs to UTF-8.

    The Windows portable runtime can otherwise inherit a legacy console encoding
    such as cp1252.  Some third-party model CLIs print Unicode status symbols
    (for example ``✓``), which can crash the child before inference even starts.
    """
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


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
    cancellation: CancellationToken | None = None,
) -> int:
    """Run a model CLI while forwarding progress and honoring cancellation."""
    if cancellation:
        cancellation.raise_if_cancelled()

    # Buffered, even though drain_cr_lf_stream takes one byte at a time: a
    # BufferedReader refills from whatever the pipe already holds rather than
    # waiting for a full buffer, so fragments still arrive as promptly as
    # unbuffered reads while costing one read(2) per refill instead of one per
    # byte - on the machine that is at that moment running the model.
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=-1,
        env=child_process_env(),
    )
    assert process.stdout is not None

    last_reported = -1
    encoding = "utf-8"

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

    segments: queue.Queue[bytes | None] = queue.Queue()

    def read_output() -> None:
        try:
            drain_cr_lf_stream(process.stdout, segments.put)
        finally:
            segments.put(None)

    reader = threading.Thread(target=read_output, name="FI-STEM process output", daemon=True)
    reader.start()

    try:
        reader_finished = False
        while not reader_finished or process.poll() is None:
            if cancellation and cancellation.requested:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)
                raise JobCancelled("FI-STEM job cancelled")

            try:
                segment = segments.get(timeout=0.10)
            except queue.Empty:
                continue

            if segment is None:
                reader_finished = True
            else:
                consume_segment(segment)

        reader.join(timeout=1.0)
        return process.wait()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=2.0)
