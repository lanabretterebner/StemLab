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


def _quantise(times: np.ndarray) -> np.ndarray:
    """The 20 ms grid Beat This! reports every beat on."""
    return np.unique(np.round(np.asarray(times) * FPS) / FPS)


def _synthetic_beats(
    bpm: float, seconds: float = 180.0, drift_bpm: float = 0.0
) -> np.ndarray:
    """Beats at ``bpm``, optionally ramping by ``drift_bpm`` across the track."""
    time, beats = 0.0, []
    while time < seconds:
        beats.append(time)
        time += 60.0 / (bpm + drift_bpm * (time / seconds))
    return _quantise(np.array(beats))


def _analyse(beats: np.ndarray):
    # Four beats to the bar, so the meter estimate has something to read.
    return derive_musical_time(beats, beats[::4], 0.9, model="test", device="cpu")


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


class TestASectionAtANearbyTempoDoesNotMoveTheReading:
    """The failure a 174 track reads as 173.8 through.

    Beat This! quantises every beat to an integer frame at 50 fps, so the
    intervals arrive on a 20 ms grid. A 16 s intro at 170 inside a 174 track
    shifts each of its intervals by 8 ms - under the quantisation step, and
    far inside the tolerance _robust_intervals allows. No amount of averaging
    or outlier rejection over intervals can separate the two tempos, because
    one beat at a time they are the same. Over the length of the intro they
    walk more than a whole beat apart, which is the only place the difference
    exists to be found.
    """

    @staticmethod
    def _two_sections(first_bpm, first_seconds, second_bpm, second_seconds):
        time, beats = 0.0, []
        while time < first_seconds:
            beats.append(time)
            time += 60.0 / first_bpm
        while time < first_seconds + second_seconds:
            beats.append(time)
            time += 60.0 / second_bpm
        return _quantise(np.array(beats))

    @pytest.mark.parametrize(
        ("intro_bpm", "intro_seconds"),
        [(170.0, 16.0), (170.0, 24.0), (172.0, 16.0), (178.0, 12.0)],
    )
    def test_an_intro_at_another_tempo_is_not_averaged_in(
        self, intro_bpm, intro_seconds
    ):
        beats = self._two_sections(intro_bpm, intro_seconds, 174.0, 240.0 - intro_seconds)
        analysis = _analyse(beats)

        assert analysis.detected_bpm == pytest.approx(174.0, abs=0.05)

    def test_an_outro_at_another_tempo_is_not_averaged_in(self):
        beats = self._two_sections(174.0, 200.0, 176.0, 40.0)

        assert _analyse(beats).detected_bpm == pytest.approx(174.0, abs=0.05)

    def test_a_track_that_really_is_off_the_round_number_stays_there(self):
        # The other half of the same rule: nothing snaps to a tidy tempo.
        # A grid at 174 would walk 0.28 s away from a 173.8 track by the end
        # of four minutes, which is exactly the misalignment being fixed.
        assert _analyse(_synthetic_beats(173.8)).detected_bpm == pytest.approx(
            173.8, abs=0.05
        )


class TestTheFitReportsWhetherOneTempoHoldsTheTrack:
    def test_a_track_cut_to_a_click_sits_at_the_quantisation_floor(self):
        analysis = _analyse(_synthetic_beats(174.0))

        # 20 ms quantised uniformly has an RMS of 20/sqrt(12) = 5.8 ms, and
        # a grid that explains every beat cannot do better than that.
        assert analysis.grid_rms == pytest.approx(0.0058, abs=0.002)
        assert analysis.grid_ratio == pytest.approx(1.0, abs=0.02)

    def test_a_drifting_track_says_so_rather_than_reporting_a_clean_tempo(self):
        # No host tempo aligns a track whose tempo moves. The reading is
        # still the best constant fit, but the fit quality is what tells the
        # difference between "174" and "roughly 174, and it wanders".
        analysis = _analyse(_synthetic_beats(174.0, drift_bpm=-1.0))

        assert analysis.grid_rms > 0.010
        assert analysis.grid_ratio < 0.80


class TestTheAnchorComesFromTheGridNotOneBeat:
    """A reported downbeat is only ever good to half a frame.

    The offsets below matter: a track whose first downbeat happens to land on
    a frame boundary hides this entirely, because then the raw downbeat is
    exact. Real audio does not start on a 20 ms boundary, and a host placing
    bar 1 on the reported downbeat inherits the whole rounding error - up to
    10 ms, 441 samples at 44.1 kHz.
    """

    @staticmethod
    def _offset_track(offset: float, seconds: float = 240.0) -> np.ndarray:
        """A 174 BPM track whose true downbeat sits between two frames."""
        period = 60.0 / 174.0
        return _quantise(np.arange(0.0, seconds, period) + offset)

    @pytest.mark.parametrize("offset", [0.009, 0.013, 0.017, 0.019])
    def test_the_fitted_anchor_beats_the_reported_downbeat(self, offset):
        beats = self._offset_track(offset)
        analysis = _analyse(beats)

        raw_error = abs(float(beats[0]) - offset)
        fitted_error = abs(analysis.bar_one - offset)

        assert fitted_error < raw_error
        assert fitted_error * 44_100 < 50

    def test_the_anchor_sharpens_as_the_track_gets_longer(self):
        offset = 0.013
        short = abs(_analyse(self._offset_track(offset, 60.0)).bar_one - offset)
        long = abs(_analyse(self._offset_track(offset, 480.0)).bar_one - offset)

        assert long < short
