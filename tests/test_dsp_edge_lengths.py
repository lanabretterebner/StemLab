"""Short-signal edge cases that used to raise deep inside scipy.

Each length here is one that previously produced
``ValueError: noverlap must be less than nperseg`` after both model passes
had already completed - the most expensive possible place to crash.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf

from stemlab.audio import STEM_NAMES
from stemlab.adaptive.analysis import analyse_audio
from stemlab.adaptive.foreground import split_foreground
from stemlab.hybrid import fuse_stem_folders, fuse_stem_pair
from stemlab.recursive import _json_safe_float
from stemlab.refinement.events import detect_kick_events

SAMPLE_RATE = 44100


def write_tone(path: Path, frames: int, channels: int = 2) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    t = np.linspace(0.0, frames / SAMPLE_RATE, frames, endpoint=False)
    wave = 0.2 * np.sin(2 * np.pi * 220.0 * t)
    data = np.stack([wave] * channels, axis=1).astype("float32")
    sf.write(str(path), data, SAMPLE_RATE)
    return path


class TestHybridFusion:
    def test_short_tail_chunks_fuse(self, tmp_path: Path):
        # 1000 and 1536 are the sub-frame remainders that used to raise, where
        # nperseg is clamped to the block; 5000 is the first length past a full
        # analysis frame, so it is the one that runs the transform at its
        # unclamped 2048/512 size. A 508150-frame case stood here too, named
        # for a chunk boundary it does not cross - the chunk is 16 s and that
        # is 11.5 - so it spent seconds proving what 5000 frames prove. The
        # boundary itself is covered below, at a chunk length a test can
        # afford.
        for frames in (1000, 1536, 5000):
            roformer = tmp_path / f"roformer_{frames}"
            demucs = tmp_path / f"demucs_{frames}"
            out = tmp_path / f"out_{frames}"

            for stem in STEM_NAMES:
                write_tone(roformer / f"{stem}.wav", frames)
                write_tone(demucs / f"{stem}.wav", frames)

            fuse_stem_folders(
                roformer_dir=roformer,
                demucs_dir=demucs,
                output_dir=out,
            )

            fused, _ = sf.read(str(out / "vocals.wav"), always_2d=True)
            assert fused.shape[0] == frames

    def test_a_short_final_chunk_is_fused_and_stitched_back(self, tmp_path: Path):
        """The tail chunk: shorter than the rest, and crossfaded into place.

        fuse_stem_pair walks the track in ``chunk_seconds`` windows and
        overlap-adds them, so the last window is almost always short. At the
        shipped 16 s chunk, reaching a second window needs a track no edge-case
        test can afford to fuse - but the chunk length is a parameter, so the
        schedule runs here at 0.1 s: three windows and a 2944-frame remainder.

        Two identical inputs fuse to themselves, so whatever the overlap-add
        did to the tail shows up as a difference from the source.
        """
        frames = 10_000
        roformer = write_tone(tmp_path / "roformer" / "vocals.wav", frames)
        demucs = write_tone(tmp_path / "demucs" / "vocals.wav", frames)

        fused, _ = fuse_stem_pair(
            roformer,
            demucs,
            tmp_path / "out.wav",
            "vocals",
            chunk_seconds=0.1,
            overlap_seconds=0.02,
        )

        source, _ = sf.read(str(roformer), always_2d=True)

        assert fused.shape == (2, frames)
        assert np.max(np.abs(fused - source.T)) < 1e-3

    def test_single_frame_input(self, tmp_path: Path):
        roformer = tmp_path / "roformer"
        demucs = tmp_path / "demucs"
        out = tmp_path / "out"

        for stem in STEM_NAMES:
            write_tone(roformer / f"{stem}.wav", 1)
            write_tone(demucs / f"{stem}.wav", 1)

        fuse_stem_folders(
            roformer_dir=roformer,
            demucs_dir=demucs,
            output_dir=out,
        )

        assert (out / "vocals.wav").exists()


class TestForegroundSplit:
    def test_tail_and_tiny_files_split(self, tmp_path: Path):
        for frames in (1200, 1536, 508150):
            source = write_tone(tmp_path / f"src_{frames}.wav", frames)
            out = tmp_path / f"out_{frames}"

            result = split_foreground(source, out)

            assert result.foreground.exists()
            assert result.backing.exists()

            foreground, _ = sf.read(str(result.foreground), always_2d=True)
            assert foreground.shape[0] == frames


class TestKickEvents:
    def test_very_short_drums_produce_no_events(self):
        assert detect_kick_events(np.zeros((2, 16), dtype="float32"), SAMPLE_RATE) == []

    def test_empty_drums_produce_no_events(self):
        assert detect_kick_events(np.zeros((2, 0), dtype="float32"), SAMPLE_RATE) == []


class TestDegenerateAnalysis:
    def test_zero_frame_file_gives_finite_confidence(self, tmp_path: Path):
        path = tmp_path / "empty.wav"
        sf.write(str(path), np.zeros((0, 2), dtype="float32"), SAMPLE_RATE)

        profile = analyse_audio(path)

        assert np.isfinite(profile.confidence)
        assert np.isfinite(profile.rms)
        assert profile.estimated_source_count >= 1

    def test_nan_samples_give_finite_confidence(self, tmp_path: Path):
        path = tmp_path / "nan.wav"
        data = np.full((4096, 2), np.nan, dtype="float32")
        sf.write(str(path), data, SAMPLE_RATE, subtype="FLOAT")

        profile = analyse_audio(path)

        assert np.isfinite(profile.confidence)
        assert np.isfinite(profile.rms)


class TestManifestFloats:
    def test_non_finite_confidence_is_serialisable(self):
        import json

        assert _json_safe_float(float("nan")) == 0.0
        assert _json_safe_float(float("inf")) == 0.0
        assert _json_safe_float(0.123456789) == 0.12346

        # json.loads rejects the bare NaN token json.dumps would emit.
        payload = json.dumps({"confidence": _json_safe_float(float("nan"))})
        assert json.loads(payload) == {"confidence": 0.0}
