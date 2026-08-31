"""BS-RoFormer is fed the rate it was trained at, and its stems come back.

The model's band split is defined in FFT bins rather than in Hz, so its band
edges only land on the frequencies it was trained for when the input is at
44.1 kHz. A 48 kHz session moved every edge up by 8.8%.

Restoring the rate afterwards is the other half: fusion reads its rate off
the RoFormer stem itself, so a stem left at 44.1 kHz would make every fused
output of a 48 kHz session 44.1 kHz too.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from stemlab import pretrained
from stemlab.audio import STEM_NAMES
from stemlab.pretrained import ROFORMER_SAMPLE_RATE, RoFormerBackend


def _tone(path: Path, rate: int, seconds: float = 0.5, hz: float = 440.0) -> Path:
    t = np.arange(int(rate * seconds), dtype=np.float64) / rate
    wave = (0.25 * np.sin(2 * math.pi * hz * t)).astype(np.float32)
    sf.write(str(path), np.column_stack([wave, wave]), rate, subtype="FLOAT")
    return path


class _FakeSeparator:
    """Stands in for the CLI, recording what rate it was actually handed."""

    def __init__(self) -> None:
        self.seen_rates: list[int] = []

    def __call__(self, command, _log, _progress, **_kwargs) -> int:
        command = list(command)
        folder = Path(command[command.index("--input_folder") + 1])
        store = Path(command[command.index("--store_dir") + 1])

        source = sorted(folder.glob("*.wav"))[0]
        data, rate = sf.read(str(source), always_2d=True, dtype="float32")
        self.seen_rates.append(rate)

        store.mkdir(parents=True, exist_ok=True)
        for stem in STEM_NAMES:
            # Upstream keeps the separator's own "{track}_{stem}.wav".
            sf.write(str(store / f"{source.stem}_{stem}.wav"), data, rate, subtype="FLOAT")
        return 0


def _run(monkeypatch, source: Path, output: Path) -> _FakeSeparator:
    separator = _FakeSeparator()
    monkeypatch.setattr("stemlab.pretrained.run_progress_process", separator)
    monkeypatch.setattr("stemlab.pretrained.resolve_torch_device", lambda *_a, **_k: "cpu")
    RoFormerBackend(device="cpu", log_callback=lambda _m: None).separate(source, output)
    return separator


@pytest.mark.parametrize("session_rate", [48000, 96000, 22050])
def test_the_model_is_fed_the_rate_it_was_trained_at(monkeypatch, tmp_path, session_rate):
    source = _tone(tmp_path / "song.wav", session_rate)
    separator = _run(monkeypatch, source, tmp_path / "stems")

    assert separator.seen_rates == [ROFORMER_SAMPLE_RATE]


def test_a_441_source_is_handed_over_untouched(monkeypatch, tmp_path):
    """No needless round trip: resampling 44.1 to 44.1 would only lose."""
    source = _tone(tmp_path / "song.wav", ROFORMER_SAMPLE_RATE)
    before = source.read_bytes()

    separator = _run(monkeypatch, source, tmp_path / "stems")

    assert separator.seen_rates == [ROFORMER_SAMPLE_RATE]
    assert source.read_bytes() == before


@pytest.mark.parametrize("session_rate", [48000, 96000, 22050])
def test_stems_come_back_at_the_session_rate(monkeypatch, tmp_path, session_rate):
    source = _tone(tmp_path / "song.wav", session_rate)
    output = tmp_path / "stems"

    _run(monkeypatch, source, output)

    written = sorted(output.rglob("*.wav"))
    assert written, "the separator produced no stems"
    for path in written:
        assert sf.info(str(path)).samplerate == session_rate, path.name


def test_the_round_trip_keeps_the_pitch(monkeypatch, tmp_path):
    """The failure this guards against is audible: 48k treated as 44.1k
    plays 8.8% fast, about 1.5 semitones sharp."""
    session_rate = 48000
    source = _tone(tmp_path / "song.wav", session_rate, seconds=1.0, hz=440.0)
    output = tmp_path / "stems"

    _run(monkeypatch, source, output)

    stem = sorted(output.rglob("*.wav"))[0]
    data, rate = sf.read(str(stem), always_2d=True, dtype="float32")
    mono = data[:, 0]

    spectrum = np.abs(np.fft.rfft(mono * np.hanning(len(mono))))
    peak_hz = float(np.fft.rfftfreq(len(mono), 1.0 / rate)[int(np.argmax(spectrum))])

    assert peak_hz == pytest.approx(440.0, abs=3.0), peak_hz


@pytest.mark.parametrize("session_rate", [44100, 48000, 96000])
def test_a_flac_source_reaches_the_separator(monkeypatch, tmp_path, session_rate):
    """The CLI globs "*.wav", so a staged .flac was invisible to it.

    44100 is the case that never worked: it needs no rate conforming, so
    nothing incidentally produced a WAV for the CLI to find.
    """
    source = tmp_path / "song.flac"
    t = np.arange(int(session_rate * 0.4), dtype=np.float64) / session_rate
    wave = (0.25 * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)
    sf.write(str(source), np.column_stack([wave, wave]), session_rate)

    output = tmp_path / "stems"
    separator = _run(monkeypatch, source, output)

    assert separator.seen_rates == [ROFORMER_SAMPLE_RATE]

    written = sorted(output.rglob("*.wav"))
    assert written, "the separator saw no input it could open"
    for path in written:
        assert sf.info(str(path)).samplerate == session_rate, path.name
        # The separator names its stems after the file it was handed, so
        # staging under a marker name would put that marker in every stem
        # of every FLAC session. 48 kHz already produced "song_*" before
        # FLAC was staged at all, and it still has to.
        assert path.name.startswith("song_"), path.name


def test_conforming_leaves_exactly_one_input_for_the_cli(monkeypatch, tmp_path):
    """Two inputs in the folder would separate the same audio twice."""
    seen: dict[str, int] = {}

    class _Counting(_FakeSeparator):
        def __call__(self, command, _log, _progress, **kwargs):
            command = list(command)
            folder = Path(command[command.index("--input_folder") + 1])
            seen["wavs"] = len(sorted(folder.glob("*.wav")))
            seen["files"] = len(sorted(folder.iterdir()))
            return super().__call__(command, _log, _progress, **kwargs)

    separator = _Counting()
    monkeypatch.setattr("stemlab.pretrained.run_progress_process", separator)
    monkeypatch.setattr("stemlab.pretrained.resolve_torch_device", lambda *_a, **_k: "cpu")

    source = tmp_path / "song.flac"
    t = np.arange(int(48000 * 0.3), dtype=np.float64) / 48000
    wave = (0.25 * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)
    sf.write(str(source), np.column_stack([wave, wave]), 48000)

    RoFormerBackend(device="cpu", log_callback=lambda _m: None).separate(source, tmp_path / "out")

    assert seen == {"wavs": 1, "files": 1}


def _staged_input_info(monkeypatch, source: Path, output: Path):
    """Run a separation and report the header of the file the CLI was given."""
    seen: dict[str, object] = {}

    class _Probing(_FakeSeparator):
        def __call__(self, command, _log, _progress, **kwargs):
            command = list(command)
            folder = Path(command[command.index("--input_folder") + 1])
            info = sf.info(str(sorted(folder.glob("*.wav"))[0]))
            seen["subtype"] = info.subtype
            seen["frames"] = int(info.frames)
            return super().__call__(command, _log, _progress, **kwargs)

    monkeypatch.setattr("stemlab.pretrained.run_progress_process", _Probing())
    monkeypatch.setattr("stemlab.pretrained.resolve_torch_device", lambda *_a, **_k: "cpu")
    RoFormerBackend(device="cpu", log_callback=lambda _m: None).separate(source, output)
    return seen


def test_conforming_does_not_requantise_a_16_bit_source(monkeypatch, tmp_path):
    """The rate trip is an extra quantisation the source never asked for.

    The conformed file is the model's throwaway input, written once and read
    once, so widening it costs nothing and keeps 16-bit material from being
    rounded a second time on its way in. Stems keep the width they were
    written at - those are the deliverable.
    """
    source = tmp_path / "song.wav"
    t = np.arange(int(48000 * 0.2), dtype=np.float64) / 48000
    wave = (0.25 * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)
    sf.write(str(source), np.column_stack([wave, wave]), 48000, subtype="PCM_16")

    seen = _staged_input_info(monkeypatch, source, tmp_path / "stems")

    assert seen["subtype"] == "PCM_24"


@pytest.mark.parametrize("session_rate,frames", [(96000, 1), (192000, 2)])
def test_a_source_shorter_than_one_target_frame_still_reaches_the_cli(
    monkeypatch, tmp_path, session_rate, frames
):
    """Downsampling a handful of frames can round the length away to zero.

    96 kHz for one frame and 192 kHz for one or two all resample to no
    frames at all, and the CLI has no answer for an empty WAV: it is the
    degenerate input that would crash a separation rather than fail it.
    """
    source = tmp_path / "song.wav"
    sf.write(str(source), np.zeros((frames, 2), dtype=np.float32), session_rate, subtype="FLOAT")

    seen = _staged_input_info(monkeypatch, source, tmp_path / "stems")

    assert seen["frames"] >= 1


def test_a_failed_flac_conversion_leaves_nothing_behind_for_ffmpeg(monkeypatch, tmp_path):
    """The soundfile and ffmpeg paths stage under different names.

    A conversion that dies partway leaves a stub WAV, and ffmpeg then writes
    beside it rather than over it - two inputs in a folder the CLI globs, so
    it separates the wrong one or both.
    """
    source = tmp_path / "song.flac"
    t = np.arange(int(48000 * 0.2), dtype=np.float64) / 48000
    wave = (0.25 * np.sin(2 * math.pi * 440.0 * t)).astype(np.float32)
    sf.write(str(source), np.column_stack([wave, wave]), 48000)

    real_write = sf.write

    def _die_after_creating_the_file(path, data, samplerate, *args, **kwargs):
        Path(path).write_bytes(b"RIFF-truncated")
        raise RuntimeError("disk full")

    monkeypatch.setattr(sf, "write", _die_after_creating_the_file)
    staging_seen: dict[str, list[str]] = {}

    def _no_ffmpeg() -> None:
        # Stop at the fallback rather than depending on ffmpeg being
        # installed: what matters is the state of the folder by then.
        staging_seen["files"] = sorted(p.name for p in staged_dir[0].iterdir())
        raise RuntimeError("stop here")

    staged_dir: list[Path] = []
    real_normalise = pretrained._normalise_input_for_backend

    def _watching(*, staging_dir, **kwargs):
        staged_dir.append(staging_dir)
        return real_normalise(staging_dir=staging_dir, **kwargs)

    monkeypatch.setattr(pretrained, "_normalise_input_for_backend", _watching)
    monkeypatch.setattr(pretrained, "_find_ffmpeg", lambda: _no_ffmpeg())
    monkeypatch.setattr("stemlab.pretrained.resolve_torch_device", lambda *_a, **_k: "cpu")

    with pytest.raises(RuntimeError):
        RoFormerBackend(device="cpu", log_callback=lambda _m: None).separate(
            source, tmp_path / "stems"
        )

    sf.write = real_write
    assert staging_seen["files"] == []


def test_an_upper_case_wav_reaches_the_separator(monkeypatch, tmp_path):
    """The CLI's glob("*.wav") is case-sensitive, and .WAV is an ordinary name.

    Staging copied the source's own name through, so a Windows-authored
    SONG.WAV failed with "No .wav files found" on any case-sensitive
    filesystem - the same root cause that hid a staged FLAC.
    """
    source = _tone(tmp_path / "SONG.WAV", ROFORMER_SAMPLE_RATE)
    output = tmp_path / "stems"

    separator = _run(monkeypatch, source, output)

    assert separator.seen_rates == [ROFORMER_SAMPLE_RATE]
    assert sorted(output.rglob("*.wav")), "the separator saw no input it could open"
