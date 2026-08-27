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
