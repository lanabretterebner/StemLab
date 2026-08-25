"""Tests for stem-file resolution and backend input staging."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from stemlab.audio import find_stem_file
from stemlab.pretrained import (
    _canonicalise_output_names,
    _clear_audio_files,
    _normalise_input_for_backend,
)


def touch_wav(folder: Path, name: str) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / name
    sf.write(str(path), np.zeros((64, 2), dtype="float32"), 44100)
    return path


class TestFindStemFile:
    def test_exact_name_wins(self, tmp_path: Path):
        touch_wav(tmp_path, "vocals.wav")
        touch_wav(tmp_path, "backing_vocals.wav")

        assert find_stem_file(tmp_path, "vocals").name == "vocals.wav"

    def test_track_named_after_a_stem_does_not_steal_other_stems(self, tmp_path: Path):
        # The separator embeds the track name, so every output contains
        # "guitar". The shortest of them is the bass stem - which the old
        # shortest-substring rule handed back as the guitar.
        for stem in ("vocals", "drums", "bass", "guitar", "piano", "other"):
            touch_wav(tmp_path, f"guitar_take_{stem}.wav")

        assert find_stem_file(tmp_path, "guitar").name == "guitar_take_guitar.wav"
        assert find_stem_file(tmp_path, "bass").name == "guitar_take_bass.wav"
        assert find_stem_file(tmp_path, "other").name == "guitar_take_other.wav"

    def test_suffix_match_beats_bare_substring(self, tmp_path: Path):
        touch_wav(tmp_path, "song_bass.wav")
        touch_wav(tmp_path, "bassoon_layer.wav")

        assert find_stem_file(tmp_path, "bass").name == "song_bass.wav"

    def test_substring_still_matches_unusual_naming(self, tmp_path: Path):
        touch_wav(tmp_path, "01 - PianoStem.wav")

        assert find_stem_file(tmp_path, "piano").name == "01 - PianoStem.wav"

    def test_missing_stem_returns_none(self, tmp_path: Path):
        touch_wav(tmp_path, "vocals.wav")

        assert find_stem_file(tmp_path, "trombone") is None


class TestCanonicaliseOutputNames:
    def test_track_prefix_is_stripped(self, tmp_path: Path):
        for stem in ("vocals", "drums", "instrumental"):
            touch_wav(tmp_path, f"guitar_take_{stem}.wav")

        _canonicalise_output_names(tmp_path, "guitar_take", lambda _message: None)

        names = sorted(p.name for p in tmp_path.glob("*.wav"))
        assert names == ["drums.wav", "instrumental.wav", "vocals.wav"]
        assert find_stem_file(tmp_path, "vocals").name == "vocals.wav"

    def test_unrelated_files_are_left_alone(self, tmp_path: Path):
        touch_wav(tmp_path, "other_song_vocals.wav")

        _canonicalise_output_names(tmp_path, "guitar_take", lambda _message: None)

        assert (tmp_path / "other_song_vocals.wav").exists()


class TestClearAudioFiles:
    def test_previous_run_audio_is_removed(self, tmp_path: Path):
        touch_wav(tmp_path, "old_song_vocals.wav")
        keep = tmp_path / "notes.txt"
        keep.write_text("keep me", encoding="utf-8")

        _clear_audio_files(tmp_path)

        assert not list(tmp_path.glob("*.wav"))
        assert keep.exists()

    def test_missing_folder_is_not_an_error(self, tmp_path: Path):
        _clear_audio_files(tmp_path / "nope")


class TestInputStaging:
    def test_flac_is_converted_for_a_wav_only_backend(self, tmp_path: Path):
        source = tmp_path / "song.flac"
        sf.write(str(source), np.zeros((256, 2), dtype="float32"), 44100)
        staging = tmp_path / "staging"
        staging.mkdir()

        staged = _normalise_input_for_backend(
            input_path=source,
            staging_dir=staging,
            log=lambda _message: None,
            passthrough_extensions={".wav"},
        )

        assert staged.suffix == ".wav"
        assert staged.exists()
        assert sorted(p.suffix for p in staging.iterdir()) == [".wav"]

    def test_flac_passes_through_for_a_backend_that_reads_it(self, tmp_path: Path):
        source = tmp_path / "song.flac"
        sf.write(str(source), np.zeros((256, 2), dtype="float32"), 44100)
        staging = tmp_path / "staging"
        staging.mkdir()

        staged = _normalise_input_for_backend(
            input_path=source,
            staging_dir=staging,
            log=lambda _message: None,
        )

        assert staged.suffix == ".flac"

    def test_uppercase_wav_is_staged_lowercase(self, tmp_path: Path):
        source = tmp_path / "SONG.WAV"
        sf.write(str(source), np.zeros((256, 2), dtype="float32"), 44100)
        staging = tmp_path / "staging"
        staging.mkdir()

        staged = _normalise_input_for_backend(
            input_path=source,
            staging_dir=staging,
            log=lambda _message: None,
            passthrough_extensions={".wav"},
        )

        # The RoFormer CLI globs "*.wav" case-sensitively.
        assert staged.name == "SONG.wav"
        assert list(staging.glob("*.wav")) == [staged]
