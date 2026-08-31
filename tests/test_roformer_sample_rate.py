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
