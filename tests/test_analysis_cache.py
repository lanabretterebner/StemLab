from __future__ import annotations

import hashlib
import sqlite3

import pytest

import stemlab.analysis_cache as analysis_cache_module
from stemlab.analysis_cache import AnalysisCache
from stemlab.beat_tracking import (
    MODEL_SPECS,
    ModelSpec,
    derive_musical_time,
    resolve_packaged_model,
)
from stemlab.runtime import CancellationToken, JobCancelled


def test_packaged_fast_and_accurate_models_resolve_without_network(tmp_path, monkeypatch):
    payloads = {"fast": b"small model", "accurate": b"accurate model"}
    specs = {
        # A url the spec could download from, deliberately unreachable: the
        # point of this test is that resolving a checkpoint already on disk
        # never reaches for it.
        mode: ModelSpec(
            name,
            len(payload),
            hashlib.sha256(payload).hexdigest(),
            f"https://models.invalid/{name}.ckpt",
        )
        for (mode, name), payload in zip(
            (("fast", "small0"), ("accurate", "final0")),
            payloads.values(),
            strict=True,
        )
    }
    monkeypatch.setattr("stemlab.beat_tracking.MODEL_SPECS", specs)
    monkeypatch.setattr(
        "socket.create_connection",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(AssertionError("network attempted")),
    )
    for mode, payload in payloads.items():
        path = tmp_path / f"{specs[mode].name}.ckpt"
        path.write_bytes(payload)
        assert resolve_packaged_model(mode, tmp_path) == path.resolve()


def test_tempo_candidates_meter_downbeats_and_bar_one():
    beats = [0.25 + index * 0.5 for index in range(20)]
    beats[9] += 0.22  # One obvious interval outlier must not skew the median.
    downbeats = [0.25 + index * 1.5 for index in range(7)]
    result = derive_musical_time(
        beats,
        downbeats,
        0.9,
        model="small0",
        device="cpu",
    )
    assert 119.0 <= result.detected_bpm <= 121.0
    assert result.half_time_bpm == pytest.approx(result.detected_bpm / 2)
    assert result.double_time_bpm == pytest.approx(result.detected_bpm * 2)
    assert result.meter_numerator == 3
    assert result.meter_denominator == 4
    assert result.bar_one == pytest.approx(0.25)
    assert result.confidence > 0.7


def test_analysis_cache_invalidation_and_local_correction(tmp_path):
    cache = AnalysisCache(tmp_path / "analysis.sqlite3")
    source_hash = "abc123"
    cache.put_result("beats", source_hash, "v1", {"mode": "fast"}, {"bpm": 120})

    assert cache.get_result("beats", source_hash, "v1", {"mode": "fast"}) == {"bpm": 120}
    assert cache.get_result("beats", source_hash, "v2", {"mode": "fast"}) is None
    assert cache.get_result("beats", source_hash, "v1", {"mode": "accurate"}) is None

    correction = {
        "bpm": 128.0,
        "key": "G minor",
        "meter_numerator": 3,
        "meter_denominator": 4,
        "bar_one": 0.25,
    }
    cache.set_correction(source_hash, correction)
    assert cache.get_correction(source_hash) == correction
    assert cache.forget_correction(source_hash)
    assert cache.get_correction(source_hash) is None



def test_analysis_cache_closes_sqlite_connections(tmp_path, monkeypatch):
    real_connect = sqlite3.connect
    opened = []

    def tracking_connect(*args, **kwargs):
        connection = real_connect(*args, **kwargs)
        opened.append(connection)
        return connection

    monkeypatch.setattr(analysis_cache_module.sqlite3, "connect", tracking_connect)
    cache = AnalysisCache(tmp_path / "analysis.sqlite3")
    cache.put_result("beats", "abc", "v1", {"mode": "fast"}, {"bpm": 120})
    assert cache.get_result("beats", "abc", "v1", {"mode": "fast"}) == {"bpm": 120}

    assert opened
    for connection in opened:
        with pytest.raises(sqlite3.ProgrammingError):
            connection.execute("SELECT 1")

def test_analysis_cancellation_is_checked_before_model_loading(tmp_path):
    marker = tmp_path / "cancel.request"
    marker.write_text("cancel", encoding="utf-8")
    with pytest.raises(JobCancelled):
        CancellationToken(marker).raise_if_cancelled()


def test_model_manifest_contains_requested_official_models():
    assert MODEL_SPECS["fast"].name == "small0"
    assert MODEL_SPECS["accurate"].name == "final0"
