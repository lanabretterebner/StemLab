import numpy as np
from scipy import signal

from stemlab.refinement.events import detect_kick_events
from stemlab.refinement.kick import KickRefinementConfig, refine_kick_bleed


def test_detects_impulses():
    sr = 8000
    x = np.zeros((2, sr), dtype=np.float32)

    for sample in (1000, 3000, 5000):
        n = min(300, sr - sample)
        t = np.arange(n, dtype=np.float32) / sr
        kick = np.exp(-t * 30.0) * np.sin(2 * np.pi * 80.0 * t)
        x[:, sample : sample + n] += kick

    events = detect_kick_events(
        x,
        sr=sr,
        threshold_std=0.4,
    )

    assert len(events) >= 2


def _rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(audio), dtype=np.float64)))


def _make_kick(sample_rate: int, seconds: float = 0.18) -> np.ndarray:
    sample_count = int(sample_rate * seconds)
    time = np.arange(sample_count, dtype=np.float32) / sample_rate
    phase = 2 * np.pi * (52.0 * time + (135.0 - 52.0) * (1.0 - np.exp(-time * 24.0)) / 24.0)
    body = 0.90 * np.exp(-time * 22.0) * np.sin(phase)
    click = 0.10 * np.exp(-time * 170.0) * np.sin(2 * np.pi * 1700.0 * time)
    return (body + click).astype(np.float32)


def _add_sound(destination: np.ndarray, start: int, sound: np.ndarray, gain: float) -> None:
    length = min(destination.shape[-1] - start, len(sound))
    if length > 0:
        destination[:, start : start + length] += gain * sound[None, :length]


def test_kick_refinement_reduces_synthetic_bleed():
    """The refinement should remove matched bleed without creating clicks."""
    sample_rate = 44_100
    sample_count = int(sample_rate * 2.4)
    time = np.arange(sample_count, dtype=np.float32) / sample_rate
    bass_mono = (
        0.20 * np.sin(2 * np.pi * 73.4 * time) + 0.06 * np.sin(2 * np.pi * 146.8 * time)
    ).astype(np.float32)
    clean_bass = np.stack([bass_mono, bass_mono])
    drums = np.zeros((2, sample_count), dtype=np.float32)
    bleed = np.zeros_like(drums)
    kick = _make_kick(sample_rate)

    for seconds, velocity in zip(
        (0.30, 0.72, 1.12, 1.48, 1.86),
        (1.0, 0.78, 0.93, 0.70, 1.0),
        strict=True,
    ):
        start = int(seconds * sample_rate)
        _add_sound(drums, start, kick, velocity)
        leaked = signal.lfilter([1.0, -0.16], [1.0], np.roll(kick, 17)).astype(np.float32)
        _add_sound(bleed, start, leaked, 0.26 * velocity)

    contaminated = clean_bass + bleed
    refined, stats = refine_kick_bleed(
        drums=drums,
        target=contaminated,
        sr=sample_rate,
        cfg=KickRefinementConfig(),
    )

    correction = refined - contaminated
    assert stats.cancellations_applied > 0
    assert _rms(refined - clean_bass) < _rms(contaminated - clean_bass)
    assert float(np.max(np.abs(np.diff(correction, axis=-1)))) < 0.25
