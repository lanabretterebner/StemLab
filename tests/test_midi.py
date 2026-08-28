from __future__ import annotations

import dataclasses
import os
import time

import mido
import numpy as np
import pytest
import soundfile as sf

import stemlab.midi
from stemlab.analysis_cache import cleanup_stale_midi_drag_files
from stemlab.midi import (
    MIDI_ALGORITHM_VERSION,
    NoteEvent,
    PitchBendPoint,
    _merge_notes,
    _smooth_pitch_frames,
    build_ableton_midi_payload,
    convert_stem_to_midi,
    create_transcription,
    metadata_path_for,
    read_transcription,
)


@pytest.fixture(autouse=True)
def _managed_analysis_home(tmp_path, monkeypatch):
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(tmp_path / "managed"))


def _note_ons(path):
    midi_file = mido.MidiFile(path)
    elapsed = 0
    notes = []
    for message in mido.merge_tracks(midi_file.tracks):
        elapsed += message.time
        if message.type == "note_on" and message.velocity > 0:
            notes.append((message.note, elapsed, message.velocity, message.channel))
    tempo = next(
        message.tempo
        for track in midi_file.tracks
        for message in track
        if message.type == "set_tempo"
    )
    return midi_file, notes, tempo


def test_monophonic_stem_writes_timed_midi(tmp_path):
    sample_rate = 22_050
    audio = np.zeros(int(sample_rate * 1.4), dtype=np.float32)
    first_start, first_end = int(0.10 * sample_rate), int(0.55 * sample_rate)
    second_start, second_end = int(0.75 * sample_rate), int(1.20 * sample_rate)
    first = np.arange(first_end - first_start, dtype=np.float32) / sample_rate
    second = np.arange(second_end - second_start, dtype=np.float32) / sample_rate
    audio[first_start:first_end] = 0.4 * np.sin(2.0 * np.pi * 220.0 * first)
    audio[second_start:second_end] = 0.35 * np.sin(2.0 * np.pi * 261.63 * second)

    source = tmp_path / "bass.wav"
    output = tmp_path / "bass.mid"
    sf.write(source, audio, sample_rate)
    convert_stem_to_midi(source, output, "bass", bpm=120.0)

    midi_file, notes, tempo = _note_ons(output)
    assert output.exists()
    assert {57, 60}.issubset({note[0] for note in notes})
    assert notes[0][1] > 0
    assert midi_file.ticks_per_beat == 480
    assert round(mido.tempo2bpm(tempo)) == 120


def test_isolated_kick_uses_general_midi_drum_note(tmp_path):
    sample_rate = 22_050
    audio = np.zeros(sample_rate, dtype=np.float32)
    pulse_time = np.arange(int(sample_rate * 0.16), dtype=np.float32) / sample_rate
    pulse = np.exp(-pulse_time * 24.0) * np.sin(2.0 * np.pi * 72.0 * pulse_time)
    for start in (0.15, 0.50, 0.82):
        sample = int(start * sample_rate)
        audio[sample : sample + pulse.size] += pulse

    source = tmp_path / "kick.wav"
    output = tmp_path / "kick.mid"
    sf.write(source, audio, sample_rate)
    convert_stem_to_midi(source, output, "drum.kick", bpm=100.0)

    _midi_file, notes, _tempo = _note_ons(output)
    assert len(notes) >= 2
    assert {note[0] for note in notes} == {36}
    assert {note[3] for note in notes} == {9}


def test_harmonic_stem_preserves_a_polyphonic_chord(tmp_path):
    sample_rate = 22_050
    time = np.arange(sample_rate, dtype=np.float32) / sample_rate
    audio = sum(
        0.22 * np.sin(2.0 * np.pi * frequency * time) for frequency in (261.63, 329.63, 392.00)
    ).astype(np.float32)

    source = tmp_path / "piano.wav"
    output = tmp_path / "piano.mid"
    sf.write(source, audio, sample_rate)
    convert_stem_to_midi(source, output, "piano", bpm=90.0)

    _midi_file, notes, _tempo = _note_ons(output)
    detected = {note[0] for note in notes}
    assert len({60, 64, 67} & detected) >= 2


