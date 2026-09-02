"""Recursive children must come back at the rate they were split out of.

Two separate ways that went wrong. audio-separator defaults to 44100 and both
loads and writes at that rate, so a child of a 48 kHz session came back at
44.1 kHz; the plugin's monitor mix takes its rate from the first lane it reads
and skips every lane that disagrees, so the child was dropped from the mix
without a sound - which reads as the parent's Solo and Mute having stopped
working, not as a rate problem.

Passing the session's rate fixed that for the MDXC models but not for the VR
one behind VOCAL_MODEL, which ignores the request and always emits 44.1 kHz
while the caller's rate still becomes the file's header. That child was not
dropped - it played, 8.8% fast and about 1.5 semitones sharp.
"""

from __future__ import annotations

import inspect
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from stemlab import recursive
from stemlab.recursive import (
    DEVERB_MODEL,
    DRUM_MODEL,
    RECURSIVE_FALLBACK_SAMPLE_RATE,
    VOCAL_MODEL,
    VR_BACKEND_SAMPLE_RATE,
    _backend_sample_rate,
    _conform_sample_rate,
    _separator,
    _source_rate_and_frames,
    deverb_vocal,
    split_drums,
    split_vocals,
)


def write(path: Path, rate: int, seconds: float = 0.25, freq: float = 440.0) -> Path:
    frames = int(rate * seconds)
    t = np.arange(frames, dtype="float32") / rate
    tone = np.sin(2 * np.pi * freq * t).astype("float32")
    sf.write(str(path), np.column_stack([tone, tone]), rate)
    return path


@pytest.mark.parametrize("rate", [44100, 48000, 88200, 96000, 22050])
def test_the_source_rate_is_read_from_the_stem(tmp_path, rate):
    probed, frames = _source_rate_and_frames(write(tmp_path / "drums.wav", rate))

    assert probed == rate
    assert frames == int(rate * 0.25)


def test_an_unreadable_stem_falls_back_rather_than_raising(tmp_path):
    """A split that cannot read its own input still has to produce something."""
    broken = tmp_path / "drums.wav"
    broken.write_bytes(b"RIFF....WAVEfmt not-really")

    assert _source_rate_and_frames(broken) == (RECURSIVE_FALLBACK_SAMPLE_RATE, None)


def test_the_separator_is_told_the_rate_it_is_given(tmp_path, monkeypatch):
    """Without an explicit sample_rate audio-separator picks its own 44100."""
    captured: dict[str, object] = {}

    class FakeSeparator:
        def __init__(self, **kwargs):
            captured.update(kwargs)

    monkeypatch.setattr("stemlab.recursive._require_separator", lambda: FakeSeparator)

    _separator(tmp_path / "out", tmp_path / "models", 48000)

    assert captured["sample_rate"] == 48000, captured
    # The rest of the contract, so a future edit cannot quietly drop one.
    assert captured["output_format"] == "WAV"
    assert captured["use_soundfile"] is True


# --- which rate each backend is actually asked for ------------------------


@pytest.mark.parametrize("rate", [44100, 48000, 88200, 96000])
def test_the_mdxc_models_are_asked_for_the_session_rate(rate):
    """.ckpt models run on MDXC, which honours the rate end to end."""
    assert _backend_sample_rate(DRUM_MODEL, rate) == rate
    assert _backend_sample_rate(DEVERB_MODEL, rate) == rate


@pytest.mark.parametrize("rate", [44100, 48000, 88200, 96000])
def test_the_vr_model_is_asked_for_the_only_rate_it_can_produce(rate):
    """The bug: VOCAL_MODEL is a .pth, and VR always returns 44.1 kHz.

    Asking it for 48000 does not resample anything - it relabels 44.1 kHz
    audio as 48 kHz. So it is asked for 44100, and the output is put back on
    the session's rate by _conform_sample_rate afterwards.
    """
    assert _backend_sample_rate(VOCAL_MODEL, rate) == VR_BACKEND_SAMPLE_RATE


def test_the_vr_model_really_is_the_pth_one():
    """If VOCAL_MODEL is ever swapped for a .ckpt this stops being needed."""
    assert Path(VOCAL_MODEL).suffix.lower() == ".pth"
    assert Path(DRUM_MODEL).suffix.lower() == ".ckpt"
    assert Path(DEVERB_MODEL).suffix.lower() == ".ckpt"


# --- conforming the children ----------------------------------------------


