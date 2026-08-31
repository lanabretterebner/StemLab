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


class TestAQuietStartOrEndingDoesNotMoveTheReading:
    """Where a detector is least sure, and what that used to cost.

    A track that fades in, or ends in a reverb tail, gives the model very
    little to lock onto at its edges. It responds by placing beats loosely,
    or by finding beats in what is effectively silence - and those land at
    whatever spacing the noise suggests, which is near enough the real tempo
    to pass every interval-based filter and wrong enough to drag the average
    down. Measured against the old estimator, on a 240 s track at 174:

      20 s of beats in silence before the track      173.61
      20 s of beats in the reverb tail after it      173.62
      30 s of tail beats running 3% slow             173.44
      both ends at once                              173.29
      40 s intro with the beats jittered 3 frames    174.31

    A grid does not have to reject these by recognising them. They fail to
    sit on it, and beats that fail to sit on the grid are not what the tempo
    is read from.
    """

    BPM = 174.0
    SECONDS = 240.0

    @classmethod
    def _grid(cls) -> np.ndarray:
        return np.arange(0.0, cls.SECONDS, 60.0 / cls.BPM)

    @classmethod
    def _replace(cls, window, transform):
        """Mangle the beats inside ``window`` and keep the rest true."""
        beats = cls._grid()
        inside = (beats >= window[0]) & (beats < window[1])
        return _quantise(
            np.concatenate([transform(beats[inside]), beats[~inside]])
        )

    @staticmethod
    def _noise(scale_frames):
        rng = np.random.default_rng(5)
        return lambda beats: beats + rng.normal(0.0, scale_frames / FPS, beats.size)

    @pytest.mark.parametrize("seconds", [20.0, 40.0, 60.0])
    def test_a_loosely_tracked_intro_is_not_averaged_in(self, seconds):
        beats = self._replace((0.0, seconds), self._noise(3.0))

        assert _analyse(beats).detected_bpm == pytest.approx(self.BPM, abs=0.05)

    @pytest.mark.parametrize("seconds", [20.0, 40.0])
    def test_a_loosely_tracked_ending_is_not_averaged_in(self, seconds):
        beats = self._replace((self.SECONDS - seconds, self.SECONDS), self._noise(3.0))

        assert _analyse(beats).detected_bpm == pytest.approx(self.BPM, abs=0.05)

    def test_beats_found_in_the_silence_before_the_track_are_not_counted(self):
        # Whatever the model hears in a fade-in need not be at the tempo, and
        # here it is 3% slow - close enough to survive any interval filter.
        period = 60.0 / self.BPM
        beats = _quantise(
            np.concatenate([np.arange(-20.0, 0.0, period * 1.03), self._grid()])
        )

        assert _analyse(beats).detected_bpm == pytest.approx(self.BPM, abs=0.05)

    def test_beats_found_in_the_reverb_tail_are_not_counted(self):
        period = 60.0 / self.BPM
        beats = _quantise(
            np.concatenate(
                [self._grid(), np.arange(self.SECONDS, self.SECONDS + 30.0, period * 1.03)]
            )
        )

        assert _analyse(beats).detected_bpm == pytest.approx(self.BPM, abs=0.05)

    def test_both_ends_at_once(self):
        period = 60.0 / self.BPM
        beats = _quantise(
            np.concatenate(
                [
                    np.arange(-20.0, 0.0, period * 1.03),
                    self._grid(),
                    np.arange(self.SECONDS, self.SECONDS + 20.0, period * 1.03),
                ]
            )
        )
        analysis = _analyse(beats)

        assert analysis.detected_bpm == pytest.approx(self.BPM, abs=0.05)
        # And the fit says so: the edges are the beats it could not explain.
        assert analysis.grid_ratio < 0.95

    @pytest.mark.parametrize(
        "transform",
        [
            lambda beats: beats[::2],                       # intro heard at half time
            lambda beats: beats[: beats.size // 2],         # intro only half tracked
            lambda beats: beats + (60.0 / 174.0) / 4.0,     # intro locked off-phase
        ],
    )
    def test_an_intro_the_model_hears_differently_is_still_harmless(self, transform):
        beats = self._replace((0.0, 20.0), transform)

        assert _analyse(beats).detected_bpm == pytest.approx(self.BPM, abs=0.05)


class TestBeatsComeOffTheFrameGrid:
    """50 fps is the model's own rate; the logits behind it are continuous.

    Beat This! reports an integer frame divided by 50, so every beat it emits
    carries up to 10 ms - 441 samples at 44.1 kHz - of rounding. That rate is
    the spectrogram hop the network was trained on and cannot be raised
    without retraining it. What can be recovered is where the peak in its
    output actually sits: the network emits a logit per frame, and a beat
    between two frames leaves a peak centred between them.

    These drive the interpolation with logits whose true peak position is
    known, which is the part that can be checked without the model. Whether
    the network's peak tracks the true onset sub-frame is a property of the
    model, and is not asserted here.
    """

    @staticmethod
    def _bump(centre: float, frames: int = 200, width: float = 1.1) -> np.ndarray:
        """Log-odds of a Gaussian beat probability centred at ``centre``."""
        index = np.arange(frames, dtype=np.float64)
        # Gaussian in probability is exactly a parabola in log-odds, which is
        # the shape the three-point fit assumes.
        return -(((index - centre) / width) ** 2)

    @pytest.mark.parametrize("offset", [-0.45, -0.3, -0.1, 0.0, 0.1, 0.3, 0.45])
    def test_a_peak_between_frames_is_reported_between_frames(self, offset):
        from stemlab.beat_tracking import _sub_frame_events

        centre = 100.0 + offset
        logits = self._bump(centre)
        reported = _sub_frame_events(np.array([round(centre) / FPS]), logits)

        assert reported[0] == pytest.approx(centre / FPS, abs=1e-6)

    def test_the_rounding_it_removes_is_worth_hundreds_of_samples(self):
        from stemlab.beat_tracking import _sub_frame_events

        centre = 100.4
        logits = self._bump(centre)
        rounded = round(centre) / FPS
        refined = float(_sub_frame_events(np.array([rounded]), logits)[0])

        assert abs(rounded - centre / FPS) * 44_100 > 300
        assert abs(refined - centre / FPS) * 44_100 < 1

    def test_a_frame_that_is_not_a_peak_is_left_alone(self):
        # Nothing to interpolate on a flat or rising run, and inventing an
        # offset there would move a beat for no reason.
        from stemlab.beat_tracking import _sub_frame_events

        flat = np.zeros(50)
        assert _sub_frame_events(np.array([20 / FPS]), flat)[0] == pytest.approx(
            20 / FPS
        )

    def test_events_on_the_first_and_last_frame_survive(self):
        from stemlab.beat_tracking import _sub_frame_events

        logits = np.zeros(10)
        events = np.array([0.0, 9 / FPS])

        assert _sub_frame_events(events, logits) == pytest.approx(events)

    def test_no_events_and_no_logits_are_not_an_error(self):
        from stemlab.beat_tracking import _sub_frame_events

        assert _sub_frame_events(np.array([]), np.zeros(10)).size == 0
        assert _sub_frame_events(np.array([0.0]), np.zeros(1)).size == 1