def test_internal_midi_round_trip_drag_lifecycle_and_ableton_payload(tmp_path):
    sample_rate = 22_050
    time_axis = np.arange(sample_rate, dtype=np.float32) / sample_rate
    audio = (0.4 * np.sin(2.0 * np.pi * 110.0 * time_axis)).astype(np.float32)
    source = tmp_path / "source bass.wav"
    output = tmp_path / "source bass.mid"
    sf.write(source, audio, sample_rate)

    convert_stem_to_midi(source, output, "bass", bpm=120.0, bar_one=0.25)
    transcription = read_transcription(metadata_path_for(output))
    assert transcription.source_stem == "bass"
    assert transcription.notes
    assert transcription.midi_file == str(output.resolve())
    drag_file = os.path.abspath(transcription.drag_file)
    assert os.path.isfile(drag_file)

    payload = build_ableton_midi_payload(transcription, capture_start_ppq=8.0)
    assert payload["protocol"] == "stemlab-ableton-midi"
    assert payload["capture_start_ppq"] == 8.0
    assert payload["notes"][0]["duration"] > 0

    convert_stem_to_midi(source, output, "bass", bpm=90.0, grid_mode="manual", bar_one=0.5)
    updated = read_transcription(metadata_path_for(output))
    assert updated.source_tempo == 90.0
    assert updated.grid_mode == "manual"
    assert updated.bar_one == 0.5

    old = time.time() - 10 * 24 * 60 * 60
    os.utime(drag_file, (old, old))
    assert cleanup_stale_midi_drag_files(max_age_days=7) == 1
    assert not os.path.exists(drag_file)


def test_smooth_pitch_frames_matches_per_frame_reference():
    rng = np.random.default_rng(5)
    for _ in range(50):
        size = int(rng.integers(0, 120))
        pitches = rng.normal(60.0, 10.0, size)
        valid = rng.random(size) < 0.6
        pitches[~valid] = np.nan

        reference = pitches.copy()
        for index in np.flatnonzero(valid):
            start = max(0, index - 2)
            end = min(pitches.size, index + 3)
            local = pitches[start:end][valid[start:end]]
            if local.size >= 2:
                reference[index] = float(np.median(local))

        result = _smooth_pitch_frames(pitches, valid)
        assert np.array_equal(np.isnan(reference), np.isnan(result))
        finite = np.isfinite(reference)
        assert np.array_equal(reference[finite], result[finite])


def test_stale_algorithm_version_triggers_retranscription(tmp_path):
    sample_rate = 22_050
    time_axis = np.arange(sample_rate, dtype=np.float32) / sample_rate
    audio = (0.4 * np.sin(2.0 * np.pi * 110.0 * time_axis)).astype(np.float32)
    source = tmp_path / "bass.wav"
    output = tmp_path / "bass.mid"
    sf.write(source, audio, sample_rate)

    convert_stem_to_midi(source, output, "bass", bpm=120.0)
    metadata_path = metadata_path_for(output)
    stale = read_transcription(metadata_path)
    assert stale.algorithm_version == MIDI_ALGORITHM_VERSION

    # An entry from an older algorithm (e.g. the pre-hop-512 transcriber)
    # must be recomputed, not reused.
    import json

    payload = json.loads(metadata_path.read_text(encoding="utf-8"))
    payload["algorithm_version"] = "stemlab-midi-2"
    payload["notes"] = []
    metadata_path.write_text(json.dumps(payload), encoding="utf-8")

    convert_stem_to_midi(source, output, "bass", bpm=120.0)
    refreshed = read_transcription(metadata_path)
    assert refreshed.algorithm_version == MIDI_ALGORITHM_VERSION
    assert refreshed.notes


