"""Tempo must survive Beat This!'s 20 ms frame grid.

Beat This! reports every beat as an integer frame index divided by 50 fps
(``beat_time = beat_frame / self.fps`` in its postprocessor), so beat
intervals arrive quantised to 20 ms. Taking the median of those intervals
snaps the tempo to a whole number of frames: a 174 BPM track, whose true
beat is 17.24 frames, came back as 176.47 BPM because most of its intervals
round to 17. Only tempos whose period is an exact multiple of 20 ms were
unaffected, which is why this went unnoticed at 120 and 100.
"""

from __future__ import annotations

import numpy as np
import pytest

from stemlab.beat_tracking import _robust_intervals, derive_musical_time

FPS = 50.0


def quantised_beats(bpm: float, seconds: float = 180.0) -> np.ndarray:
    """Beats at ``bpm``, snapped to the frame grid Beat This! reports on."""
    period = 60.0 / bpm
    return np.round(np.arange(0.0, seconds, period) * FPS) / FPS


def detected(bpm: float, **kwargs) -> float:
    beats = quantised_beats(bpm, **kwargs)
    # Four beats to the bar, so the meter estimate has something to read.
    analysis = derive_musical_time(
        beats, beats[::4], 0.9, model="test", device="cpu"
    )
    return analysis.detected_bpm


@pytest.mark.parametrize(
    "bpm",
    [174.0, 175.0, 128.0, 140.0, 96.0, 90.0, 123.0, 137.0, 145.0, 168.0],
)
def test_a_tempo_off_the_frame_grid_is_still_reported_correctly(bpm):
    """The regression: every one of these landed on the wrong whole frame."""
    assert detected(bpm) == pytest.approx(bpm, abs=0.5)


@pytest.mark.parametrize("bpm", [120.0, 100.0, 150.0, 75.0])
def test_a_tempo_on_the_frame_grid_still_works(bpm):
    """These never broke - they must not break now."""
    assert detected(bpm) == pytest.approx(bpm, abs=0.5)


def test_the_median_really_does_get_174_wrong():
    """Pins the bug itself, so this test cannot pass for the wrong reason.

    If a future edit puts the median back, the assertion above stops being
    an accident of the arithmetic and starts failing - but only if the
    median genuinely disagrees here, which is what this proves.
    """
    intervals, _, _ = _robust_intervals(quantised_beats(174.0))

    assert 60.0 / float(np.median(intervals)) == pytest.approx(176.47, abs=0.05)
    assert 60.0 / float(np.mean(intervals)) == pytest.approx(174.0, abs=0.05)


def test_half_and_double_time_follow_the_corrected_tempo():
    beats = quantised_beats(174.0)
    analysis = derive_musical_time(beats, beats[::4], 0.9, model="t", device="cpu")

    assert analysis.bpm == pytest.approx(174.0, abs=0.5)
    assert analysis.half_time_bpm == pytest.approx(87.0, abs=0.25)
    assert analysis.double_time_bpm == pytest.approx(348.0, abs=1.0)


# --- the reason it is a mean over inliers, not a fit over the whole span ---


def _with_dropped_beats(bpm: float, fraction: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    beats = quantised_beats(bpm)
    return beats[rng.random(beats.size) > fraction]


def _with_spurious_beats(bpm: float, fraction: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    beats = quantised_beats(bpm)
    extra = rng.uniform(0.0, float(beats[-1]), int(beats.size * fraction))
    return np.unique(np.round(np.concatenate([beats, extra]) * FPS) / FPS)


@pytest.mark.parametrize("seed", [1, 7, 13])
def test_a_dropped_beat_does_not_move_the_tempo(seed):
    """A least-squares fit over beat index returned 166.20 on this input.

    It reads tempo from the whole span, so one missing beat shifts every
    index after it. The mean works on surviving intervals instead, and
    _robust_intervals has already dropped the doubled one as an outlier.
    """
    beats = _with_dropped_beats(174.0, 0.05, seed)
    analysis = derive_musical_time(beats, beats[::4], 0.9, model="t", device="cpu")

    assert analysis.detected_bpm == pytest.approx(174.0, abs=1.0)


@pytest.mark.parametrize("seed", [1, 7, 13])
def test_a_spurious_beat_does_not_move_the_tempo(seed):
    """The same fit returned 183.57 here."""
    beats = _with_spurious_beats(174.0, 0.05, seed)
    analysis = derive_musical_time(beats, beats[::4], 0.9, model="t", device="cpu")

    assert analysis.detected_bpm == pytest.approx(174.0, abs=1.5)


def test_jitter_within_a_frame_does_not_move_the_tempo():
    rng = np.random.default_rng(3)
    period = 60.0 / 174.0
    times = np.arange(0.0, 180.0, period) + rng.normal(0.0, 0.008, int(180.0 / period) + 1)[
        : len(np.arange(0.0, 180.0, period))
    ]
    beats = np.unique(np.round(np.sort(times) * FPS) / FPS)
    analysis = derive_musical_time(beats, beats[::4], 0.9, model="t", device="cpu")

    assert analysis.detected_bpm == pytest.approx(174.0, abs=1.0)


def test_the_cache_version_moved_with_the_tempo_change():
    """A cached analysis outlives the fix unless the key changes.

    BEAT_ALGORITHM_VERSION is part of the analysis cache's primary key
    (kind, source_hash, algorithm_version, settings_hash). Leaving it alone
    would serve the frame-quantised BPM back from sqlite for every track
    already analysed - including, on the machine that reported this, the
    one track guaranteed to have been analysed already.
    """
    from stemlab.beat_tracking import BEAT_ALGORITHM_VERSION

    assert BEAT_ALGORITHM_VERSION != "beat-this-1.1.0-stemlab-1"
