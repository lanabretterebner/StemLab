from __future__ import annotations

import numpy as np
from scipy import signal

from stemlab.refinement.kick import (
    KickRefinementConfig,
    refine_kick_bleed,
)


def rms(x):
    return float(np.sqrt(np.mean(np.square(x), dtype=np.float64)))


def make_kick(sr: int, seconds: float = 0.18):
    n = int(sr * seconds)
    t = np.arange(n, dtype=np.float32) / sr

    f0 = 135.0
    f1 = 52.0
    phase = 2 * np.pi * (
        f1 * t
        + (f0 - f1) * (1.0 - np.exp(-t * 24.0)) / 24.0
    )
    env = np.exp(-t * 22.0)
    click = np.exp(-t * 170.0) * np.sin(2 * np.pi * 1700.0 * t)

    return (
        0.90 * env * np.sin(phase)
        + 0.10 * click
    ).astype(np.float32)


def add_at(dst, sample, sound, gain=1.0):
    end = min(dst.shape[-1], sample + len(sound))
    length = end - sample
    if length > 0:
        dst[:, sample:end] += gain * sound[None, :length]


def main():
    sr = 44100
    duration = 2.4
    n = int(sr * duration)
    t = np.arange(n, dtype=np.float32) / sr

    # Legitimate non-drum source we want to preserve.
    bass_mono = (
        0.20 * np.sin(2 * np.pi * 73.4 * t)
        + 0.06 * np.sin(2 * np.pi * 146.8 * t)
    ).astype(np.float32)
    bass = np.stack([bass_mono, bass_mono], axis=0)

    drums = np.zeros((2, n), dtype=np.float32)
    leak = np.zeros((2, n), dtype=np.float32)

    kick = make_kick(sr)
    event_times = [0.30, 0.72, 1.12, 1.48, 1.86]

    for i, sec in enumerate(event_times):
        sample = int(sec * sr)
        velocity = [1.0, 0.78, 0.93, 0.70, 1.0][i]
        add_at(drums, sample, kick, gain=velocity)

        # Simulate bleed: delayed, quieter and spectrally altered.
        leaked = np.roll(kick, 17)
        leaked = signal.lfilter(
            [1.0, -0.16],
            [1.0],
            leaked,
        ).astype(np.float32)
        add_at(leak, sample, leaked, gain=0.26 * velocity)

    contaminated = bass + leak

    refined, stats = refine_kick_bleed(
        drums=drums,
        target=contaminated,
        sr=sr,
        cfg=KickRefinementConfig(),
    )

    before = rms(contaminated - bass)
    after = rms(refined - bass)

    # Boundary-click proxy: look at the largest first-difference introduced
    # by our correction signal. Hard splices would create very large impulses.
    correction = refined - contaminated
    max_correction_jump = float(np.max(np.abs(np.diff(correction, axis=-1))))

    print("events detected:", stats.events_detected)
    print("attempted:", stats.cancellations_attempted)
    print("applied:", stats.cancellations_applied)
    print("rejected by event confidence:", stats.rejected_event_confidence)
    print("rejected by match confidence:", stats.rejected_match_confidence)
    print("mean match confidence:", round(stats.mean_confidence, 4))
    print("bleed error before:", round(before, 6))
    print("bleed error after: ", round(after, 6))
    print(
        "bleed reduction:",
        round(100.0 * (1.0 - after / max(before, 1e-12)), 2),
        "%",
    )
    print("max correction sample jump:", round(max_correction_jump, 6))

    assert stats.cancellations_applied > 0
    assert after < before

    # Conservative sanity check: no giant impulse-like correction.
    assert max_correction_jump < 0.25

    print("WINDOWED REFINEMENT SMOKE TEST PASS")


if __name__ == "__main__":
    main()
