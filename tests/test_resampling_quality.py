"""The fusion path resamples one engine's stems onto the other's rate, so a
weak anti-alias filter there folds inaudible content back into the music.
scipy's ``resample_poly`` did exactly that; these tests pin the replacement
so nobody swaps it back for something more convenient.
"""

from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from stemlab.audio import load_audio

SOURCE_RATE = 48000
TARGET_RATE = 44100


def _write(path, audio, sample_rate=SOURCE_RATE):
    sf.write(str(path), np.asarray(audio).T, sample_rate, subtype="FLOAT")


def _rms_dbfs(audio):
    flat = np.asarray(audio, dtype=np.float64).ravel()
    return 20.0 * np.log10(max(float(np.sqrt((flat**2).mean())), 1e-20))


def _tone(frequency, seconds=2.0, sample_rate=SOURCE_RATE):
    time = np.arange(int(seconds * sample_rate)) / sample_rate
    mono = np.sin(2.0 * np.pi * frequency * time).astype(np.float32)
    return np.stack((mono, mono))


def test_content_above_the_output_nyquist_is_rejected_not_folded_back(tmp_path):
    """A 23 kHz tone cannot exist at 44.1 kHz. It must be filtered away, not
    aliased down into the audible band - resample_poly left it at -14 dBFS."""
    source = tmp_path / "above_nyquist.wav"
    _write(source, _tone(23_000.0))

    resampled, rate = load_audio(source, target_sr=TARGET_RATE)

    assert rate == TARGET_RATE
    assert _rms_dbfs(resampled) < -55.0


def test_audible_content_survives_resampling(tmp_path):
    """Rejecting everything would also pass the test above, so assert that a
    tone comfortably inside the passband comes through at its original level."""
    source = tmp_path / "in_band.wav"
    _write(source, _tone(1_000.0))

    resampled, _ = load_audio(source, target_sr=TARGET_RATE)

    # A full-scale sine sits at about -3 dBFS RMS.
    assert -4.0 < _rms_dbfs(resampled) < -2.0


def test_length_follows_the_rate_ratio(tmp_path):
    """Fusion aligns two engines' stems by sample index, so a resampler that
    rounds length differently would desync them against the session."""
    source = tmp_path / "length.wav"
    _write(source, _tone(440.0, seconds=3.0))

    resampled, _ = load_audio(source, target_sr=TARGET_RATE)

    expected = round(3.0 * SOURCE_RATE * TARGET_RATE / SOURCE_RATE)
    assert abs(resampled.shape[1] - expected) <= 1


def test_a_matching_rate_is_passed_through_untouched(tmp_path):
    """Most sessions are already at the target rate; they must not be paid for."""
    source = tmp_path / "same_rate.wav"
    audio = _tone(440.0, seconds=1.0, sample_rate=TARGET_RATE)
    _write(source, audio, sample_rate=TARGET_RATE)

    resampled, rate = load_audio(source, target_sr=TARGET_RATE)

    assert rate == TARGET_RATE
    np.testing.assert_allclose(resampled, audio, atol=1e-6)


@pytest.mark.parametrize("source_rate", [22_050, 32_000, 48_000, 88_200, 96_000])
def test_common_session_rates_all_resample_cleanly(tmp_path, source_rate):
    """Whatever the session runs at, the result must be finite, correctly
    rated, and roughly the right length."""
    source = tmp_path / f"rate_{source_rate}.wav"
    _write(source, _tone(1_000.0, seconds=1.0, sample_rate=source_rate), source_rate)

    resampled, rate = load_audio(source, target_sr=TARGET_RATE)

    assert rate == TARGET_RATE
    assert np.isfinite(resampled).all()
    assert abs(resampled.shape[1] - TARGET_RATE) <= 2
