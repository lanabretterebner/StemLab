"""Demucs stems must match what the other engines publish.

Two differences, both silent. Demucs is never told a sample rate: it loads
at its own model rate and saves there (demucs/separate.py uses
model.samplerate for both, and every htdemucs/hdemucs variant sets it to
44100), so a 48 kHz session got 44.1 kHz stems back while the RoFormer path
restored the source rate. And StemLab passed neither --int24 nor --float32,
so demucs fell to its 16-bit default while every other backend here writes
32-bit float.

Neither made a stem play wrong - content and header agreed - which is why
choosing an engine quietly chose an output format too.
"""

from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from stemlab.audio import STEM_NAMES
from stemlab.demucs_backend import (
    DEFAULT_DEMUCS_MODEL,
    DEMUCS_MODEL_SAMPLE_RATE,
    DemucsBackend,
    _warn_if_not_float,
)
from stemlab.resample import rate_and_frames, restore_folder_sample_rate


def tone(path, rate, seconds=0.5, freq=440.0, subtype="FLOAT"):
    frames = int(rate * seconds)
    t = np.arange(frames, dtype="float32") / rate
    wave = (0.5 * np.sin(2 * np.pi * freq * t)).astype("float32")
    sf.write(str(path), np.column_stack([wave, wave]), rate, subtype=subtype)
    return path


def run(monkeypatch, source, output, *, model_rate=DEMUCS_MODEL_SAMPLE_RATE):
    """Drive separate() with the model replaced by a writer of real stems."""
    monkeypatch.setattr("stemlab.demucs_backend.resolve_torch_device", lambda *_a: "cpu")
    monkeypatch.setattr(
        "stemlab.demucs_backend._normalise_input_for_backend",
        lambda input_path, staging_dir, **_k: input_path,
    )

    seen: dict[str, list[str]] = {}

    def fake_run(command, _log, _progress, **_kwargs):
        # The backend probes for demucs by running an import through this
        # same helper before it separates anything. That call carries no
        # --out, and answering it is what "demucs is installed" looks like.
        if "--out" not in list(command):
            return 0

        seen["command"] = list(command)
        raw = list(command)[list(command).index("--out") + 1]
        from pathlib import Path

        # Demucs writes at its model rate whatever it was handed.
        for stem in STEM_NAMES:
            path = Path(raw) / DEFAULT_DEMUCS_MODEL / source.stem / f"{stem}.wav"
            path.parent.mkdir(parents=True, exist_ok=True)
            tone(path, model_rate)
        return 0

    monkeypatch.setattr("stemlab.demucs_backend.run_progress_process", fake_run)

    logs: list[str] = []
    placed = DemucsBackend(log_callback=logs.append).separate(input_path=source, output_dir=output)
    return placed, seen["command"], logs


# --- bit depth -----------------------------------------------------------


def test_the_command_asks_for_float32(tmp_path, monkeypatch):
    """Without this demucs falls to bits_per_sample=16."""
    source = tone(tmp_path / "song.wav", 44100)
    _, command, _ = run(monkeypatch, source, tmp_path / "stems")

    assert "--float32" in command


def test_float32_is_not_confused_with_disabling_prevent_clip(tmp_path, monkeypatch):
    """--float32 only sets the encoding; prevent_clip runs regardless.

    Pinned because it is exactly the thing that was got wrong once: the
    clip mode is a separate flag, and the CLI offers only rescale and clamp.
    """
    source = tone(tmp_path / "song.wav", 44100)
    _, command, _ = run(monkeypatch, source, tmp_path / "stems")

    assert "--clip-mode" not in command


def test_a_non_float_stem_is_reported_but_does_not_raise(tmp_path):
    """Lower fidelity is not a reason to throw away a finished separation."""
    logs: list[str] = []
    stems = [tone(tmp_path / "vocals.wav", 44100, subtype="PCM_16")]

    _warn_if_not_float(stems, logs.append)

    assert any("not 32-bit float" in line for line in logs), logs


def test_float_stems_are_reported_silently(tmp_path):
    logs: list[str] = []
    stems = [tone(tmp_path / "vocals.wav", 44100, subtype="FLOAT")]

    _warn_if_not_float(stems, logs.append)

    assert logs == []


def test_an_unreadable_stem_does_not_trip_the_float_check(tmp_path):
    junk = tmp_path / "notes.wav"
    junk.write_bytes(b"not audio")
    logs: list[str] = []

    _warn_if_not_float([junk], logs.append)

    assert logs == []


# --- sample rate ---------------------------------------------------------


@pytest.mark.parametrize("rate", [48000, 88200, 96000])
def test_stems_come_back_at_the_session_rate(tmp_path, monkeypatch, rate):
    """The regression: these used to arrive at 44.1 kHz whatever the source."""
    source = tone(tmp_path / "song.wav", rate, seconds=0.5)
    placed, _, _ = run(monkeypatch, source, tmp_path / "stems")

    for path in placed:
        assert sf.info(str(path)).samplerate == rate, path.name


def test_a_441_source_is_left_alone(tmp_path, monkeypatch):
    """No pointless round trip when demucs already wrote the right rate."""
    source = tone(tmp_path / "song.wav", 44100)
    placed, _, logs = run(monkeypatch, source, tmp_path / "stems")

    for path in placed:
        assert sf.info(str(path)).samplerate == 44100
    assert not any("Returning stems" in line for line in logs), logs


def test_restored_stems_keep_the_sources_length(tmp_path, monkeypatch):
    """A stem a sample off drifts against every other track in the session."""
    source = tone(tmp_path / "song.wav", 48000, seconds=0.5)
    expected = sf.info(str(source)).frames
    placed, _, _ = run(monkeypatch, source, tmp_path / "stems")

    for path in placed:
        assert sf.info(str(path)).frames == expected, path.name


def test_restoring_preserves_pitch(tmp_path, monkeypatch):
    """A real resample, not a relabel - the mistake made once already."""
    source = tone(tmp_path / "song.wav", 48000, seconds=1.0, freq=440.0)
    placed, _, _ = run(monkeypatch, source, tmp_path / "stems")

    data, rate = sf.read(str(placed[0]), always_2d=True)
    mono = data[:, 0] * np.hanning(len(data))
    peak = np.fft.rfftfreq(len(mono), 1 / rate)[int(np.argmax(np.abs(np.fft.rfft(mono))))]

    assert peak == pytest.approx(440.0, abs=2.0), peak


def test_an_unprobeable_source_does_not_fail_the_separation(tmp_path, monkeypatch):
    """The stems are fine; only the restore needed that probe."""
    source = tmp_path / "song.wav"
    source.write_bytes(b"not audio at all")

    placed, _, logs = run(monkeypatch, source, tmp_path / "stems")

    assert len(placed) == len(STEM_NAMES)
    assert any("Could not read the input" in line for line in logs), logs


# --- the shared helper ---------------------------------------------------


def test_the_restore_is_shared_with_the_roformer_backend():
    """One implementation, so a fix to either reaches both."""
    from stemlab import pretrained

    assert pretrained.restore_folder_sample_rate is restore_folder_sample_rate
    assert pretrained.rate_and_frames is rate_and_frames


def test_an_unknown_length_does_not_truncate(tmp_path):
    """None means the probe failed, which is not a length of zero."""
    folder = tmp_path / "stems"
    folder.mkdir()
    tone(folder / "vocals.wav", 44100, seconds=1.0)

    restore_folder_sample_rate(folder, 48000, None, lambda _m: None)

    info = sf.info(str(folder / "vocals.wav"))
    assert info.samplerate == 48000
    assert info.frames > 47000, info.frames
