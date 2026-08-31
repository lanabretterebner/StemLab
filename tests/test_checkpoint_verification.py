"""The packaged Beat This! checkpoint is hashed once, not once per run.

beat_tracking imports torch at module scope and torch is not a declared
dependency, so a stub stands in: nothing exercised here touches it.
"""

from __future__ import annotations

import sys
import types

import pytest

if "torch" not in sys.modules:  # pragma: no cover - depends on the environment
    stub = types.ModuleType("torch")
    stub.nn = types.SimpleNamespace(Module=object)
    sys.modules["torch"] = stub

from stemlab import beat_tracking  # noqa: E402
from stemlab.beat_tracking import MODEL_SPECS, resolve_packaged_model  # noqa: E402


@pytest.fixture
def packaged(tmp_path, monkeypatch):
    """A model directory holding a checkpoint whose digest the spec pins."""
    mode = next(iter(MODEL_SPECS))
    spec = MODEL_SPECS[mode]

    payload = b"x" * spec.size
    checkpoint = tmp_path / f"{spec.name}.ckpt"
    checkpoint.write_bytes(payload)

    import hashlib

    monkeypatch.setattr(
        beat_tracking, "MODEL_SPECS", {mode: type(spec)(spec.name, spec.size, hashlib.sha256(payload).hexdigest())}
    )
    return mode, checkpoint


def _count_hashes(monkeypatch):
    calls: list[str] = []
    real = beat_tracking._sha256_file

    def counting(path):
        calls.append(str(path))
        return real(path)

    monkeypatch.setattr(beat_tracking, "_sha256_file", counting)
    return calls


def test_second_resolve_skips_the_hash(packaged, monkeypatch, tmp_path):
    mode, checkpoint = packaged
    calls = _count_hashes(monkeypatch)

    assert resolve_packaged_model(mode, tmp_path) == checkpoint.resolve()
    assert len(calls) == 1, "the first resolve must actually hash the file"

    assert resolve_packaged_model(mode, tmp_path) == checkpoint.resolve()
    assert len(calls) == 1, "the second resolve must trust the recorded digest"


def test_a_rewritten_checkpoint_is_hashed_again(packaged, monkeypatch, tmp_path):
    mode, checkpoint = packaged
    calls = _count_hashes(monkeypatch)

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 1

    # Same size, same content, new mtime: the record no longer describes it.
    import os

    stat = checkpoint.stat()
    os.utime(checkpoint, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 2, "a changed mtime must not be trusted"


def test_a_corrupt_record_is_ignored(packaged, monkeypatch, tmp_path):
    mode, checkpoint = packaged
    calls = _count_hashes(monkeypatch)

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 1

    beat_tracking._verification_path(checkpoint).write_text("{ not json", encoding="utf-8")

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 2, "an unreadable record means unverified, not verified"


def test_a_record_naming_another_digest_is_ignored(packaged, monkeypatch, tmp_path):
    """The record is only ever a shortcut to the digest the spec already pins."""
    import json

    mode, checkpoint = packaged
    calls = _count_hashes(monkeypatch)

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 1

    stat = checkpoint.stat()
    beat_tracking._verification_path(checkpoint).write_text(
        json.dumps({"sha256": "0" * 64, "size": stat.st_size, "mtime_ns": stat.st_mtime_ns}),
        encoding="utf-8",
    )

    resolve_packaged_model(mode, tmp_path)
    assert len(calls) == 2, "a record must not be able to assert a different digest"


def test_a_tampered_checkpoint_still_fails(packaged, monkeypatch, tmp_path):
    """A record can skip work already done; it can never pass a bad file."""
    mode, checkpoint = packaged

    resolve_packaged_model(mode, tmp_path)

    spec = beat_tracking.MODEL_SPECS[mode]
    checkpoint.write_bytes(b"y" * spec.size)

    with pytest.raises(RuntimeError, match="SHA-256"):
        resolve_packaged_model(mode, tmp_path)
