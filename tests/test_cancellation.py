from __future__ import annotations

import sys
import threading
import time

import pytest

from stemlab.runtime import (
    CancellationToken,
    JobCancelled,
    child_process_env,
    run_progress_process,
)


def test_cancel_stops_only_the_requested_child_process(tmp_path):
    cancel_file = tmp_path / "cancel.request"
    token = CancellationToken(cancel_file)

    def request_cancel():
        time.sleep(0.2)
        cancel_file.write_text("cancel\n", encoding="utf-8")

    requester = threading.Thread(target=request_cancel)
    requester.start()
    started = time.monotonic()

    with pytest.raises(JobCancelled):
        run_progress_process(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            lambda _line: None,
            lambda _percent: None,
            cancellation=token,
        )

    requester.join(timeout=1.0)
    assert time.monotonic() - started < 4.0


def test_child_process_env_forces_utf8(monkeypatch):
    monkeypatch.setenv("PYTHONUTF8", "0")
    monkeypatch.setenv("PYTHONIOENCODING", "cp1252")

    env = child_process_env()

    assert env["PYTHONUTF8"] == "1"
    assert env["PYTHONIOENCODING"] == "utf-8"


def test_child_python_can_print_unicode_status_symbol():
    lines: list[str] = []
    exit_code = run_progress_process(
        [sys.executable, "-c", "print('\u2713 Copied packaged config')"],
        lines.append,
        lambda _percent: None,
    )

    assert exit_code == 0
    assert any("✓ Copied packaged config" in line for line in lines)
