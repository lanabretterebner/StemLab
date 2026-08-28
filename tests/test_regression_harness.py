"""The regression harness is the gate for the whole engine migration, so it
gets its own tests: every failure mode it is supposed to catch is injected
here deliberately, and a harness that passed a broken stem would be worse than
having no harness at all.
"""

from __future__ import annotations

import math

import numpy as np
import pytest
import soundfile as sf

from stemlab.regression import build_corpus, compare_directories, compare_stem, si_sdr
from stemlab.regression.corpus import CORPUS_STEMS

SAMPLE_RATE = 44100


def _tone(seconds: float = 0.5, frequency: float = 220.0) -> np.ndarray:
    time = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    mono = 0.5 * np.sin(2.0 * np.pi * frequency * time)
    return np.stack((mono, mono * 0.9))


def _write(directory, name: str, audio: np.ndarray, sample_rate: int = SAMPLE_RATE) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    sf.write(str(directory / f"{name}.wav"), np.asarray(audio).T, sample_rate, subtype="FLOAT")


def test_identical_stems_pass_with_perfect_correlation():
    audio = _tone()
    metrics = compare_stem("vocals", audio, audio, SAMPLE_RATE)

    assert metrics.passed
    assert metrics.correlation == pytest.approx(1.0)
    assert math.isinf(metrics.si_sdr_db)
    assert metrics.lag_samples == 0


def test_silent_candidate_is_reported_as_silence_not_as_low_correlation():
    """The failure mode seen in the wild: a mask head wired up wrong emits a
    stem at exactly zero, and 'correlation was 0' would bury the diagnosis."""
    reference = _tone()
    candidate = np.zeros_like(reference)

    metrics = compare_stem("vocals", candidate, reference, SAMPLE_RATE)

    assert not metrics.passed
    assert metrics.candidate_silent
    assert any("silent" in failure for failure in metrics.failures)


def test_nan_and_inf_are_caught_rather_than_poisoning_the_metrics():
    """An f16 accumulator overflow shows up as NaN in a handful of frames; the
    metrics must stay finite so the report is still readable."""
    reference = _tone()

    poisoned = reference.copy()
    poisoned[0, 100:110] = np.nan
    metrics = compare_stem("drums", poisoned, reference, SAMPLE_RATE)
    assert metrics.has_nan
    assert not metrics.passed
    assert math.isfinite(metrics.correlation)

    infinite = reference.copy()
    infinite[1, 50] = np.inf
    metrics = compare_stem("drums", infinite, reference, SAMPLE_RATE)
    assert metrics.has_inf
    assert not metrics.passed


def test_si_sdr_is_scale_invariant_but_correlation_still_tracks_shape():
    """A different output gain is not a separation error, and must not score
    as one - otherwise every runtime with its own normalisation looks broken."""
    reference = _tone()
    quieter = reference * 0.25

    assert si_sdr(quieter, reference) > 100.0
    assert compare_stem("bass", quieter, reference, SAMPLE_RATE).correlation == pytest.approx(1.0)


def test_a_silent_estimate_scores_worst_rather_than_perfect():
    """A silent estimate leaves both the projection and the residual at zero.
    Read carelessly that is 0/0; it must come out as the worst score, not the
    best, or a dead stem would look flawless in the quality table."""
    reference = _tone()

    assert si_sdr(np.zeros_like(reference), reference) == -math.inf
    # And the symmetric case still holds: silence does match silence.
    assert si_sdr(np.zeros_like(reference), np.zeros_like(reference)) == math.inf


def test_a_silent_stem_is_not_also_accused_of_a_timing_offset():
    """Silence has no alignment, so the lag check must stay quiet and let the
    silence failure stand on its own."""
    reference = _tone()
    metrics = compare_stem("vocals", np.zeros_like(reference), reference, SAMPLE_RATE)

    assert metrics.lag_samples == 0
    assert not any("centring" in failure for failure in metrics.failures)


def test_phase_inversion_is_caught_by_correlation_even_though_si_sdr_allows_it():
    """SI-SDR is invariant to sign as well as scale, so an inverted stem scores
    perfectly by that measure alone. Correlation is what catches it."""
    reference = _tone()
    inverted = -reference

    assert si_sdr(inverted, reference) == math.inf

    metrics = compare_stem("bass", inverted, reference, SAMPLE_RATE)
    assert metrics.correlation == pytest.approx(-1.0)
    assert not metrics.passed


