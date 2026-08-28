from __future__ import annotations

from pathlib import Path

from stemlab.audio import STEM_NAMES
from stemlab.demucs_backend import (
    DEFAULT_DEMUCS_MODEL,
    PACKAGED_DEMUCS_FILENAME,
    PACKAGED_DEMUCS_SIGNATURE,
    DemucsBackend,
)


def test_packaged_demucs_uses_local_repo_and_signature(tmp_path, monkeypatch):
    source = tmp_path / "source.wav"
    source.write_bytes(b"audio-fixture")
    output = tmp_path / "out"
    repo = tmp_path / "demucs-repo"
    repo.mkdir()
    (repo / PACKAGED_DEMUCS_FILENAME).write_bytes(b"checkpoint-fixture")

    monkeypatch.setenv("STEMLAB_DEMUCS_MODEL_REPO", str(repo))
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

    assert len(commands) == 2
    assert [path.name for path in results] == [f"{stem}.wav" for stem in STEM_NAMES]
    assert all(path.is_file() for path in results)


def test_scratch_directory_opens_beside_the_output(tmp_path):
    """So placing the stems is a rename rather than six whole-file copies."""
    from stemlab.demucs_backend import _work_directory

    output = tmp_path / "job" / "baseline"
    output.mkdir(parents=True)

    with _work_directory(output) as scratch:
        assert Path(scratch).parent == output.parent


def test_scratch_directory_falls_back_when_the_parent_will_not_take_it(tmp_path, monkeypatch):
    """A read-only job tree still has to yield a working separation."""
    import tempfile

    from stemlab.demucs_backend import _work_directory

    output = tmp_path / "job" / "baseline"
    output.mkdir(parents=True)

    real = tempfile.TemporaryDirectory
    calls: list[Path | None] = []

    def refuse_the_parent(*args, **kwargs):
        calls.append(kwargs.get("dir"))
        if kwargs.get("dir") is not None:
            raise OSError("read-only file system")
        return real(*args, **kwargs)

    monkeypatch.setattr(tempfile, "TemporaryDirectory", refuse_the_parent)

    with _work_directory(output) as scratch:
        # Fell back to the system temp root rather than raising.
        assert Path(scratch).is_dir()
        assert Path(scratch).parent != output.parent

    assert calls == [output.parent, None]


def test_stems_are_moved_out_of_the_scratch_directory(tmp_path, monkeypatch):
    """The scratch copy must not survive: it is the same six full-length files."""
    source = tmp_path / "source.wav"
    source.write_bytes(b"audio-fixture")
    output = tmp_path / "job" / "baseline"

    monkeypatch.setattr("stemlab.demucs_backend.resolve_torch_device", lambda *_args: "cpu")
    monkeypatch.setattr(
        "stemlab.demucs_backend._normalise_input_for_backend",
        lambda input_path, staging_dir, **_kwargs: input_path,
    )

    seen_raw_output: list[Path] = []

    def fake_run(command, _log, _progress, **_kwargs):
        command = list(command)
        if "demucs.separate" in command:
            raw_output = Path(command[command.index("--out") + 1])
            seen_raw_output.append(raw_output)
            for stem in STEM_NAMES:
                path = raw_output / "htdemucs_6s" / source.stem / f"{stem}.wav"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(stem.encode("ascii"))
        return 0

    monkeypatch.setattr("stemlab.demucs_backend.run_progress_process", fake_run)

    results = DemucsBackend(model=DEFAULT_DEMUCS_MODEL, device="cpu").separate(source, output)

    assert all(path.is_file() for path in results)
    assert [path.read_bytes() for path in results] == [s.encode("ascii") for s in STEM_NAMES]
    # Moved, not copied: nothing is left behind in the scratch tree, which the
    # context manager has since removed entirely.
    assert seen_raw_output and not seen_raw_output[0].exists()
