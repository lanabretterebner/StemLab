"""Shared process helpers for StemLab job runners and model backends."""

from __future__ import annotations

import contextlib
import os
import queue
import re
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
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

# Exit code a job dies with when the plugin cancels it, distinguishing a
# user's cancel from a genuine failure on the JUCE side.
CANCEL_EXIT_CODE = 75

# Exit code when the job shut itself down because the plugin process
# disappeared (host closed or crashed mid-job).
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
    """Raised when the user cancels one specific StemLab worker job."""


@dataclass(frozen=True)
class CancellationToken:
    """Cooperative cancellation backed by a per-job sentinel file."""

    path: Path | None = None

    @property
    def requested(self) -> bool:
        return self.path is not None and self.path.exists()

    def raise_if_cancelled(self) -> None:
        if self.requested:
            raise JobCancelled("StemLab job cancelled")


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


def looks_like_download(raw: bytes) -> bool:
    """True when a fragment is a byte-transfer bar (a model download)."""
    return (
        _BYTE_RATE_RE.search(raw) is not None
        or _BYTE_COUNT_RE.search(raw) is not None
        or _DOWNLOAD_HINT_RE.search(raw) is not None
    )


def tqdm_remaining_seconds(raw: bytes) -> float | None:
    """Return tqdm's ``[elapsed<remaining, rate]`` remaining time, if present."""
    # The last match on the line, not the first: tqdm rewrites the whole bar
    # on one carriage-returned line, so earlier matches are stale.
    matches = list(_TQDM_REMAINING_RE.finditer(raw))

    if not matches:
        return None

    match = matches[-1]
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

    CancellationToken already gives a clean cooperative stop wherever the
    job reaches a checkpoint. This is the backstop for where it cannot: a
    model child can sit inside torch for minutes without reaching one, and
    nothing cooperative runs at all once the plugin is gone. The watchdog
    kills the registered model subprocesses and this process together, so a
    separation can never keep burning CPU with nobody left to collect it.

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


class _DownloadWatchingStream:
    """A text sink that reports byte-transfer bars and forwards everything on.

    run_progress_process can watch a model CLI because the CLI is a child
    process and its output is a pipe. audio-separator is not a CLI - it
    downloads inside this process while ``load_model`` blocks - so there is
    no pipe to read and the bar it writes goes straight to the job's own
    stdout, where the plugin ignores it. This wraps that stream instead,
    applying the same two parsers so an in-process download reaches the
    status area the same way a subprocess one does.
    """

    def __init__(self, wrapped, on_download: Callable[[float], None]) -> None:
        self._wrapped = wrapped
        self._on_download = on_download
        self._segment = ""
        self._last_reported = -1

    def write(self, text: str) -> int:
        written = self._wrapped.write(text)

        # tqdm rewrites one line with carriage returns, so a bar only ever
        # arrives as CR-terminated fragments - splitting on newlines alone
        # would hold the whole download in the buffer until it finished.
        self._segment += text
        while True:
            cut = max(self._segment.find("\r"), self._segment.find("\n"))
            if cut < 0:
                break
            self._consume(self._segment[:cut])
            self._segment = self._segment[cut + 1 :]

        return written

    def _consume(self, fragment: str) -> None:
        if not fragment:
            return

        raw = fragment.encode("utf-8", "replace")
        if not looks_like_download(raw):
            return

        percent = last_progress_percent(raw)
        if percent is None or int(percent) == self._last_reported:
            return

        self._last_reported = int(percent)
        self._on_download(percent)

    def drain(self) -> None:
        """Consume a fragment left unterminated when the transfer ended.

        tqdm's last frame is the one that says 100%, and it is written
        without a trailing carriage return - the bar is closed by the caller
        moving on, not by another redraw. Dropping it would leave the status
        stuck a frame short of finished.
        """
        pending, self._segment = self._segment, ""
        self._consume(pending)

    def flush(self) -> None:
        self._wrapped.flush()

    def isatty(self) -> bool:
        # tqdm renders a bar rather than one line per update only when it
        # believes it is on a terminal, and a bar is what the parsers read.
        return True

    def __getattr__(self, name):
        return getattr(self._wrapped, name)


@contextlib.contextmanager
def report_downloads(on_download: Callable[[float], None] | None):
    """Report byte-transfer bars written to stdout/stderr inside this process.

    A no-op without a callback, so callers can wrap unconditionally.
    """
    if on_download is None:
        yield
        return

    stdout, stderr = sys.stdout, sys.stderr
    watched_out = _DownloadWatchingStream(stdout, on_download)
    watched_err = _DownloadWatchingStream(stderr, on_download)
    sys.stdout, sys.stderr = watched_out, watched_err
    try:
        yield
    finally:
        for watched in (watched_out, watched_err):
            try:
                watched.drain()
            except Exception:
                # Reporting a download must never be what fails a job.
                pass
        sys.stdout, sys.stderr = stdout, stderr


def run_progress_process(
    command: Sequence[str],
    log: Callable[[str], None],
    progress: Callable[[float], None],
    *,
    eta: Callable[[float], None] | None = None,
    download: Callable[[float], None] | None = None,
    log_progress_lines: bool = True,
    cancellation: CancellationToken | None = None,
) -> int:
    """Run a model CLI while forwarding progress and honoring cancellation.

    ``progress`` receives separation percentages. ``download`` receives
    percentages from byte-transfer bars (one-time model downloads) so callers
    can present them as their own stage instead of fake separation progress.
    ``eta`` receives estimated seconds remaining whenever the child reports
    one - tqdm's ``[elapsed<remaining]`` postfix, or bs_roformer's
    "Estimated time remaining" lines (its chunk loop prints no percentages,
    so those lines are also converted into ``progress`` calls here).
    """
    if cancellation:
        cancellation.raise_if_cancelled()

    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        env=child_process_env(),
    )
    assert process.stdout is not None

    with _active_processes_lock:
        _active_processes.append(process)

    # A watchdog shutdown that snapshotted the registry just before this
    # append would _exit without ever seeing this child, leaving a fresh
    # torch process running with nobody attached. shut_down sets the flag
    # before it snapshots, so checking it here covers the other ordering.
    if _shutdown_in_progress.is_set():
        try:
            process.terminate()
        except OSError:
            pass

    last_reported = -1
    last_download_reported = -1
    roformer_total: float | None = None
    encoding = "utf-8"

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

    segments: queue.Queue[bytes | None] = queue.Queue()

    def read_output() -> None:
        try:
            drain_cr_lf_stream(process.stdout, segments.put)
        finally:
            segments.put(None)

    reader = threading.Thread(target=read_output, name="StemLab process output", daemon=True)
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
                raise JobCancelled("StemLab job cancelled")

            try:
                segment = segments.get(timeout=0.10)
            except queue.Empty:
                continue

            if segment is None:
                reader_finished = True
            else:
                consume_segment(segment)

        reader.join(timeout=1.0)
        code = process.wait()

        # If the child died because the watchdog is taking the job down,
        # park until its _exit lands - otherwise the caller reports a
        # phantom model failure in the cancel's last milliseconds.
        while _shutdown_in_progress.is_set():
            time.sleep(0.25)

        return code
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=2.0)

        with _active_processes_lock:
            if process in _active_processes:
                _active_processes.remove(process)