def test_a_time_shift_is_detected_as_lag():
    """A padding or centring difference in an STFT shows up as a constant
    offset, which is diagnosable - unlike the bad correlation it would cause."""
    reference = _tone()
    shifted = np.roll(reference, 64, axis=1)

    metrics = compare_stem("guitar", shifted, reference, SAMPLE_RATE, max_lag_samples=0)

    assert metrics.lag_samples == 64
    assert not metrics.passed
    assert any("padding or centring" in failure for failure in metrics.failures)


def test_added_noise_drops_correlation_below_the_gate():
    reference = _tone()
    rng = np.random.default_rng(7)
    noisy = reference + 0.2 * rng.standard_normal(reference.shape)

    metrics = compare_stem("other", noisy, reference, SAMPLE_RATE, min_correlation=0.99)

    assert not metrics.passed
    assert metrics.correlation < 0.99


def test_directory_comparison_flags_missing_extra_and_mismatched_rate(tmp_path):
    reference_dir = tmp_path / "reference"
    candidate_dir = tmp_path / "candidate"

    audio = _tone()
    for name in ("vocals", "drums", "bass"):
        _write(reference_dir, name, audio)

    _write(candidate_dir, "vocals", audio)
    # "drums" is absent entirely; "bass" comes back at the wrong rate; and the
    # candidate adds a residual stem the reference never had.
    _write(candidate_dir, "bass", audio, sample_rate=48000)
    _write(candidate_dir, "instrumental", audio)

    report = compare_directories(reference_dir, candidate_dir)

    assert not report.passed
    assert report.missing == ["drums"]
    assert report.unexpected == ["instrumental"]
    assert len(report.sample_rate_mismatches) == 1
    assert "bass" in report.sample_rate_mismatches[0]


def test_identical_directories_pass(tmp_path):
    reference_dir = tmp_path / "reference"
    candidate_dir = tmp_path / "candidate"

    for name in ("vocals", "drums"):
        audio = _tone(frequency=220.0 if name == "vocals" else 110.0)
        _write(reference_dir, name, audio)
        _write(candidate_dir, name, audio)

    report = compare_directories(reference_dir, candidate_dir)

    assert report.passed
    assert len(report.stems) == 2


def test_truth_scoring_separates_different_from_worse(tmp_path):
    """Agreement alone cannot tell a regression from an improvement; scoring
    both against ground truth can, which is why --truth exists."""
    truth_dir = tmp_path / "truth"
    reference_dir = tmp_path / "reference"
    candidate_dir = tmp_path / "candidate"

    truth = _tone()
    rng = np.random.default_rng(3)

    _write(truth_dir, "vocals", truth)
    _write(reference_dir, "vocals", truth + 0.05 * rng.standard_normal(truth.shape))
    _write(candidate_dir, "vocals", truth + 0.01 * rng.standard_normal(truth.shape))

    report = compare_directories(
        reference_dir, candidate_dir, truth_dir=truth_dir, min_correlation=0.0, min_si_sdr_db=-100.0
    )

    scores = report.truth_si_sdr["vocals"]
    assert scores["candidate"] > scores["reference"]


def test_corpus_is_deterministic_and_its_stems_sum_to_the_mixture(tmp_path):
    """Metrics stored today must stay comparable next month, and separation
    metrics are only meaningful if the stems really do sum to the mix."""
    first = build_corpus(tmp_path / "a", duration_seconds=4.0)
    second = build_corpus(tmp_path / "b", duration_seconds=4.0)

    mix_a, rate = sf.read(str(first.mixture), always_2d=True)
    mix_b, _ = sf.read(str(second.mixture), always_2d=True)

    assert rate == 44100
    np.testing.assert_array_equal(mix_a, mix_b)
    assert not np.isnan(mix_a).any()
    assert np.abs(mix_a).max() <= 1.0

    summed = np.zeros_like(mix_a)
    for name in CORPUS_STEMS:
        stem, _ = sf.read(str(first.stems[name]), always_2d=True)
        summed += stem

    np.testing.assert_allclose(summed, mix_a, atol=1e-6)


def test_corpus_stems_are_audible_and_distinct(tmp_path):
    """A corpus whose stems were silent or identical would let a broken
    separator pass, so assert the material itself is real."""
    corpus = build_corpus(tmp_path / "corpus", duration_seconds=4.0)

    rendered = {}
    for name in CORPUS_STEMS:
        audio, _ = sf.read(str(corpus.stems[name]), always_2d=True)
        assert np.abs(audio).max() > 1e-3, f"{name} is effectively silent"
        rendered[name] = audio[:, 0]

    for name, audio in rendered.items():
        for other_name, other in rendered.items():
            if name >= other_name:
                continue
            correlation = float(np.corrcoef(audio, other)[0, 1])
            assert abs(correlation) < 0.9, f"{name} and {other_name} are near-duplicates"
