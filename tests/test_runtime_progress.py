"""Tests for the model-output parsing and cancel plumbing in stemlab.runtime."""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

from stemlab.runtime import (
    CANCEL_EXIT_CODE,
    CANCEL_FILE,
    last_progress_percent,
    looks_like_download,
    tqdm_remaining_seconds,
)


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
        import os
        import signal

        job_dir = tmp_path / "job"
        job_dir.mkdir()

        job_script = tmp_path / "job_script.py"
        job_script.write_text(
            "import time\n"
            "from stemlab.runtime import start_cancel_watchdog\n"
            f"start_cancel_watchdog({str(job_dir)!r})\n"
            "time.sleep(30)\n",
            encoding="utf-8",
        )
        launcher_script = tmp_path / "launcher_script.py"
        launcher_script.write_text(
            "import subprocess, sys\n"
            f"child = subprocess.Popen([sys.executable, {str(job_script)!r}])\n"
            "print(child.pid, flush=True)\n",
            encoding="utf-8",
        )

        launcher = subprocess.run(
            [sys.executable, str(launcher_script)],
            stdout=subprocess.PIPE,
            timeout=15,
        )
        job_pid = int(launcher.stdout.strip())

        def job_alive() -> bool:
            try:
                os.kill(job_pid, 0)
            except ProcessLookupError:
                return False
            return True

        try:
            deadline = time.monotonic() + 5.0
            while job_alive() and time.monotonic() < deadline:
                time.sleep(0.1)
            assert not job_alive()
        finally:
            if job_alive():
                os.kill(job_pid, signal.SIGKILL)

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
