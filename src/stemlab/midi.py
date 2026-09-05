"""Stem-aware audio-to-MIDI transcription and MIDI file export."""

from __future__ import annotations

import argparse
import json
import os
import re
from dataclasses import asdict, dataclass, replace
from pathlib import Path

import mido
import numpy as np

from .analysis_cache import cleanup_stale_midi_drag_files, source_identity
from .paths import analysis_dir
from .runtime import configure_utf8_stdio

# librosa is imported inside the transcription functions so that cache-hit
# MIDI exports (re-serializing an existing transcription) never pay for it.

_SAMPLE_RATE = 22_050
_MONO_HOP = 512
_POLY_HOP = 512
MIDI_ALGORITHM_VERSION = "stemlab-midi-3"


@dataclass(frozen=True)
class PitchBendPoint:
    """One expressive pitch offset relative to its parent note."""

    time: float
    semitones: float


@dataclass(frozen=True)
class NoteEvent:
    """One unquantized MIDI note event measured from the source-file start."""

    start: float
    end: float
    pitch: int
    velocity: int
    confidence: float = 1.0
    pitch_bends: tuple[PitchBendPoint, ...] = ()


@dataclass(frozen=True)
class MidiTranscription:
    """Cached internal representation retained before any export or handoff."""

    schema: int
    algorithm_version: str
    source_hash: str
    source_stem: str
    source_tempo: float | None
    grid_mode: str
    bar_one: float
    drums: bool
    notes: tuple[NoteEvent, ...]
    midi_file: str = ""
    drag_file: str = ""


def _velocity(level: float, reference: float) -> int:
    ratio = float(np.clip(level / max(reference, 1.0e-9), 0.0, 1.0))
    return int(np.clip(round(30.0 + 97.0 * np.sqrt(ratio)), 1, 127))


def _merge_notes(notes: list[NoteEvent], max_gap: float = 0.055) -> list[NoteEvent]:
    """Merge adjacent same-pitch fragments created by pitch/onset jitter."""
    merged: list[NoteEvent] = []
    # Where each pitch's merged entries sit in ``merged``, oldest first. Only
    # a same-pitch entry can ever match, so searching every note merged so
    # far is quadratic for nothing.
    positions: dict[int, list[int]] = {}
    for note in sorted(notes, key=lambda item: (item.start, item.pitch)):
        candidates = positions.setdefault(note.pitch, [])
        match = None
        # Entries still sounding past this note's start: no match now, but a
        # later note can still close on one of them.
        reachable: list[int] = []
        # Newest first, the order the scan this replaces read them in - the
        # newest entry for a pitch is not always the one that matches.
        for cursor in range(len(candidates) - 1, -1, -1):
            index = candidates[cursor]
            gap = note.start - merged[index].end
            if 0.0 <= gap <= max_gap:
                match = index
                break
            if gap < 0.0:
                reachable.append(index)
        if match is None:
            # Nothing matched, so every entry was looked at. The ones not
            # collected have fallen more than max_gap behind a start that
            # only moves forward, and nothing still to come can reach them.
            reachable.reverse()
            candidates[:] = reachable
            candidates.append(len(merged))
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


