from __future__ import annotations

import os
import time

import mido
import numpy as np
import pytest
import soundfile as sf

from stemlab.analysis_cache import cleanup_stale_midi_drag_files
from stemlab.midi import (
    build_ableton_midi_payload,
    convert_stem_to_midi,
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


def _merge_notes_reference(notes, max_gap=0.055):
    """The straightforward quadratic merge, kept as the definition of correct.

    ``_merge_notes`` indexes by pitch to avoid rescanning every merged note.
    This is the scan it replaced, so any disagreement between the two is a
    bug in the index rather than a judgement call.
    """
    from dataclasses import replace

    merged = []
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
            merged[match] = replace(
                previous,
                end=max(previous.end, note.end),
                velocity=max(previous.velocity, note.velocity),
                confidence=max(previous.confidence, note.confidence),
                pitch_bends=previous.pitch_bends + note.pitch_bends,
            )
    return sorted(merged, key=lambda item: (item.start, item.pitch))


def _note(pitch, start, end, velocity=90):
    from stemlab.midi import NoteEvent

    return NoteEvent(
        pitch=pitch,
        start=start,
        end=end,
        velocity=velocity,
        confidence=1.0,
        pitch_bends=[],
    )


def test_merge_notes_reaches_past_a_still_sounding_note():
    """A held note must not hide an older one the gap window still reaches.

    Indexing only the newest entry per pitch - the obvious way to drop the
    quadratic scan - gets this wrong: it stops at the sustained note, whose
    end is still ahead of the new start, and never sees the short note that
    ended 20 ms ago. The original scan walks past it, so the index has to as
    well.
    """
    from stemlab.midi import _merge_notes

    short = _note(60, 0.00, 0.10)
    sustained = _note(60, 0.05, 0.50)
    trailing = _note(60, 0.12, 0.20)

    merged = _merge_notes([short, sustained, trailing])

    assert merged == _merge_notes_reference([short, sustained, trailing])
    # The short note absorbed the trailing fragment; the sustained one is
    # untouched.
    assert [(n.start, n.end) for n in merged] == [(0.00, 0.20), (0.05, 0.50)]


def test_merge_notes_matches_the_reference_scan_on_random_input():
    from stemlab.midi import _merge_notes

    rng = np.random.default_rng(20260828)
    for _ in range(40):
        notes = []
        for _ in range(rng.integers(5, 60)):
            start = float(rng.uniform(0.0, 4.0))
            notes.append(
                _note(
                    int(rng.integers(48, 55)),
                    start,
                    start + float(rng.uniform(0.01, 0.6)),
                    velocity=int(rng.integers(1, 128)),
                )
            )
        assert _merge_notes(notes) == _merge_notes_reference(notes)


def test_merge_notes_is_not_quadratic():
    """One pitch, thousands of fragments - the shape a long stem produces."""
    from stemlab.midi import _merge_notes

    notes = [_note(60, i * 0.5, i * 0.5 + 0.1) for i in range(8000)]

    started = time.perf_counter()
    merged = _merge_notes(notes)
    elapsed = time.perf_counter() - started

    assert len(merged) == 8000
    # The quadratic scan needs tens of seconds at this size; the index needs
    # milliseconds. A second is far above the latter and far below the former.
    assert elapsed < 1.0, f"_merge_notes took {elapsed:.1f}s for {len(notes)} notes"
