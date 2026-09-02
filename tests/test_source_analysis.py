from __future__ import annotations

import librosa
import numpy as np
import soundfile as sf

from stemlab.analysis_cache import AnalysisCache
from stemlab.beat_tracking import BeatAnalysis, TempoSegment
from stemlab.source_analysis import analyse_key, analyse_source


def test_source_analysis_detects_tempo_and_key(tmp_path, monkeypatch):
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(tmp_path / "analysis"))
    sample_rate = 22_050
    duration = 8.0
    time = np.arange(int(sample_rate * duration), dtype=np.float32) / sample_rate

    # A-minor harmony with a 120 BPM click pulse.
    audio = (
        0.20 * np.sin(2.0 * np.pi * 220.00 * time)
        + 0.11 * np.sin(2.0 * np.pi * 261.63 * time)
        + 0.14 * np.sin(2.0 * np.pi * 329.63 * time)
    )
    click = np.exp(-np.arange(int(0.035 * sample_rate)) / (0.007 * sample_rate))
    for start in np.arange(0.0, duration, 0.5):
        index = int(start * sample_rate)
        end = min(audio.size, index + click.size)
        audio[index:end] += 0.65 * click[: end - index]

    source = tmp_path / "source.wav"
    sf.write(source, audio.astype(np.float32), sample_rate)

    # Keep the unit suite independent of the ~80 MB Beat This! checkpoint.
    # The release smoke test exercises the real packaged model after staging.
    beats = tuple(round(index * 0.5, 6) for index in range(16))
    downbeats = tuple(round(index * 2.0, 6) for index in range(4))

    # The segment is part of the fixture rather than an extra: every real
    # analysis of a track with sixteen beats reports one, and a fake without
    # it never exercises the flattening in to_dict or the rebuild on the way
    # back - which is how a cache-hit crash on segment.start reached a
    # release with the suite green.
    segments = (TempoSegment(start=0.0, end=7.5, bpm=120.0, beats=len(beats)),)

    def fake_beat_analysis(*_args, **_kwargs):
        return BeatAnalysis(
            bpm=120.0,
            detected_bpm=120.0,
            half_time_bpm=60.0,
            double_time_bpm=240.0,
            beats=beats,
            downbeats=downbeats,
            meter_numerator=4,
            meter_denominator=4,
            bar_one=0.0,
            confidence=0.95,
            model="final0",
            model_version="test-fixture",
            device="cpu",
            tempo_segments=segments,
        )

    monkeypatch.setattr("stemlab.source_analysis.analyse_beats", fake_beat_analysis)
    cache = AnalysisCache(tmp_path / "analysis.sqlite3")
    result = analyse_source(source, cache=cache)
    assert result.bpm is not None
    assert 115.0 <= result.bpm <= 125.0
    assert result.key is not None
    assert result.key.endswith(("major", "minor"))
    assert len(result.beats) >= 8
    assert result.half_time_bpm < result.detected_bpm < result.double_time_bpm
    assert result.tempo_segments == ({"start": 0.0, "end": 7.5, "bpm": 120.0},)

    # Analysing the same file twice is the ordinary case, and the second
    # answer comes out of the cache: it has to be the first one, segments
    # and all, rather than a crash on the segments it stored as dicts.
    assert analyse_source(source, cache=cache) == result


def test_tonal_ensemble_prefers_g_minor_over_d_minor():
    sample_rate = 22_050
    seconds = 12.0
    time = np.arange(int(sample_rate * seconds), dtype=np.float32) / sample_rate

    def chord(notes):
        return sum(np.sin(2.0 * np.pi * librosa.note_to_hz(note) * time) for note in notes) / len(
            notes
        )

    # A deterministic tonic/dominant fixture for the previous class of error:
    # G minor is sustained twice as long as a D-minor dominant-related section.
    g_minor = chord(("G3", "A#3", "D4"))
    d_minor = chord(("D3", "F3", "A3"))
    envelope = np.ones_like(time)
    audio = np.where((time >= 4.0) & (time < 8.0), d_minor, g_minor) * envelope * 0.3

    result = analyse_key(audio.astype(np.float32), sample_rate)
    names = [candidate.key for candidate in result.candidates[:3]]
    assert names[0] == "G minor"
    assert "D minor" in [candidate.key for candidate in result.candidates[:8]]


def test_key_analysis_marks_noise_uncertain():
    random = np.random.default_rng(7)
    audio = random.normal(0.0, 0.03, 22_050 * 4).astype(np.float32)
    result = analyse_key(audio, 22_050)
    assert result.uncertain
    assert result.key is None


def test_chord_root_evidence_resists_strong_dominant_tonic_confusion():
    sample_rate = 22_050
    seconds = 16.0
    time = np.arange(int(sample_rate * seconds), dtype=np.float32) / sample_rate
    audio = np.zeros_like(time)

    progression = (
        (("G3", "A#3", "D4"), 0.35),
        (("A#3", "D4", "F4"), 0.35),
        (("D#3", "G3", "A#3"), 0.35),
        (("D3", "F#3", "A3"), 0.55),
    )
    segment_seconds = seconds / len(progression)
    for index, (notes, amplitude) in enumerate(progression):
        mask = (time >= index * segment_seconds) & (time < (index + 1) * segment_seconds)
        local_time = time[mask]
        chord = sum(
            np.sin(2.0 * np.pi * librosa.note_to_hz(note) * local_time) for note in notes
        ) / len(notes)
        audio[mask] = amplitude * chord

    result = analyse_key(audio.astype(np.float32), sample_rate)
    assert result.key == "G minor"
    assert result.candidates[0].key == "G minor"
    assert result.candidates[0].probability > result.candidates[1].probability
