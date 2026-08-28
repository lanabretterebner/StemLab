"""Tests for remembering that a Beat This! checkpoint has been verified.

The digest is the whole security story for a downloaded checkpoint, and
taking it over a multi-hundred-megabyte file on every run is not free. What
is cached here is the fact that a particular file already hashed to the
value the spec pins - never the decision to load an unverified one.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import stemlab.beat_tracking as beat_tracking
from stemlab.beat_tracking import ModelSpec, resolve_packaged_model

PAYLOAD = b"a plausible checkpoint" * 500


@pytest.fixture
def checkpoint(tmp_path: Path, monkeypatch):
    """A checkpoint on disk that matches the only spec resolution will see."""
    monkeypatch.setattr(
        "stemlab.beat_tracking.MODEL_SPECS",
        {
            "fast": ModelSpec(
                "small0",
                len(PAYLOAD),
                hashlib.sha256(PAYLOAD).hexdigest(),
                "https://models.invalid/small0.ckpt",
            )
        },
    )
    path = tmp_path / "small0.ckpt"
    path.write_bytes(PAYLOAD)
    return path


@pytest.fixture
def digests(monkeypatch):
    """Every full-file digest resolution takes, in order."""
    taken: list[str] = []
    real = beat_tracking._sha256_file
    monkeypatch.setattr(
        "stemlab.beat_tracking._sha256_file",
        lambda path: (taken.append(str(path)), real(path))[1],
    )
    return taken


def test_a_verified_checkpoint_is_hashed_once(checkpoint: Path, digests):
    assert resolve_packaged_model("fast", checkpoint.parent) == checkpoint.resolve()
    assert len(digests) == 1

    for _ in range(3):
        assert resolve_packaged_model("fast", checkpoint.parent) == checkpoint.resolve()
    assert len(digests) == 1


def test_a_modified_checkpoint_is_hashed_again_and_rejected(checkpoint: Path, digests):
    resolve_packaged_model("fast", checkpoint.parent)
    digests.clear()

    # Same length, different bytes: nothing but the digest can catch this,
    # and the record must not be what stops it being taken.
    checkpoint.write_bytes(PAYLOAD[:-1] + b"X")

    with pytest.raises(RuntimeError, match="SHA-256"):
        resolve_packaged_model("fast", checkpoint.parent)
    assert len(digests) == 1


def test_a_truncated_checkpoint_is_still_refused_on_size(checkpoint: Path, digests):
    resolve_packaged_model("fast", checkpoint.parent)
    digests.clear()

    checkpoint.write_bytes(PAYLOAD[:-16])

    with pytest.raises(RuntimeError, match="wrong size"):
        resolve_packaged_model("fast", checkpoint.parent)
    assert digests == []


def test_a_record_from_another_file_is_not_trusted(checkpoint: Path, digests):
    resolve_packaged_model("fast", checkpoint.parent)
    record_path = beat_tracking._verification_path(checkpoint)
    record = json.loads(record_path.read_text(encoding="utf-8"))
    digests.clear()

    # A record whose stat belongs to some other file cannot vouch for this
    # one, however well-formed it is.
    record_path.write_text(
        json.dumps({**record, "mtime_ns": int(record["mtime_ns"]) + 1}),
        encoding="utf-8",
    )
    resolve_packaged_model("fast", checkpoint.parent)
    assert len(digests) == 1

    record_path.write_text(json.dumps({**record, "size": int(record["size"]) + 1}), encoding="utf-8")
    digests.clear()
    resolve_packaged_model("fast", checkpoint.parent)
    assert len(digests) == 1


def test_a_record_naming_the_wrong_digest_is_not_trusted(checkpoint: Path, digests):
    resolve_packaged_model("fast", checkpoint.parent)
    record_path = beat_tracking._verification_path(checkpoint)
    record = json.loads(record_path.read_text(encoding="utf-8"))
    digests.clear()

    # Whatever this file once hashed to, it is not what the spec pins now.
    record_path.write_text(json.dumps({**record, "sha256": "0" * 64}), encoding="utf-8")

    resolve_packaged_model("fast", checkpoint.parent)
    assert len(digests) == 1


def test_a_corrupt_record_falls_back_to_hashing(checkpoint: Path, digests):
    resolve_packaged_model("fast", checkpoint.parent)
    digests.clear()

    beat_tracking._verification_path(checkpoint).write_text("{ not json", encoding="utf-8")

    assert resolve_packaged_model("fast", checkpoint.parent) == checkpoint.resolve()
    assert len(digests) == 1


def test_a_record_that_cannot_be_written_costs_only_the_hash(checkpoint: Path, digests):
    # Standing a directory in the record's place is the portable way to have
    # every write of it fail. A model directory that will not take the record
    # must still resolve the model, and must leave no scratch file behind.
    beat_tracking._verification_path(checkpoint).mkdir()

    for _ in range(2):
        assert resolve_packaged_model("fast", checkpoint.parent) == checkpoint.resolve()
    assert len(digests) == 2
    assert [path.name for path in checkpoint.parent.iterdir() if path.suffix == ".tmp"] == []
