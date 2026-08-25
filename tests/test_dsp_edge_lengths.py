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
from stemlab.hybrid import fuse_stem_folders
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


# hop is chunk - overlap; a remainder in 1..1536 frames used to crash.
TAIL_CRASHING_LENGTHS = [
    1,
    1000,
    1536,
    508150,  # 11.52 s: one full 12 s chunk plus a 1000-frame tail
]


class TestHybridFusion:
    def test_short_tail_chunks_fuse(self, tmp_path: Path):
        for frames in (1000, 1536, 508150):
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
