"""Shared process helpers for StemLab job runners and model backends."""

from __future__ import annotations

import locale
import os
import re
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import BinaryIO

_PERCENT_RE = re.compile(rb"(?<!\d)(\d{1,3}(?:\.\d+)?)%")

# tqdm byte-transfer bars. Their percents describe a model download, not
# separation, and must not drive the main bar. Three signatures, because
# bs_roformer passes ncols=80 with a long description and tqdm truncates
# the rate clean off the line:
#   - a transfer rate           "23.4MB/s", "512kB/s", "1.2GiB/s"
#   - unit-scaled byte counts   "6.48M/667M", "8.00k/667M", "1.2G/2.7G"
#   - the word "download" in the fragment
# Separation bars count plain seconds ("5.85/11.7") and never match.
_BYTE_RATE_RE = re.compile(rb"\d(?:\.\d+)?\s*[kKMGTP]?i?B/s")
_BYTE_COUNT_RE = re.compile(rb"\d(?:\.\d+)?[kKMGTP]i?B?/\d")
_DOWNLOAD_HINT_RE = re.compile(rb"download", re.IGNORECASE)

# tqdm postfix "[elapsed<remaining, rate]" - the remaining half is a real ETA.
_TQDM_REMAINING_RE = re.compile(rb"<(?:(\d+):)?(\d{1,2}):(\d{2})[,\]]")

# bs_roformer's chunk loop prints no percentages at all - only these two
# time estimates. Together they are both a percentage and an ETA.
_RF_TOTAL_RE = re.compile(
    rb"Estimated total processing time for this track:\s*(\d+(?:\.\d+)?)\s*seconds"
)
_RF_REMAINING_RE = re.compile(rb"Estimated time remaining:\s*(\d+(?:\.\d+)?)\s*seconds")

# The exit code a job dies with when the plugin cancels it. Distinguishes a
# user's cancel from a genuine failure in the JUCE-side exit handling.
CANCEL_EXIT_CODE = 75

# The exit code when the job shut itself down because the plugin process
# disappeared (host closed or crashed mid-job). Nobody is usually left to
# read it; it exists for post-mortem clarity.
ORPHAN_EXIT_CODE = 76

CANCEL_FILE = "stemlab_cancel.txt"

# Model subprocesses currently running under this job, so a cancel can take
# them down with the job instead of orphaning a torch run at full CPU.
_active_processes: list[subprocess.Popen] = []
_active_processes_lock = threading.Lock()

# Set while the watchdog is taking the job down. The main thread sees its
# model child die at that moment and must not race ahead reporting a model
# failure - the watchdog's exit is the authoritative outcome.
_shutdown_in_progress = threading.Event()


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


def looks_like_download(raw: bytes) -> bool:
    """True when a fragment is a byte-transfer bar (a model download)."""
    return (
        _BYTE_RATE_RE.search(raw) is not None
        or _BYTE_COUNT_RE.search(raw) is not None
        or _DOWNLOAD_HINT_RE.search(raw) is not None
    )


def tqdm_remaining_seconds(raw: bytes) -> float | None:
    """Return tqdm's ``[elapsed<remaining, rate]`` remaining time, if present."""
    match = None
    for match in _TQDM_REMAINING_RE.finditer(raw):
        pass
    if match is None:
        return None
    hours = int(match.group(1) or 0)
    return float(hours * 3600 + int(match.group(2)) * 60 + int(match.group(3)))


def _terminate_registered_processes() -> None:
    with _active_processes_lock:
        processes = list(_active_processes)
    for process in processes:
        try:
            process.terminate()
        except OSError:
            pass
    deadline = time.monotonic() + 2.0
    for process in processes:
        try:
            process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except (subprocess.TimeoutExpired, OSError):
            try:
                process.kill()
            except OSError:
                pass


def _make_parent_death_check() -> Callable[[], bool]:
    """Return a poll function that is True once the launching process died.

    The plugin passes its own pid in STEMLAB_PARENT_PID, which is what makes
    this reliable: comparing ppid against the value seen at startup only
    works if this thread started before the parent died, and it does not -
    importing torch takes seconds, and a host that dies during that window
    has already had this process reparented. An explicitly named pid has no
    such window.

    Without the variable (an older plugin, or a manual run) it falls back to
    watching for reparenting, which is still right in the common case.
    """
    named_parent = 0

    try:
        named_parent = int(os.environ.get("STEMLAB_PARENT_PID", "") or 0)
    except ValueError:
        named_parent = 0

    if sys.platform == "win32":
        import ctypes

        synchronize = 0x00100000
        wait_object_0 = 0
        kernel32 = ctypes.windll.kernel32
        target = named_parent or os.getppid()
        handle = kernel32.OpenProcess(synchronize, False, target)

        if not handle:
            # A named parent that cannot be opened has already exited (the
            # pid is gone); an unopenable ppid is not conclusive.
            return (lambda: True) if named_parent else (lambda: False)

        return lambda: kernel32.WaitForSingleObject(handle, 0) == wait_object_0

    if named_parent > 0:

        def named_parent_died() -> bool:
            try:
                os.kill(named_parent, 0)
            except ProcessLookupError:
                return True
            except PermissionError:
                # Alive, just not ours to signal.
                return False
            return False

        return named_parent_died

    initial_parent = os.getppid()
    return lambda: os.getppid() != initial_parent


