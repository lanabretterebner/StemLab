"""A truncated model download must not poison every later run.

audio-separator's download_file_if_not_exists writes straight to the final
path and returns early whenever that path already exists, so an interrupted
transfer leaves a short file it will then reuse for ever. These tests drive
_load_model against a stand-in with exactly that behaviour.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from stemlab.recursive import _discard_unusable_downloads, _load_model

MODEL = "dereverb_mel_band_roformer_less_aggressive_anvuew_sdr_18.8050.ckpt"
GOOD = b"x" * 4096


class FakeSeparator:
    """Mimics audio-separator: skip the download if the path exists, then load."""

    def __init__(self, model_dir: Path) -> None:
        self.model_dir = model_dir
        self.downloads = 0
        self.loads = 0

    def load_model(self, model_filename: str) -> None:
        self.loads += 1
        path = self.model_dir / model_filename
        if not path.is_file():  # download_file_if_not_exists
            self.downloads += 1
            path.write_bytes(GOOD)
            (self.model_dir / f"{Path(model_filename).stem}.yaml").write_text("cfg\n")
        if path.stat().st_size != len(GOOD):
            raise RuntimeError(f"Invalid checksum for file {path}")


def test_a_truncated_model_is_replaced_rather_than_reused(tmp_path):
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    # What a cancelled download leaves behind.
    (model_dir / MODEL).write_bytes(b"x" * 100)

    separator = FakeSeparator(model_dir)
    _load_model(separator, MODEL, "vocal de-reverb", None, model_dir)

    assert separator.loads == 2, "the first load must fail and be retried"
    assert separator.downloads == 1, "the retry must actually re-download"
    assert (model_dir / MODEL).read_bytes() == GOOD


def test_a_healthy_model_loads_once_and_is_never_deleted(tmp_path):
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    (model_dir / MODEL).write_bytes(GOOD)

    separator = FakeSeparator(model_dir)
    _load_model(separator, MODEL, "vocal de-reverb", None, model_dir)

    assert (separator.loads, separator.downloads) == (1, 0)
    assert (model_dir / MODEL).is_file()


def test_a_failure_with_nothing_cached_is_not_retried(tmp_path):
    """No network, a missing dependency: re-downloading only fails slower."""
    model_dir = tmp_path / "models"
    model_dir.mkdir()

    class AlwaysFails(FakeSeparator):
        def load_model(self, model_filename: str) -> None:
            self.loads += 1
            raise RuntimeError("onnxruntime is not installed")

    separator = AlwaysFails(model_dir)
    with pytest.raises(RuntimeError, match="onnxruntime"):
        _load_model(separator, MODEL, "vocal de-reverb", None, model_dir)

    assert separator.loads == 1, "nothing was cached, so there was nothing to retry"


def test_the_purge_takes_the_config_beside_the_checkpoint(tmp_path):
    """A roformer checkpoint without its yaml is as unusable as a short one."""
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    (model_dir / MODEL).write_bytes(b"short")
    (model_dir / f"{Path(MODEL).stem}.yaml").write_text("cfg\n")
    keep = model_dir / "some_other_model.ckpt"
    keep.write_bytes(b"unrelated")

    discarded = _discard_unusable_downloads(model_dir, MODEL)

    assert sorted(discarded) == sorted([MODEL, f"{Path(MODEL).stem}.yaml"])
    assert keep.is_file(), "another model's cache must survive"


def test_a_corrupt_registry_is_discarded_but_a_valid_one_is_kept(tmp_path):
    model_dir = tmp_path / "models"
    model_dir.mkdir()
    (model_dir / MODEL).write_bytes(b"short")

    registry = model_dir / "download_checks.json"
    registry.write_text('{"ok": true}')
    assert "download_checks.json" not in _discard_unusable_downloads(model_dir, MODEL)
    assert registry.is_file(), "a parseable registry is shared and must be kept"

    (model_dir / MODEL).write_bytes(b"short")
    registry.write_text("{ truncated")
    assert "download_checks.json" in _discard_unusable_downloads(model_dir, MODEL)
    assert not registry.exists()