def test_a_child_at_the_backends_rate_is_returned_to_the_session_rate(tmp_path):
    child = write(tmp_path / "lead_vocals.wav", 44100, seconds=1.0)

    _conform_sample_rate([child], 48000, 48000)

    info = sf.info(str(child))
    assert info.samplerate == 48000
    assert info.frames == 48000


def test_a_child_already_at_the_session_rate_is_left_alone(tmp_path):
    child = write(tmp_path / "lead_vocals.wav", 48000, seconds=1.0)
    before = child.read_bytes()

    _conform_sample_rate([child], 48000, 48000)

    assert child.read_bytes() == before


def test_conforming_preserves_pitch(tmp_path):
    """The symptom that started this: the stem came back sharp.

    A 440 Hz tone resampled 44.1 -> 48 kHz must still be 440 Hz. Relabelling
    instead of resampling would put it at 440 * 48000/44100 = 478.9 Hz.
    """
    child = write(tmp_path / "lead_vocals.wav", 44100, seconds=1.0, freq=440.0)

    _conform_sample_rate([child], 48000, 48000)

    data, rate = sf.read(str(child), always_2d=True)
    mono = data[:, 0]
    spectrum = np.abs(np.fft.rfft(mono * np.hanning(len(mono))))
    peak_hz = np.fft.rfftfreq(len(mono), 1 / rate)[int(np.argmax(spectrum))]

    assert peak_hz == pytest.approx(440.0, abs=2.0), peak_hz


def test_conforming_pins_the_length_to_the_parent(tmp_path):
    """A child a sample off drifts against the parent Solo subtracts it from."""
    child = write(tmp_path / "lead_vocals.wav", 44100, seconds=1.0)

    _conform_sample_rate([child], 48000, 12345)

    assert sf.info(str(child)).frames == 12345


def test_an_unconvertible_child_fails_the_split_rather_than_playing_wrong(
    tmp_path, monkeypatch
):
    """Silently mis-rated audio is worse than a split that says it failed."""
    child = write(tmp_path / "lead_vocals.wav", 44100, seconds=0.25)

    def boom(*args, **kwargs):
        raise OSError("no space left on device")

    monkeypatch.setattr("stemlab.resample.resample_file", boom)

    with pytest.raises(RuntimeError, match="wrong pitch and speed"):
        _conform_sample_rate([child], 48000, 12000)

    assert not (tmp_path / "lead_vocals_stemlab_rate.wav").exists()


def test_an_unprobeable_parent_does_not_truncate_its_children(tmp_path):
    """A failed length probe is not a length of zero.

    audio-separator reads through librosa, which opens formats libsndfile
    will not, so a parent this cannot probe can still separate. Pinning the
    children to a failed probe would hand back silence.
    """
    child = write(tmp_path / "lead_vocals.wav", 44100, seconds=1.0)

    _conform_sample_rate([child], 48000, None)

    info = sf.info(str(child))
    assert info.samplerate == 48000
    assert info.frames > 47000, info.frames


def test_an_unreadable_file_does_not_fail_the_split(tmp_path):
    """Whatever else the model left in the output dir is not a child."""
    junk = tmp_path / "notes.wav"
    junk.write_bytes(b"not audio at all")

    _conform_sample_rate([junk], 48000, 12000)

    assert junk.read_bytes() == b"not audio at all"


# --- the wiring -----------------------------------------------------------


@pytest.mark.parametrize(
    ("split", "model"),
    [(split_drums, "DRUM_MODEL"), (split_vocals, "VOCAL_MODEL"), (deverb_vocal, "DEVERB_MODEL")],
)
def test_every_split_asks_its_own_backend_and_conforms_the_result(split, model):
    """Three call sites; a new one that forgets either half reopens this."""
    source = inspect.getsource(split)

    assert f"_backend_sample_rate({model}, source_rate)" in source, source
    assert "_conform_sample_rate(paths, source_rate, source_frames" in source, source


def test_no_split_asks_for_a_rate_without_conforming_the_output():
    """Guards the pairing itself across the whole module."""
    # The module as it was imported, rather than a path relative to whatever
    # directory pytest was started in: the working-directory form failed
    # outright from anywhere but the repository root, and against an installed
    # StemLab it read the tree's copy instead of the code that runs.
    source = inspect.getsource(recursive)

    assert source.count("_backend_sample_rate(") == 4  # one def, three calls
    assert source.count("_conform_sample_rate(") == 4