def start_cancel_watchdog(job_dir: str | Path) -> None:
    """Shut the whole job down on a plugin cancel or a vanished plugin.

    The JUCE plugin cannot see this process's children, so a direct kill from
    the host side would orphan the torch subprocess at full CPU. Instead the
    plugin writes ``stemlab_cancel.txt`` into the job directory and this
    watchdog takes the job down from the inside, model children included.

    The same watchdog also notices when the launching process itself has
    died - the plugin was unloaded without a clean shutdown, or the host
    closed or crashed mid-job - and shuts down the same way, so a separation
    can never keep burning CPU with nobody left to collect it.

    Any sentinel seen is honored, even one written before this thread
    started: a cancel clicked while the interpreter was still importing must
    not be lost. The plugin deletes leftover sentinels before launching a
    job, so a stale file cannot cancel a fresh run.
    """
    cancel_file = Path(job_dir) / CANCEL_FILE
    parent_died = _make_parent_death_check()

    def shut_down(marker: str, exit_code: int) -> None:
        _shutdown_in_progress.set()
        try:
            print(marker, flush=True)
        except OSError:
            pass
        _terminate_registered_processes()
        try:
            cancel_file.unlink(missing_ok=True)
        except OSError:
            pass
        os._exit(exit_code)

    def watch() -> None:
        while True:
            time.sleep(0.5)
            try:
                if cancel_file.exists():
                    shut_down("STEMLAB_CANCELLED", CANCEL_EXIT_CODE)
            except OSError:
                pass
            try:
                if parent_died():
                    shut_down("STEMLAB_ORPHANED", ORPHAN_EXIT_CODE)
            except OSError:
                pass

    threading.Thread(target=watch, name="stemlab-cancel-watchdog", daemon=True).start()


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
    eta: Callable[[float], None] | None = None,
    download: Callable[[float], None] | None = None,
    log_progress_lines: bool = True,
) -> int:
    """Run a model CLI while forwarding CR/LF progress output.

    ``progress`` receives separation percentages. ``download`` receives
    percentages from byte-transfer bars (one-time model downloads) so callers
    can present them as their own stage instead of fake separation progress.
    ``eta`` receives estimated seconds remaining whenever the child reports
    one - tqdm's ``[elapsed<remaining]`` postfix, or bs_roformer's
    "Estimated time remaining" lines (its chunk loop prints no percentages,
    so those lines are also converted into ``progress`` calls here).
    """
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    assert process.stdout is not None

    with _active_processes_lock:
        _active_processes.append(process)

    # A shutdown that snapshotted the registry just before this append would
    # _exit without ever seeing this child, leaving a fresh torch process
    # running with nobody attached. shut_down sets the flag before it
    # snapshots, so checking it here covers the opposite ordering too.
    if _shutdown_in_progress.is_set():
        try:
            process.terminate()
        except OSError:
            pass

    last_reported = -1
    last_download_reported = -1
    roformer_total: float | None = None
    encoding = locale.getpreferredencoding(False) or "utf-8"

    def report_progress(percent: float) -> None:
        nonlocal last_reported
        if int(percent) != last_reported:
            last_reported = int(percent)
            progress(percent)

    def consume_segment(raw: bytes) -> None:
        nonlocal last_download_reported, roformer_total
        if not raw:
            return

        is_download = looks_like_download(raw)
        percent = last_progress_percent(raw)
        handled_progress = False

        if percent is not None:
            if is_download:
                if download is not None and int(percent) != last_download_reported:
                    last_download_reported = int(percent)
                    download(percent)
                handled_progress = True
            else:
                report_progress(percent)
                handled_progress = True

        total_match = _RF_TOTAL_RE.search(raw)
        if total_match is not None:
            roformer_total = max(1e-6, float(total_match.group(1)))
            if eta is not None:
                eta(roformer_total)
            report_progress(0.0)
            handled_progress = True

        remaining_match = _RF_REMAINING_RE.search(raw)
        if remaining_match is not None:
            remaining = float(remaining_match.group(1))
            if eta is not None:
                eta(remaining)
            if roformer_total is not None:
                report_progress(
                    max(0.0, min(100.0, 100.0 * (1.0 - remaining / roformer_total)))
                )
            handled_progress = True
        elif not is_download:
            tqdm_remaining = tqdm_remaining_seconds(raw)
            if tqdm_remaining is not None and eta is not None:
                eta(tqdm_remaining)

        try:
            text = raw.decode(encoding, errors="replace").strip()
        except LookupError:
            text = raw.decode("utf-8", errors="replace").strip()
        if text and (log_progress_lines or not handled_progress):
            log(text)

    try:
        drain_cr_lf_stream(process.stdout, consume_segment)
        code = process.wait()

        # If the child died because the watchdog is cancelling the job,
        # park this thread until the watchdog's _exit lands - otherwise the
        # caller reports a phantom model failure in the cancel's last
        # milliseconds.
        while _shutdown_in_progress.is_set():
            time.sleep(0.25)

        return code
    finally:
        with _active_processes_lock:
            if process in _active_processes:
                _active_processes.remove(process)
