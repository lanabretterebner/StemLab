"""Contracts for the cache-aware fast paths: lazy imports, lazy decode,
per-signal key-evidence reuse, and the combined correction+analysis CLI."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

import stemlab.source_analysis as source_analysis_module
from stemlab.analysis_cache import AnalysisCache
from stemlab.beat_tracking import BeatAnalysis

_SRC_DIR = Path(__file__).resolve().parents[1] / "src"


@pytest.fixture(autouse=True)
def _managed_analysis_home(tmp_path, monkeypatch):
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(tmp_path / "managed"))


def _fake_beat_analysis(*_args, **_kwargs) -> BeatAnalysis:
    return BeatAnalysis(
        bpm=120.0,
        detected_bpm=120.0,
        half_time_bpm=60.0,
        double_time_bpm=240.0,
        beats=tuple(round(index * 0.5, 6) for index in range(16)),
        downbeats=(0.0, 2.0, 4.0, 6.0),
        meter_numerator=4,
        meter_denominator=4,
        bar_one=0.0,
        confidence=0.95,
        model="small0",
        model_version="test-fixture",
        device="cpu",
    )


def _write_tone(path: Path, frequencies: tuple[float, ...], seconds: float = 4.0) -> None:
    sample_rate = 22_050
    time = np.arange(int(sample_rate * seconds), dtype=np.float32) / sample_rate
    audio = sum(
        0.2 * np.sin(2.0 * np.pi * frequency * time) for frequency in frequencies
    ).astype(np.float32)
    sf.write(path, audio, sample_rate)


def test_analysis_and_midi_modules_import_without_torch_or_librosa():
    # The sqlite-only CLI paths and cache-hit runs must not need the model
    # stack, so importing the modules cannot pull torch or librosa.
    script = (
        "import sys; "
        "import stemlab.source_analysis, stemlab.midi; "
        "missing = [m for m in ('torch', 'librosa') if m in sys.modules]; "
        "sys.exit(f'eagerly imported: {missing}' if missing else 0)"
    )
    environment = {
        **os.environ,
        "PYTHONPATH": os.pathsep.join([str(_SRC_DIR), os.environ.get("PYTHONPATH", "")]),
    }
    completed = subprocess.run(
        [sys.executable, "-c", script], env=environment, capture_output=True, text=True
    )
    assert completed.returncode == 0, completed.stderr or completed.stdout


def test_fully_cached_analysis_never_decodes_audio(tmp_path, monkeypatch):
    source = tmp_path / "mix.wav"
    _write_tone(source, (220.0, 329.63))
    cache = AnalysisCache(tmp_path / "cache.sqlite3")
    monkeypatch.setattr(source_analysis_module, "analyse_beats", _fake_beat_analysis)

    first = source_analysis_module.analyse_source(source, cache=cache)

    def refuse_decode(_path):
        raise AssertionError("cached analysis decoded audio")

    monkeypatch.setattr(source_analysis_module, "_load_signal", refuse_decode)
    second = source_analysis_module.analyse_source(source, cache=cache)
    assert second == first


def test_mix_key_evidence_survives_stem_hash_change(tmp_path, monkeypatch):
    source = tmp_path / "mix.wav"
    harmony = tmp_path / "harmony.wav"
    _write_tone(source, (220.0, 261.63, 329.63))
    _write_tone(harmony, (261.63,))
    monkeypatch.setattr(source_analysis_module, "analyse_beats", _fake_beat_analysis)

    cache = AnalysisCache(tmp_path / "cache.sqlite3")
    source_analysis_module.analyse_source(source, cache=cache)

    computed_roles: list[bool] = []
    real_evidence = source_analysis_module._tonal_evidence

    def counting_evidence(audio, sample_rate, **kwargs):
        computed_roles.append(bool(kwargs.get("bass_role")))
        return real_evidence(audio, sample_rate, **kwargs)

    monkeypatch.setattr(source_analysis_module, "_tonal_evidence", counting_evidence)

    # Adding a stem invalidates the final "key" entry, but the full-mix
    # evidence must come from the per-signal cache: only the stem is analysed.
    with_stem = source_analysis_module.analyse_source(
        source, harmony_paths=(harmony,), cache=cache
    )
    assert computed_roles == [False]

    cold = source_analysis_module.analyse_source(
        source, harmony_paths=(harmony,), cache=AnalysisCache(tmp_path / "cold.sqlite3")
    )
    assert with_stem == cold


def test_combined_set_correction_and_analysis_single_invocation(tmp_path, monkeypatch):
    source = tmp_path / "mix.wav"
    output = tmp_path / "analysis.json"
    cache_path = tmp_path / "cache.sqlite3"
    _write_tone(source, (220.0, 329.63))
    monkeypatch.setattr(source_analysis_module, "analyse_beats", _fake_beat_analysis)
    source_analysis_module.analyse_source(source, cache=AnalysisCache(cache_path))

    environment = {
        **os.environ,
        "PYTHONPATH": os.pathsep.join([str(_SRC_DIR), os.environ.get("PYTHONPATH", "")]),
    }
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "stemlab.source_analysis",
            "--input",
            str(source),
            "--cache-path",
            str(cache_path),
            "--set-correction",
            "--correct-bpm",
            "124.0",
            "--correct-key",
            "C major",
            "--output",
            str(output),
        ],
        env=environment,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr or completed.stdout
    assert "Analysis correction saved locally" in completed.stdout
    assert "Source analysis:" in completed.stdout

    written = json.loads(output.read_text(encoding="utf-8"))
    assert written["bpm"] == 124.0
    assert written["key"] == "C major"
    assert written["corrected"] is True
    assert written["detected_bpm"] == 120.0

    # The historical exit-early contract without --output must be unchanged.
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "stemlab.source_analysis",
            "--input",
            str(source),
            "--cache-path",
            str(cache_path),
            "--forget-correction",
        ],
        env=environment,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr or completed.stdout
    assert "Correction forgotten" in completed.stdout
    assert "Source analysis:" not in completed.stdout
