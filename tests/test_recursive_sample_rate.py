"""Recursive children must come back at the rate they were split out of.

audio-separator defaults to 44100 and both loads and writes at that rate. The
plugin's monitor mix takes its rate from the first lane it reads and skips
every lane that disagrees, so a child at the wrong rate is dropped from the
mix without a sound - which reads as the parent stem's Solo and Mute having
stopped working, not as a rate problem.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from stemlab.recursive import (
    RECURSIVE_FALLBACK_SAMPLE_RATE,
    _separator,
    _source_sample_rate,
)


def write(path: Path, rate: int, seconds: float = 0.25) -> Path:
    frames = int(rate * seconds)
    data = np.zeros((frames, 2), dtype="float32")
    sf.write(str(path), data, rate)
    return path


@pytest.mark.parametrize("rate", [44100, 48000, 88200, 96000, 22050])
def test_the_source_rate_is_read_from_the_stem(tmp_path, rate):
    assert _source_sample_rate(write(tmp_path / "drums.wav", rate)) == rate


def test_an_unreadable_stem_falls_back_rather_than_raising(tmp_path):
    """A split that cannot read its own input still has to produce something."""
    broken = tmp_path / "drums.wav"
    broken.write_bytes(b"RIFF....WAVEfmt not-really")

    assert _source_sample_rate(broken) == RECURSIVE_FALLBACK_SAMPLE_RATE


def test_the_separator_is_told_the_session_rate(tmp_path, monkeypatch):
    """The whole bug in one assertion: audio-separator must not pick 44100.

    Without an explicit sample_rate it uses its own default and writes every
    child at 44.1 kHz, whatever the session runs at.
    """
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


def test_every_split_entry_point_passes_the_input_rate(tmp_path):
    """Three call sites; a new one that forgets this reintroduces the bug."""
    source = Path("src/stemlab/recursive.py").read_text(encoding="utf-8")

    calls = [line for line in source.splitlines() if "_separator(output_dir, model_dir" in line]

    assert len(calls) == 3, calls
    for line in calls:
        assert "_source_sample_rate(input_path)" in line, line
