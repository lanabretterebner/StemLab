import numpy as np

from stemlab.refinement.events import detect_kick_events


def test_detects_impulses():
    sr = 8000
    x = np.zeros((2, sr), dtype=np.float32)

    for sample in (1000, 3000, 5000):
        n = min(300, sr - sample)
        t = np.arange(n, dtype=np.float32) / sr
        kick = np.exp(-t * 30.0) * np.sin(2 * np.pi * 80.0 * t)
        x[:, sample:sample+n] += kick

    events = detect_kick_events(
        x,
        sr=sr,
        threshold_std=0.4,
    )

    assert len(events) >= 2
