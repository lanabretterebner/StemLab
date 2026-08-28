from __future__ import annotations

import errno
import shutil
from pathlib import Path

from stemlab.audio import STEM_NAMES
from stemlab.demucs_backend import (
    DEFAULT_DEMUCS_MODEL,
    PACKAGED_DEMUCS_FILENAME,
    PACKAGED_DEMUCS_SIGNATURE,
    DemucsBackend,
)


def _stub_separation(monkeypatch, source: Path) -> list[Path]:
    """Stand in for the model: write the six stems, report where they went."""
    monkeypatch.setattr("stemlab.demucs_backend._demucs_available", lambda: True)
    monkeypatch.setattr("stemlab.demucs_backend.resolve_torch_device", lambda *_args: "cpu")
    monkeypatch.setattr(
        "stemlab.demucs_backend._normalise_input_for_backend",
        lambda input_path, staging_dir, **_kwargs: input_path,
    )

    raw_outputs: list[Path] = []

    def fake_run(command, _log, _progress, **_kwargs):
        raw_output = Path(list(command)[list(command).index("--out") + 1])
        raw_outputs.append(raw_output)
        for stem in STEM_NAMES:
            path = raw_output / DEFAULT_DEMUCS_MODEL / source.stem / f"{stem}.wav"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(stem.encode("ascii"))
        return 0

    monkeypatch.setattr("stemlab.demucs_backend.run_progress_process", fake_run)
    return raw_outputs


def _count_copies(monkeypatch) -> list[str]:
    """Every whole-file copy made while the separation lands its stems."""
    copied: list[str] = []
    real = shutil.copy2

    def counted(source, destination, **kwargs):
        copied.append(str(source))
        return real(source, destination, **kwargs)

    monkeypatch.setattr("stemlab.demucs_backend.shutil.copy2", counted)
    return copied


def test_stems_are_moved_off_a_work_directory_beside_the_output(tmp_path, monkeypatch):
    source = tmp_path / "source.wav"
    source.write_bytes(b"audio-fixture")
    output = tmp_path / "job" / "stems"

    raw_outputs = _stub_separation(monkeypatch, source)
    copied = _count_copies(monkeypatch)

    results = DemucsBackend(device="cpu").separate(source, output)

    # Six full-length stems are written into the work directory and then have
    # to end up in the output. That is a rename only while the two are on one
    # filesystem, which is what anchoring the work directory buys.
    work = raw_outputs[0].parent
    assert work.parent == output.parent.resolve()
    assert copied == []
    assert not list(raw_outputs[0].rglob("*.wav"))
    assert [path.read_bytes() for path in results] == [
        stem.encode("ascii") for stem in STEM_NAMES
    ]
    assert not work.exists()


def test_a_rename_that_cannot_cross_a_filesystem_falls_back_to_copying(tmp_path, monkeypatch):
    source = tmp_path / "source.wav"
    source.write_bytes(b"audio-fixture")
    output = tmp_path / "job" / "stems"

    _stub_separation(monkeypatch, source)
    copied = _count_copies(monkeypatch)

    def refuse(_self, _target):
        raise OSError(errno.EXDEV, "Invalid cross-device link")

    monkeypatch.setattr(Path, "replace", refuse)

    results = DemucsBackend(device="cpu").separate(source, output)

    assert len(copied) == len(STEM_NAMES)
    assert [path.read_bytes() for path in results] == [
        stem.encode("ascii") for stem in STEM_NAMES
    ]


def test_packaged_demucs_uses_local_repo_and_signature(tmp_path, monkeypatch):
    source = tmp_path / "source.wav"
    source.write_bytes(b"audio-fixture")
    output = tmp_path / "out"
    repo = tmp_path / "demucs-repo"
    repo.mkdir()
    (repo / PACKAGED_DEMUCS_FILENAME).write_bytes(b"checkpoint-fixture")

    monkeypatch.setenv("STEMLAB_DEMUCS_MODEL_REPO", str(repo))
    monkeypatch.setattr("stemlab.demucs_backend._demucs_available", lambda: True)
    monkeypatch.setattr("stemlab.demucs_backend.resolve_torch_device", lambda *_args: "cpu")
    monkeypatch.setattr(
        "stemlab.demucs_backend._normalise_input_for_backend",
        lambda input_path, staging_dir, **_kwargs: input_path,
    )

    commands: list[list[str]] = []

    def fake_run(command, _log, _progress, **_kwargs):
        command = list(command)
        commands.append(command)
        if "demucs.separate" in command:
            assert command[command.index("--name") + 1] == PACKAGED_DEMUCS_SIGNATURE
            assert Path(command[command.index("--repo") + 1]) == repo.resolve()
            raw_output = Path(command[command.index("--out") + 1])
            for stem in STEM_NAMES:
                path = raw_output / PACKAGED_DEMUCS_SIGNATURE / source.stem / f"{stem}.wav"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(stem.encode("ascii"))
        return 0

    monkeypatch.setattr("stemlab.demucs_backend.run_progress_process", fake_run)

    results = DemucsBackend(model=DEFAULT_DEMUCS_MODEL, device="cpu").separate(source, output)

    # The availability probe runs in-process now, so the model invocation is
    # the only subprocess.
    assert len(commands) == 1
    assert [path.name for path in results] == [f"{stem}.wav" for stem in STEM_NAMES]
    assert all(path.is_file() for path in results)