def _smooth_pitch_frames(pitches: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Median-smooth each valid frame over the valid entries of its 5-frame window."""
    smoothed = pitches.copy()
    if pitches.size == 0 or not np.any(valid):
        return smoothed
    # NaN-masking invalid frames and NaN-padding the edges reproduces the
    # boundary clipping and valid-only selection of the per-frame loop exactly.
    padded = np.pad(np.asarray(pitches, dtype=np.float64), 2, constant_values=np.nan)
    padded[2:-2][~valid] = np.nan
    windows = np.lib.stride_tricks.sliding_window_view(padded, 5)
    counts = np.count_nonzero(np.isfinite(windows), axis=1)
    rows = np.flatnonzero(valid & (counts >= 2))
    if rows.size:
        smoothed[rows] = np.nanmedian(windows[rows], axis=1)
    return smoothed


def _transcribe_monophonic(
    audio: np.ndarray,
    sample_rate: int,
    *,
    bass: bool,
) -> list[NoteEvent]:
    import librosa

    fmin = librosa.note_to_hz("E1" if bass else "C2")
    fmax = librosa.note_to_hz("C5" if bass else "C7")
    f0, voiced, probability = librosa.pyin(
        audio,
        fmin=fmin,
        fmax=fmax,
        sr=sample_rate,
        frame_length=2048,
        hop_length=_MONO_HOP,
    )
    if f0 is None or probability is None:
        return []

    rms = librosa.feature.rms(y=audio, frame_length=2048, hop_length=_MONO_HOP)[0]
    frame_count = min(f0.size, probability.size, rms.size)
    f0 = f0[:frame_count]
    probability = probability[:frame_count]
    voiced = np.asarray(voiced[:frame_count], dtype=bool)
    rms = rms[:frame_count]

    midi_pitch = librosa.hz_to_midi(f0)
    valid = voiced & np.isfinite(midi_pitch) & (probability >= (0.62 if bass else 0.55))
    midi_pitch = _smooth_pitch_frames(midi_pitch, valid)
    rounded = np.where(valid, np.rint(midi_pitch), -1).astype(np.int16)

    # Bridge only tiny unvoiced gaps when both sides agree. This retains rests
    # while avoiding note splits caused by a single uncertain analysis frame.
    # A filled frame can never enable an adjacent fill (the condition needs
    # both neighbours already voiced), so this equals the sequential scan.
    gap = (rounded[1:-1] < 0) & (rounded[:-2] >= 0) & (rounded[:-2] == rounded[2:])
    fill = np.flatnonzero(gap) + 1
    if fill.size:
        rounded[fill] = rounded[fill - 1]
        rms[fill] = np.minimum(rms[fill - 1], rms[fill + 1])

    min_duration = 0.10 if bass else 0.075
    reference = float(np.percentile(rms[rms > 0.0], 95)) if np.any(rms > 0.0) else 1.0
    notes: list[NoteEvent] = []
    start = 0
    while start < frame_count:
        pitch = int(rounded[start])
        if pitch < 0:
            start += 1
            continue
        end = start + 1
        while end < frame_count and int(rounded[end]) == pitch:
            end += 1
        start_seconds = librosa.frames_to_time(start, sr=sample_rate, hop_length=_MONO_HOP)
        end_seconds = librosa.frames_to_time(end, sr=sample_rate, hop_length=_MONO_HOP)
        if end_seconds - start_seconds >= min_duration:
            bends = tuple(
                PitchBendPoint(
                    float(librosa.frames_to_time(frame, sr=sample_rate, hop_length=_MONO_HOP)),
                    round(float(midi_pitch[frame] - pitch), 4),
                )
                for frame in range(start, end, 4)
                if np.isfinite(midi_pitch[frame]) and abs(float(midi_pitch[frame] - pitch)) >= 0.03
            )
            notes.append(
                NoteEvent(
                    float(start_seconds),
                    float(end_seconds),
                    pitch,
                    _velocity(float(np.mean(rms[start:end])), reference),
                    round(float(np.mean(probability[start:end])), 5),
                    bends,
                )
            )
        start = end
    return _merge_notes(notes)


def _fixed_drum_pitch(stem_type: str) -> int | None:
    normalized = stem_type.lower().replace("-", "_").replace(" ", "_")
    if "kick" in normalized:
        return 36
    if "snare" in normalized:
        return 38
    if "open" in normalized and ("hat" in normalized or "hihat" in normalized):
        return 46
    if "hat" in normalized or "hihat" in normalized:
        return 42
    if "tom" in normalized:
        return 45
    if "ride" in normalized:
        return 51
    if "crash" in normalized or "cymbal" in normalized:
        return 49
    return None


def _classify_drum(audio: np.ndarray, sample_rate: int, sample: int) -> tuple[int, float]:
    end = min(audio.size, sample + int(0.24 * sample_rate))
    region = audio[sample:end]
    if region.size < 64:
        return 38, 0.08

    spectrum = np.square(np.abs(np.fft.rfft(region * np.hanning(region.size))))
    frequencies = np.fft.rfftfreq(region.size, 1.0 / sample_rate)
    total = float(np.sum(spectrum)) + 1.0e-12
    low = float(np.sum(spectrum[frequencies < 180.0])) / total
    high = float(np.sum(spectrum[frequencies > 4500.0])) / total

    if low > 0.42:
        return 36, 0.10
    if high > 0.38:
        split = min(region.size, int(0.12 * sample_rate))
        tail_ratio = float(np.mean(np.abs(region[split:]))) / (
            float(np.mean(np.abs(region[: max(1, split)]))) + 1.0e-9
        )
        return (46, 0.24) if tail_ratio > 0.28 else (42, 0.055)
    return 38, 0.10


def _transcribe_drums(
    audio: np.ndarray,
    sample_rate: int,
    stem_type: str,
) -> list[NoteEvent]:
    import librosa

    onset_envelope = librosa.onset.onset_strength(
        y=audio,
        sr=sample_rate,
        hop_length=_POLY_HOP,
    )
    frames = librosa.onset.onset_detect(
        onset_envelope=onset_envelope,
        sr=sample_rate,
        hop_length=_POLY_HOP,
        backtrack=False,
        units="frames",
    )
    if len(frames) == 0:
        return []

    fixed_pitch = _fixed_drum_pitch(stem_type)
    reference = float(np.percentile(onset_envelope[frames], 95)) + 1.0e-9
    notes: list[NoteEvent] = []
    for frame in frames:
        start = float(librosa.frames_to_time(frame, sr=sample_rate, hop_length=_POLY_HOP))
        sample = int(librosa.frames_to_samples(frame, hop_length=_POLY_HOP))
        if fixed_pitch is None:
            pitch, duration = _classify_drum(audio, sample_rate, sample)
        else:
            pitch = fixed_pitch
            duration = 0.24 if pitch == 46 else (0.055 if pitch == 42 else 0.10)
        strength = float(onset_envelope[min(int(frame), onset_envelope.size - 1)])
        notes.append(
            NoteEvent(
                start,
                start + duration,
                pitch,
                _velocity(strength, reference),
                round(float(np.clip(strength / reference, 0.0, 1.0)), 5),
            )
        )
    return notes


def _transcribe_polyphonic(
    audio: np.ndarray,
    sample_rate: int,
    stem_type: str,
) -> list[NoteEvent]:
    import librosa
    from scipy.signal import find_peaks

    min_midi = 40 if "guitar" in stem_type.lower() else 21
    max_midi = 100 if "piano" in stem_type.lower() else 96
    magnitude = np.abs(
        librosa.cqt(
            audio,
            sr=sample_rate,
            hop_length=_POLY_HOP,
            fmin=librosa.midi_to_hz(min_midi),
            n_bins=max_midi - min_midi + 1,
            bins_per_octave=12,
        )
    )
    if magnitude.size == 0 or float(np.max(magnitude)) <= 1.0e-8:
        return []

    onset_frames = librosa.onset.onset_detect(
        y=audio,
        sr=sample_rate,
        hop_length=_POLY_HOP,
        backtrack=True,
        units="frames",
    )
    boundaries = np.unique(np.concatenate(([0], onset_frames, [magnitude.shape[1]]))).astype(int)
    global_reference = float(np.percentile(magnitude, 99)) + 1.0e-12
    notes: list[NoteEvent] = []

    for start_frame, end_frame in zip(boundaries[:-1], boundaries[1:], strict=True):
        if end_frame - start_frame < 2:
            continue
        levels = np.percentile(magnitude[:, start_frame:end_frame], 75, axis=1)
        peak = float(np.max(levels))
        if peak < global_reference * 0.025:
            continue

        levels_db = librosa.amplitude_to_db(levels, ref=max(peak, 1.0e-12))
        candidates, _ = find_peaks(levels_db, prominence=2.5)
        candidates = [index for index in candidates if levels_db[index] >= -22.0]
        candidates.sort(key=lambda index: float(levels[index]), reverse=True)

        accepted: list[int] = []
        for candidate in candidates:
            # Weak upper harmonics are not emitted as extra notes. Strong real
            # octaves/chord tones survive this deliberately narrow filter.
            is_weak_harmonic = any(
                candidate - lower in (12, 19, 24, 28) and levels[candidate] < levels[lower] * 0.42
                for lower in accepted
                if lower < candidate
            )
            if not is_weak_harmonic:
                accepted.append(int(candidate))
            if len(accepted) >= 8:
                break

        start = float(librosa.frames_to_time(start_frame, sr=sample_rate, hop_length=_POLY_HOP))
        end = float(librosa.frames_to_time(end_frame, sr=sample_rate, hop_length=_POLY_HOP))
        if end - start < 0.07:
            continue
        for candidate in accepted:
            notes.append(
                NoteEvent(
                    start,
                    end,
                    min_midi + candidate,
                    _velocity(float(levels[candidate]), global_reference),
                    round(float(np.clip(levels[candidate] / global_reference, 0.0, 1.0)), 5),
                )
            )
    return _merge_notes(notes, max_gap=0.07)


def transcribe_stem(
    input_path: str | Path,
    stem_type: str,
) -> tuple[list[NoteEvent], bool]:
    """Transcribe one isolated stem and return notes plus its drum-channel flag."""
    import librosa

    input_path = Path(input_path).expanduser().resolve()
    if not input_path.is_file():
        raise FileNotFoundError(f"Stem audio does not exist: {input_path}")

    audio, sample_rate = librosa.load(str(input_path), sr=_SAMPLE_RATE, mono=True)
    audio = np.asarray(audio, dtype=np.float32)
    if audio.size == 0 or float(np.max(np.abs(audio))) < 1.0e-5:
        raise RuntimeError("The stem is silent")

    normalized = stem_type.lower()
    is_drum = normalized == "drums" or normalized.startswith("drum.")
    if is_drum:
        notes = _transcribe_drums(audio, sample_rate, normalized)
    elif normalized == "bass" or normalized.startswith("instrument.bass"):
        notes = _transcribe_monophonic(audio, sample_rate, bass=True)
    elif normalized == "vocals" or normalized.startswith("vocal."):
        notes = _transcribe_monophonic(audio, sample_rate, bass=False)
    else:
        notes = _transcribe_polyphonic(audio, sample_rate, normalized)

    if not notes:
        raise RuntimeError("No sufficiently confident notes were detected in this stem")
    return notes, is_drum


def _file_hash(path: Path) -> str:
    """The digest a stem is identified by, wherever it is identified.

    Deliberately not a second SHA-256 loop of its own. This has to agree with
    the analysis cache's idea of the same file - the sidecar written here is
    matched against a hash the analysis produced - so there is one streaming
    implementation and this names it for the callers below.
    """
    return source_identity(path)


def metadata_path_for(midi_path: str | Path) -> Path:
    path = Path(midi_path)
    return path.with_suffix(".stemlab-midi.json")


def _transcription_from_dict(payload: dict) -> MidiTranscription:
    notes = tuple(
        NoteEvent(
            start=float(item["start"]),
            end=float(item["end"]),
            pitch=int(item["pitch"]),
            velocity=int(item["velocity"]),
            confidence=float(item.get("confidence", 1.0)),
            pitch_bends=tuple(PitchBendPoint(**point) for point in item.get("pitch_bends", ())),
        )
        for item in payload.get("notes", ())
    )
    return MidiTranscription(
        schema=int(payload.get("schema", 1)),
        algorithm_version=str(payload.get("algorithm_version", "")),
        source_hash=str(payload.get("source_hash", "")),
        source_stem=str(payload.get("source_stem", "")),
        source_tempo=payload.get("source_tempo"),
        grid_mode=str(payload.get("grid_mode", "source")),
        bar_one=float(payload.get("bar_one", 0.0)),
        drums=bool(payload.get("drums", False)),
        notes=notes,
        midi_file=str(payload.get("midi_file", "")),
        drag_file=str(payload.get("drag_file", "")),
    )


def read_transcription(path: str | Path) -> MidiTranscription:
    """Read a cached internal transcription from JSON."""
    return _transcription_from_dict(json.loads(Path(path).read_text(encoding="utf-8")))


def write_transcription(path: str | Path, transcription: MidiTranscription) -> Path:
    """Atomically persist StemLab's internal MIDI object."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(asdict(transcription), indent=2), encoding="utf-8")
    os.replace(temporary, path)
    return path.resolve()


def _managed_drag_path(source: Path, stem_type: str, source_hash: str) -> Path:
    cleanup_stale_midi_drag_files()
    safe_stem = re.sub(r"[^A-Za-z0-9._-]+", "_", stem_type).strip("_") or "stem"
    safe_source = re.sub(r"[^A-Za-z0-9._-]+", "_", source.stem).strip("_") or "source"
    directory = analysis_dir() / "MidiDrag"
    directory.mkdir(parents=True, exist_ok=True)
    return directory / f"{safe_source}-{safe_stem}-{source_hash[:10]}.mid"


def create_transcription(
    input_path: str | Path,
    stem_type: str,
    *,
    bpm: float | None,
    grid_mode: str = "source",
    bar_one: float = 0.0,
    source_hash: str | None = None,
) -> MidiTranscription:
    """Create or reuse an internal stem transcription independent of export target.

    ``source_hash`` is the digest of ``input_path`` a caller already holds
    from its own cache lookup. Without it the whole stem is streamed a second
    time to arrive at the identical digest.
    """
    source = Path(input_path).expanduser().resolve()
    if source_hash is None:
        source_hash = _file_hash(source)
    notes, drums = transcribe_stem(source, stem_type)
    return MidiTranscription(
        schema=2,
        algorithm_version=MIDI_ALGORITHM_VERSION,
        source_hash=source_hash,
        source_stem=stem_type,
        source_tempo=bpm,
        grid_mode=grid_mode,
        bar_one=bar_one,
        drums=drums,
        notes=tuple(notes),
    )


def write_midi(
    output_path: str | Path,
    notes: list[NoteEvent],
    *,
    bpm: float | None,
    stem_type: str,
    drums: bool,
) -> Path:
    """Write unquantized note times with source BPM stored as tempo metadata."""
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    tempo_bpm = float(bpm) if bpm is not None and 20.0 <= float(bpm) <= 400.0 else 120.0
    tempo = mido.bpm2tempo(tempo_bpm)
    midi_file = mido.MidiFile(type=1, ticks_per_beat=480)
    track = mido.MidiTrack()
    midi_file.tracks.append(track)
    track.append(mido.MetaMessage("track_name", name=f"StemLab {stem_type}", time=0))
    track.append(mido.MetaMessage("set_tempo", tempo=tempo, time=0))

    if not drums:
        programs = {"bass": 33, "guitar": 25, "piano": 0, "vocals": 53}
        family = next((name for name in programs if name in stem_type.lower()), "piano")
        track.append(mido.Message("program_change", program=programs[family], channel=0, time=0))

    events: list[tuple[int, int, mido.Message]] = []
    channel = 9 if drums else 0
    for note in notes:
        start_tick = max(0, round(mido.second2tick(note.start, midi_file.ticks_per_beat, tempo)))
        end_tick = max(
            start_tick + 1,
            round(mido.second2tick(note.end, midi_file.ticks_per_beat, tempo)),
        )
        events.append(
            (
                start_tick,
                2,
                mido.Message("note_on", note=note.pitch, velocity=note.velocity, channel=channel),
            )
        )
        events.append(
            (end_tick, 0, mido.Message("note_off", note=note.pitch, velocity=0, channel=channel))
        )
        for bend in note.pitch_bends:
            bend_tick = max(
                start_tick,
                min(end_tick, round(mido.second2tick(bend.time, midi_file.ticks_per_beat, tempo))),
            )
            wheel = int(np.clip(round(bend.semitones / 2.0 * 8192), -8192, 8191))
            events.append((bend_tick, 1, mido.Message("pitchwheel", pitch=wheel, channel=channel)))
        if note.pitch_bends:
            events.append((end_tick, 1, mido.Message("pitchwheel", pitch=0, channel=channel)))

    previous_tick = 0
    for tick, _priority, message in sorted(events, key=lambda item: (item[0], item[1])):
        message.time = tick - previous_tick
        track.append(message)
        previous_tick = tick
    track.append(mido.MetaMessage("end_of_track", time=0))
    midi_file.save(output_path)
    return output_path.resolve()


def convert_stem_to_midi(
    input_path: str | Path,
    output_path: str | Path,
    stem_type: str,
    bpm: float | None = None,
    *,
    grid_mode: str = "source",
    bar_one: float = 0.0,
) -> Path:
    """Create/reuse the internal object, then serialize export and drag files."""
    source = Path(input_path).expanduser().resolve()
    output = Path(output_path).expanduser().resolve()
    metadata_path = metadata_path_for(output)
    source_hash = _file_hash(source)
    transcription = None
    if metadata_path.is_file():
        cached = read_transcription(metadata_path)
        if (
            cached.algorithm_version == MIDI_ALGORITHM_VERSION
            and cached.source_hash == source_hash
            and cached.source_stem == stem_type
        ):
            transcription = cached

    if transcription is None:
        transcription = create_transcription(
            source,
            stem_type,
            bpm=bpm,
            grid_mode=grid_mode,
            bar_one=bar_one,
            source_hash=source_hash,
        )
    else:
        transcription = replace(
            transcription,
            source_tempo=bpm,
            grid_mode=grid_mode,
            bar_one=bar_one,
        )

    write_midi(
        output,
        list(transcription.notes),
        bpm=transcription.source_tempo,
        stem_type=transcription.source_stem,
        drums=transcription.drums,
    )
    drag_path = _managed_drag_path(source, stem_type, source_hash)
    write_midi(
        drag_path,
        list(transcription.notes),
        bpm=transcription.source_tempo,
        stem_type=transcription.source_stem,
        drums=transcription.drums,
    )
    transcription = replace(
        transcription,
        midi_file=str(output),
        drag_file=str(drag_path.resolve()),
    )
    write_transcription(metadata_path, transcription)
    return output


def main() -> None:
    """CLI entry used by the JUCE MIDI worker.

    The reporting wrapper lives here rather than under ``__main__``, because
    the plugin reaches this through the ``stemlab-midi-job`` console script on
    venv and development installs. That calls this function directly, so a
    wrapper under ``__main__`` never ran and a failed conversion reached the
    user as a bare exit code with no reason attached.
    """
    try:
        _main()
    except Exception as exc:
        print(f"STEMLAB_ERROR MIDI conversion failed: {exc}", flush=True)
        raise


def _main() -> None:
    configure_utf8_stdio()
    parser = argparse.ArgumentParser(description="Convert one separated StemLab stem to MIDI.")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--stem-type", required=True)
    parser.add_argument("--bpm", type=float)
    parser.add_argument("--grid-mode", default="source")
    parser.add_argument("--bar-one", type=float, default=0.0)
    args = parser.parse_args()

    print(f"STEMLAB_PROGRESS 5.0 Analysing {args.stem_type} notes", flush=True)
    output = convert_stem_to_midi(
        args.input,
        args.output,
        args.stem_type,
        args.bpm,
        grid_mode=args.grid_mode,
        bar_one=args.bar_one,
    )
    print("STEMLAB_PROGRESS 100.0 MIDI saved", flush=True)
    print(f"MIDI: {output}", flush=True)


if __name__ == "__main__":
    main()
