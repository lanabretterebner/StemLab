from pathlib import Path

import stemlab.plugin_job as plugin_job


def test_progress_replace_permission_error_is_nonfatal(
    tmp_path,
    monkeypatch,
):
    attempts = {"count": 0}

    def always_locked(source, destination):
        attempts["count"] += 1
        raise PermissionError(5, "Access is denied")

    monkeypatch.setattr(
        plugin_job.os,
        "replace",
        always_locked,
    )

    # The regression: this used to propagate PermissionError and kill the
    # entire separator subprocess.
    plugin_job.write_progress(
        tmp_path,
        97.0,
        "Demucs (97%)",
    )

    assert attempts["count"] >= 2


def test_progress_writes_expected_payload(tmp_path):
    plugin_job.write_progress(
        tmp_path,
        42.5,
        "Separating",
    )

    target = Path(tmp_path) / "stemlab_progress.txt"
    assert target.exists()

    text = target.read_text(encoding="utf-8")
    assert text.startswith("42.5\n")
    assert "Separating" in text
