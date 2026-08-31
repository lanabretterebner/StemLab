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


def test_analysis_cache_invalidation(tmp_path):
    cache = AnalysisCache(tmp_path / "analysis.sqlite3")
    source_hash = "abc123"
    cache.put_result("beats", source_hash, "v1", {"mode": "fast"}, {"bpm": 120})

    assert cache.get_result("beats", source_hash, "v1", {"mode": "fast"}) == {"bpm": 120}
    assert cache.get_result("beats", source_hash, "v2", {"mode": "fast"}) is None
    assert cache.get_result("beats", source_hash, "v1", {"mode": "accurate"}) is None


def test_schema_1_corrections_table_is_dropped_on_open(tmp_path):
    """An old cache must not keep overriding an analysis nothing can edit.

    Schema 1 stored manual BPM/key corrections and applied them to every
    later analysis of that file. Nothing writes them any more, so a row left
    behind by an older install would silently override results forever with
    no way to see it or clear it. Opening the cache has to shed the table.
    """
    path = tmp_path / "analysis.sqlite3"

    with sqlite3.connect(path) as connection:
        connection.executescript(
            """
            CREATE TABLE corrections (
                source_hash TEXT PRIMARY KEY,
                bpm REAL,
                key TEXT,
                meter_numerator INTEGER,
                meter_denominator INTEGER,
                bar_one REAL,
                updated_at REAL NOT NULL
            );
            CREATE TABLE cache_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            """
        )
        connection.execute(
            "INSERT INTO corrections VALUES ('abc123', 128.0, 'G minor', 3, 4, 0.25, 0.0)"
        )
        connection.execute("INSERT INTO cache_metadata VALUES ('schema', '1')")

    cache = AnalysisCache(path)

    with sqlite3.connect(path) as connection:
        tables = {
            row[0]
            for row in connection.execute("SELECT name FROM sqlite_master WHERE type = 'table'")
        }
        schema = connection.execute(
            "SELECT value FROM cache_metadata WHERE key = 'schema'"
        ).fetchone()[0]

    assert "corrections" not in tables
    assert schema == "2"

    # And the cache is still usable afterwards.
    cache.put_result("beats", "abc123", "v1", {"mode": "fast"}, {"bpm": 120})
    assert cache.get_result("beats", "abc123", "v1", {"mode": "fast"}) == {"bpm": 120}



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
