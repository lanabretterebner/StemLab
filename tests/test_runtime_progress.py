"""Tests for the model-output parsing and cancel plumbing in stemlab.runtime."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

from stemlab.runtime import (
    CANCEL_EXIT_CODE,
    CANCEL_FILE,
    ORPHAN_EXIT_CODE,
    last_progress_percent,
    looks_like_download,
    tqdm_remaining_seconds,
)


def pid_alive(pid: int) -> bool:
    """Is this process still running?

    os.kill(pid, 0) is the POSIX idiom and is wrong on Windows, where os.kill
    calls TerminateProcess for any signal it does not recognise - signal 0
    included. It does not ask a question there, it tries to kill, and CPython
    surfaces that as a SystemError rather than an answer.
    """
    if sys.platform == "win32":
        import ctypes

        query_limited_information = 0x1000
        still_active = 259

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(query_limited_information, False, pid)
        if not handle:
            return False

        try:
            code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == still_active
        finally:
            kernel32.CloseHandle(handle)

    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Alive, just not ours to signal.
        return True
    return True


# SIGKILL does not exist on Windows; SIGTERM is what os.kill maps to
# TerminateProcess there, which is the same unconditional stop.
KILL_SIGNAL = getattr(signal, "SIGKILL", signal.SIGTERM)


class TestPercentParsing:
    def test_plain_percent(self):
        assert last_progress_percent(b"50%|####      | 5.85/11.7") == 50.0

    def test_no_percent(self):
        assert last_progress_percent(b"Separating track input.wav") is None

    def test_last_percent_wins(self):
        assert last_progress_percent(b"10% then 60%") == 60.0


class TestDownloadDetection:
    def test_tqdm_byte_bar_is_download(self):
        segment = b"model.ckpt:  45%|####  | 1.21G/2.70G [00:30<01:10, 23.4MB/s]"
        assert looks_like_download(segment)

    def test_binary_units_are_download(self):
        assert looks_like_download(b"12%|#| 12.0MiB/98.0MiB [00:01<00:09, 9.4MiB/s]")

    def test_slow_download_is_download(self):
        assert looks_like_download(b"3%| | 80.0M/2.70G [00:30<19:10, 512kB/s]")

    def test_truncated_download_bar_is_download(self):
        # The real bs_roformer download bar: ncols=80 plus a long description
        # makes tqdm cut the rate clean off; the scaled byte counts remain.
        segment = b"Roformer Model: BS Roformer SW by jarredou checkpoint:   1%| | 6.48M/667M [00:00"
        assert looks_like_download(segment)

    def test_download_keyword_is_download(self):
        segment = b"Downloading model.ckpt:  45%| | 302M/"
        assert looks_like_download(segment)

    def test_separation_bar_is_not_download(self):
        segment = b"50%|####| 5.85/11.7 [00:10<00:10,  1.75s/seconds]"
        assert not looks_like_download(segment)


class TestTqdmRemaining:
    def test_minutes_seconds(self):
        segment = b"50%|####| 5.85/11.7 [00:10<01:30,  1.75s/seconds]"
        assert tqdm_remaining_seconds(segment) == 90.0

    def test_hours(self):
        segment = b"1%| | 1/100 [00:01<1:40:00,  1.00s/it]"
        assert tqdm_remaining_seconds(segment) == 6000.0

    def test_unknown_remaining(self):
        assert tqdm_remaining_seconds(b"0%| | 0.0/11.7 [00:00<?, ?seconds/s]") is None


class TestRunProgressProcess:
    def run_fake_cli(self, script: str, **kwargs):
        from stemlab.runtime import run_progress_process

        percents: list[float] = []
        downloads: list[float] = []
        etas: list[float] = []
        lines: list[str] = []

        code = run_progress_process(
            [sys.executable, "-c", script],
            lines.append,
            percents.append,
            eta=etas.append,
            download=downloads.append,
            **kwargs,
        )
        return code, percents, downloads, etas, lines

    def test_roformer_time_estimates_become_percent_and_eta(self):
        script = (
            "import sys\n"
            "sys.stdout.write('Estimated total processing time for this track: "
            "100.00 seconds\\n')\n"
            "sys.stdout.write('\\rEstimated time remaining: 75.00 seconds')\n"
            "sys.stdout.write('\\rEstimated time remaining: 25.00 seconds')\n"
            "sys.stdout.write('\\n')\n"
        )
        code, percents, downloads, etas, _ = self.run_fake_cli(script)

        assert code == 0
        assert etas == [100.0, 75.0, 25.0]
        assert percents == [0.0, 25.0, 75.0]
        assert downloads == []

    def test_download_bar_goes_to_download_callback(self):
        script = (
            "import sys\n"
            "sys.stdout.write('\\rmodel:  10%|#| 270M/2.70G [00:05<00:45, 53.9MB/s]')\n"
            "sys.stdout.write('\\rmodel: 100%|#| 2.70G/2.70G [00:50<00:00, 54.1MB/s]')\n"
            "sys.stdout.write('\\n50%|####| 5.85/11.7 [00:10<00:10, 1.75s/seconds]\\n')\n"
        )
        code, percents, downloads, etas, _ = self.run_fake_cli(script)

        assert code == 0
        assert downloads == [10.0, 100.0]
        assert percents == [50.0]
        assert etas == [10.0]

    def test_only_the_first_frame_at_a_reading_reaches_the_log(self):
        # tqdm redraws its bar many times per percent. Every frame crosses the
        # pipe into the plugin's diagnostics log, and every frame after the
        # first at a reading says exactly what that one said.
        script = (
            "import sys\n"
            "out = sys.stdout\n"
            "out.write('Loading model weights\\n')\n"
            "for frame in range(30):\n"
            "    out.write('\\rSeparating:  25%|' + '#' * (frame % 8)\n"
            "              + '| 3.0/12.0 [00:03<00:09, 1.2s/seconds]')\n"
            "for frame in range(30):\n"
            "    out.write('\\rSeparating:  26%|' + '#' * (frame % 8)\n"
            "              + '| 3.1/12.0 [00:03<00:09, 1.2s/seconds]')\n"
            "out.write('\\nWarning: falling back to cpu\\n')\n"
            "out.write('STEMLAB_PROGRESS 26% still going\\n')\n"
            "out.write('STEMLAB_PROGRESS 26% still going\\n')\n"
            "out.write('RuntimeError: 26% of the graph failed\\n')\n"
        )
        code, percents, downloads, etas, lines = self.run_fake_cli(script)

        assert code == 0
        assert percents == [25.0, 26.0]
        assert downloads == []

        # One frame per reading, out of sixty written.
        assert len([line for line in lines if "|" in line]) == 2

        # Everything that is not a redraw survives whole, including a
        # protocol line the plugin parses, a repeat of it, and an error that
        # happens to carry the reading last logged.
        assert [line for line in lines if "|" not in line] == [
            "Loading model weights",
            "Warning: falling back to cpu",
            "STEMLAB_PROGRESS 26% still going",
            "STEMLAB_PROGRESS 26% still going",
            "RuntimeError: 26% of the graph failed",
        ]

    def test_a_download_bar_is_logged_once_per_percent(self):
        script = (
            "import sys\n"
            "out = sys.stdout\n"
            "for frame in range(20):\n"
            "    out.write('\\rmodel:  10%|#| 270M/2.70G [00:05<00:45, 53.9MB/s]')\n"
            "out.write('\\rmodel:  11%|#| 297M/2.70G [00:05<00:45, 53.9MB/s]')\n"
            "out.write('\\n')\n"
        )
        code, _percents, downloads, _etas, lines = self.run_fake_cli(script)

        assert code == 0
        assert downloads == [10.0, 11.0]
        assert len(lines) == 2

    def test_exit_code_is_returned(self):
        code, *_ = self.run_fake_cli("raise SystemExit(3)")
        assert code == 3


class TestCancelWatchdog:
    def test_sentinel_kills_job_with_cancel_exit_code(self, tmp_path: Path):
        job_dir = tmp_path / "job"
        job_dir.mkdir()

        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys, time\n"
                "from stemlab.runtime import start_cancel_watchdog\n"
                f"start_cancel_watchdog({str(job_dir)!r})\n"
                "print('watching', flush=True)\n"
                "time.sleep(30)\n",
            ],
            stdout=subprocess.PIPE,
        )
        try:
            assert process.stdout is not None
            assert process.stdout.readline().strip() == b"watching"

            (job_dir / CANCEL_FILE).write_text("cancel\n", encoding="utf-8")

            deadline = time.monotonic() + 5.0
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)

            assert process.poll() == CANCEL_EXIT_CODE
            assert b"STEMLAB_CANCELLED" in process.stdout.read()
        finally:
            if process.poll() is None:
                process.kill()

    def test_orphaned_job_shuts_itself_down(self, tmp_path: Path):
        # An intermediate process launches the watchdog-carrying job and then
        # exits, exactly like a plugin instance vanishing mid-separation. The
        # job must notice the reparenting and stop on its own.
        #
        # The launcher waits for the job to signal that its watchdog is armed
        # before exiting. Without that the test races itself: the launcher is
        # gone within a millisecond of spawning, long before the job's
        # interpreter has booted, so the reparenting has already happened by
        # the time the watchdog records the parent it is meant to watch - and
        # no change is ever observed. A plugin dying mid-job is the case this
        # protects, and a plugin is alive while its job starts up.
        job_dir = tmp_path / "job"
        job_dir.mkdir()

        armed = job_dir / "armed.txt"
        # What the job saw of its own parent, so that a survival is a report
        # rather than a bare "assert not True". Which half is wrong - the
        # parent it identified, or its ability to watch that parent - is the
        # whole question, and neither is visible from the outside.
        job_diagnosis = job_dir / "job_diagnosis.json"
        launcher_diagnosis = job_dir / "launcher_diagnosis.json"

        job_script = tmp_path / "job_script.py"
        job_script.write_text(
            "import json, os, sys, time\n"
            "from pathlib import Path\n"
            "from stemlab.runtime import start_cancel_watchdog\n"
            "seen = {'platform': sys.platform, 'pid': os.getpid(),\n"
            "        'getppid': os.getppid()}\n"
            "if sys.platform == 'win32':\n"
            "    import ctypes\n"
            "    synchronize = 0x00100000\n"
            "    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)\n"
            "    kernel32.OpenProcess.restype = ctypes.c_void_p\n"
            "    handle = kernel32.OpenProcess(synchronize, False,\n"
            "                                  os.getppid())\n"
            "    seen['open_process'] = handle or 0\n"
            "    seen['last_error'] = ctypes.get_last_error()\n"
            f"Path({str(job_diagnosis)!r}).write_text(json.dumps(seen))\n"
            f"start_cancel_watchdog({str(job_dir)!r})\n"
            f"Path({str(armed)!r}).write_text('armed', encoding='utf-8')\n"
            "time.sleep(30)\n",
            encoding="utf-8",
        )

        launcher_script = tmp_path / "launcher_script.py"
        launcher_script.write_text(
            "import json, os, subprocess, sys, time\n"
            "from pathlib import Path\n"
            # The job must not inherit this process's stdout: the pipe would
            # stay open for as long as the job lives, and the caller reading
            # it would wait on the job rather than on the launcher.
            f"child = subprocess.Popen([sys.executable, {str(job_script)!r}],\n"
            "                         stdout=subprocess.DEVNULL,\n"
            "                         stderr=subprocess.DEVNULL)\n"
            "print(child.pid, flush=True)\n"
            f"Path({str(launcher_diagnosis)!r}).write_text(\n"
            "    json.dumps({'launcher_pid': os.getpid(),\n"
            "                'job_pid': child.pid}))\n"
            f"armed = Path({str(armed)!r})\n"
            "deadline = time.monotonic() + 30.0\n"
            "while not armed.exists() and time.monotonic() < deadline:\n"
            "    time.sleep(0.05)\n",
            encoding="utf-8",
        )

        subprocess.run(
            [sys.executable, str(launcher_script)],
            stdout=subprocess.PIPE,
            timeout=60,
        )

        def diagnosis() -> str:
            parts = []
            for label, path in (
                ("launcher", launcher_diagnosis),
                ("job", job_diagnosis),
            ):
                try:
                    parts.append(f"{label}={path.read_text()}")
                except OSError:
                    parts.append(f"{label}=<not written>")
            return "; ".join(parts)

        assert armed.exists(), f"the job never armed its watchdog. {diagnosis()}"

        # Take the job's word for its own pid and its own parent rather than
        # Popen's. On Windows a venv's Scripts\python.exe is a launcher stub
        # that runs the real interpreter as a child, so Popen reports the
        # stub: the job runs one level deeper, and the process it watches is
        # the stub rather than the launcher. Watching Popen's pid there meant
        # watching the wrong process, and waiting for the launcher to die
        # meant waiting on a process the job had never heard of.
        seen = json.loads(job_diagnosis.read_text())
        job_pid = seen["pid"]
        parent_pid = seen["getppid"]

        def job_alive() -> bool:
            return pid_alive(job_pid)

        try:
            # Whatever the job called its parent is what has to die for this
            # to mean anything. On POSIX that is the launcher and it is
            # already gone, so this does nothing; on Windows it is the stub,
            # which outlives the launcher because it is waiting on the job.
            if pid_alive(parent_pid):
                os.kill(parent_pid, KILL_SIGNAL)

            deadline = time.monotonic() + 15.0
            while job_alive() and time.monotonic() < deadline:
                time.sleep(0.1)

            assert not job_alive(), (
                "the job outlived the parent it was watching: the watchdog "
                f"never saw that parent die. {diagnosis()}"
            )
        finally:
            if job_alive():
                os.kill(job_pid, KILL_SIGNAL)

    def test_a_live_intermediate_parent_keeps_the_job_running(self, tmp_path: Path):
        # The shape that cost three CI rounds to find. On Windows a venv's
        # Scripts\python.exe is a launcher stub that runs the real interpreter
        # as a child, so the tree is launcher -> stub -> job and the stub sits
        # there waiting on the job. The job's parent is the stub, not the
        # launcher, and the stub outlives the launcher by definition.
        #
        # The watchdog is right to keep going there: a parent it can still see
        # is a parent that has not died. What is wrong is any test that spawns
        # through an intermediate and then expects the launcher's exit to be
        # noticed. Standing the intermediate up explicitly puts that on both
        # platforms, so this cannot go back to being a Windows-only discovery
        # made during a release.
        job_dir = tmp_path / "job"
        job_dir.mkdir()

        armed = job_dir / "armed.txt"
        job_diagnosis = job_dir / "job_diagnosis.json"

        job_script = tmp_path / "job_script.py"
        job_script.write_text(
            "import json, os, sys, time\n"
            "from pathlib import Path\n"
            "from stemlab.runtime import start_cancel_watchdog\n"
            "seen = {'pid': os.getpid(), 'getppid': os.getppid()}\n"
            f"Path({str(job_diagnosis)!r}).write_text(json.dumps(seen))\n"
            f"start_cancel_watchdog({str(job_dir)!r})\n"
            f"Path({str(armed)!r}).write_text('armed', encoding='utf-8')\n"
            "time.sleep(30)\n",
            encoding="utf-8",
        )

        # Stands in for the venv launcher stub: spawn the job, then wait on it.
        stub_script = tmp_path / "stub_script.py"
        stub_script.write_text(
            "import subprocess, sys\n"
            f"child = subprocess.Popen([sys.executable, {str(job_script)!r}],\n"
            "                         stdout=subprocess.DEVNULL,\n"
            "                         stderr=subprocess.DEVNULL)\n"
            "child.wait()\n",
            encoding="utf-8",
        )

        launcher_script = tmp_path / "launcher_script.py"
        launcher_script.write_text(
            "import subprocess, sys, time\n"
            "from pathlib import Path\n"
            f"subprocess.Popen([sys.executable, {str(stub_script)!r}],\n"
            "                 stdout=subprocess.DEVNULL,\n"
            "                 stderr=subprocess.DEVNULL)\n"
            f"armed = Path({str(armed)!r})\n"
            "deadline = time.monotonic() + 30.0\n"
            "while not armed.exists() and time.monotonic() < deadline:\n"
            "    time.sleep(0.05)\n",
            encoding="utf-8",
        )

        subprocess.run([sys.executable, str(launcher_script)], timeout=60)

        assert armed.exists(), "the job never armed its watchdog"
        seen = json.loads(job_diagnosis.read_text())
        job_pid = seen["pid"]
        stub_pid = seen["getppid"]

        def job_alive() -> bool:
            return pid_alive(job_pid)

        try:
            # The launcher is gone. The stub is not, so neither is the job.
            time.sleep(2.0)
            assert pid_alive(stub_pid), "the stub should still be waiting"
            assert job_alive(), (
                "the job stopped while the process it watches was still alive"
            )

            os.kill(stub_pid, KILL_SIGNAL)

            deadline = time.monotonic() + 15.0
            while job_alive() and time.monotonic() < deadline:
                time.sleep(0.1)
            assert not job_alive(), "the job outlived the parent it was watching"
        finally:
            for pid in (job_pid, stub_pid):
                if pid_alive(pid):
                    os.kill(pid, KILL_SIGNAL)

    def test_named_parent_death_stops_the_job(self, tmp_path: Path):
        # What the plugin actually uses: it exports its own pid, so the job
        # watches a named process rather than watching for reparenting. This
        # has no startup race - the pid is meaningful whether or not the
        # parent is still alive when the watchdog reads it.
        job_dir = tmp_path / "job"
        job_dir.mkdir()

        # A parent that is already gone: the job must stop rather than run on
        # believing someone is still waiting for it.
        doomed = subprocess.Popen([sys.executable, "-c", "import sys; sys.exit(0)"])
        doomed.wait()

        environment = dict(os.environ)
        environment["STEMLAB_PARENT_PID"] = str(doomed.pid)

        job = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import time\n"
                "from stemlab.runtime import start_cancel_watchdog\n"
                f"start_cancel_watchdog({str(job_dir)!r})\n"
                "time.sleep(30)\n",
            ],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        try:
            deadline = time.monotonic() + 15.0
            while job.poll() is None and time.monotonic() < deadline:
                time.sleep(0.1)

            assert job.poll() == ORPHAN_EXIT_CODE
        finally:
            if job.poll() is None:
                job.kill()

    def test_pre_existing_sentinel_still_cancels(self, tmp_path: Path):
        job_dir = tmp_path / "job"
        job_dir.mkdir()
        (job_dir / CANCEL_FILE).write_text("cancel\n", encoding="utf-8")

        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import time\n"
                "from stemlab.runtime import start_cancel_watchdog\n"
                f"start_cancel_watchdog({str(job_dir)!r})\n"
                "time.sleep(30)\n",
            ],
        )
        try:
            deadline = time.monotonic() + 5.0
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)

            assert process.poll() == CANCEL_EXIT_CODE
        finally:
            if process.poll() is None:
                process.kill()
