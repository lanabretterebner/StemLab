from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path

import pytest

_SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "stage_models.py"
_SPEC = importlib.util.spec_from_file_location("stemlab_stage_models", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
_STAGE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_STAGE)


def _manifest(tmp_path: Path, relative: str, payload: bytes) -> Path:
    path = tmp_path / "models.json"
    path.write_text(
        json.dumps(
            {
                "schema": 1,
                "models": [
                    {
                        "purpose": "fixture",
                        "file": relative,
                        "bytes": len(payload),
                        "sha256": hashlib.sha256(payload).hexdigest(),
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    return path


def test_stage_models_copies_and_verifies_local_asset(tmp_path):
    payload = b"verified model fixture"
    source = tmp_path / "cache"
    source.mkdir()
    (source / "fixture.ckpt").write_bytes(payload)
    engine = tmp_path / "Engine"

    staged = _STAGE.stage_models(
        _manifest(tmp_path, "BeatThis/fixture.ckpt", payload),
        engine,
        source_roots=[source],
    )

    assert staged == [engine / "Models" / "BeatThis" / "fixture.ckpt"]
    assert staged[0].read_bytes() == payload


def test_stage_models_rejects_bad_hash(tmp_path):
    payload = b"expected"
    engine = tmp_path / "Engine"
    destination = engine / "Models" / "BeatThis" / "fixture.ckpt"
    destination.parent.mkdir(parents=True)
    destination.write_bytes(b"wrong")

    with pytest.raises(RuntimeError, match="Wrong size|SHA-256"):
        _STAGE.stage_models(_manifest(tmp_path, "BeatThis/fixture.ckpt", payload), engine)


def test_model_destination_cannot_escape_engine(tmp_path):
    with pytest.raises(ValueError, match="escapes"):
        _STAGE.destination_for(tmp_path / "Engine", "../../outside.ckpt")


def test_stage_models_replaces_corrupt_destination_from_verified_cache(tmp_path):
    payload = b"verified replacement"
    source = tmp_path / "cache"
    source.mkdir()
    (source / "fixture.ckpt").write_bytes(payload)

    engine = tmp_path / "Engine"
    destination = engine / "Models" / "BeatThis" / "fixture.ckpt"
    destination.parent.mkdir(parents=True)
    destination.write_bytes(b"corrupt")

    staged = _STAGE.stage_models(
        _manifest(tmp_path, "BeatThis/fixture.ckpt", payload),
        engine,
        source_roots=[source],
    )

    assert staged == [destination]
    assert destination.read_bytes() == payload