def _merge_notes_by_linear_scan(notes, max_gap=0.055):
    """_merge_notes as it read before the per-pitch index, verbatim."""
    merged: list[NoteEvent] = []
    for note in sorted(notes, key=lambda item: (item.start, item.pitch)):
        match = next(
            (
                index
                for index in range(len(merged) - 1, -1, -1)
                if merged[index].pitch == note.pitch
                and 0.0 <= note.start - merged[index].end <= max_gap
            ),
            None,
        )
        if match is None:
            merged.append(note)
        else:
            previous = merged[match]
            merged[match] = dataclasses.replace(
                previous,
                end=max(previous.end, note.end),
                velocity=max(previous.velocity, note.velocity),
                confidence=max(previous.confidence, note.confidence),
                pitch_bends=previous.pitch_bends + note.pitch_bends,
            )
    return sorted(merged, key=lambda item: (item.start, item.pitch))


def test_merge_notes_picks_what_the_linear_scan_picked():
    # The predecessor is not always the newest entry for the pitch: an older
    # one whose end lands inside the gap window is the match even when a
    # newer one is still sounding, which is what the scan found by walking
    # past it. An index keyed on the newest entry alone merges nothing here.
    overlapping = [
        NoteEvent(0.00, 0.10, 60, 64),
        NoteEvent(0.05, 1.00, 60, 70),
        NoteEvent(0.12, 0.20, 60, 80),
    ]
    assert _merge_notes(overlapping) == _merge_notes_by_linear_scan(overlapping)
    assert [(note.start, note.end) for note in _merge_notes(overlapping)] == [
        (0.00, 0.20),
        (0.05, 1.00),
    ]

    rng = np.random.default_rng(20260827)
    # Sparse across the keyboard; dense on a handful of pitches, where gaps
    # land inside max_gap constantly; long overlapping notes on two pitches;
    # and exact duplicates, which merge into themselves.
    shapes = (
        (21, 108, 12.0, 0.4),
        (58, 63, 2.0, 0.06),
        (60, 62, 2.0, 5.0),
        (60, 61, 0.4, 0.05),
    )
    for trial in range(300):
        low, high, span, longest = shapes[trial % len(shapes)]
        count = int(rng.integers(0, 60))
        notes = []
        for _ in range(count):
            start = round(float(rng.uniform(0.0, span)), 4)
            end = start + round(float(rng.uniform(0.0, longest)), 4)
            bends = ()
            if rng.random() < 0.3:
                bends = (PitchBendPoint(start, round(float(rng.uniform(-2, 2)), 3)),)
            notes.append(
                NoteEvent(
                    start,
                    end,
                    int(rng.integers(low, high)),
                    int(rng.integers(1, 128)),
                    round(float(rng.random()), 3),
                    bends,
                )
            )
        if trial % len(shapes) == 3:
            notes = notes + notes

        assert _merge_notes(notes) == _merge_notes_by_linear_scan(notes)


def test_export_hashes_the_stem_once(tmp_path, monkeypatch):
    sample_rate = 22_050
    time_axis = np.arange(sample_rate, dtype=np.float32) / sample_rate
    source = tmp_path / "bass.wav"
    sf.write(source, (0.4 * np.sin(2.0 * np.pi * 110.0 * time_axis)).astype(np.float32),
             sample_rate)

    digests: list[str] = []
    real_hash = stemlab.midi._file_hash
    monkeypatch.setattr(
        "stemlab.midi._file_hash",
        lambda path: (digests.append(str(path)), real_hash(path))[1],
    )

    # The cache lookup already streamed the whole stem; transcribing it must
    # not stream it again to arrive at the same digest.
    convert_stem_to_midi(source, tmp_path / "bass.mid", "bass", bpm=120.0)
    assert len(digests) == 1

    # A direct caller that has no digest of its own still gets one.
    digests.clear()
    transcription = create_transcription(source, "bass", bpm=120.0)
    assert len(digests) == 1
    assert transcription.source_hash == real_hash(source)
