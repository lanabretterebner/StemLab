"""Compare two sets of separated stems and gate on the difference.

The migration's central question is whether a new engine produces the same
audio as the one it replaces, so this compares a candidate stem directory
against a reference one and fails loudly when it does not.

When ground-truth stems are available - the synthetic corpus, or a labelled
test set - pass --truth as well. Agreement alone cannot tell "the candidate is
wrong" from "the candidate is different and better", and separation quality
measured against the truth can.

    python -m stemlab.regression.compare --reference ref/ --candidate cand/
    python -m stemlab.regression.compare --reference ref/ --candidate cand/ \\
        --truth corpus/stems --json report.json
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf

from .metrics import StemMetrics, compare_stem, si_sdr

AUDIO_SUFFIXES = (".wav", ".flac", ".aiff", ".aif", ".ogg")


def _format_db(value: float) -> str:
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return f"{value:.2f}"


def discover_stems(directory: Path) -> dict[str, Path]:
    """Map stem name to file for every audio file directly in a directory."""
    if not directory.is_dir():
        raise FileNotFoundError(f"Not a directory: {directory}")

    found: dict[str, Path] = {}

    for path in sorted(directory.iterdir()):
        if path.is_file() and path.suffix.lower() in AUDIO_SUFFIXES:
            found[path.stem.lower()] = path

    return found


def _read(path: Path) -> tuple[np.ndarray, int]:
    audio, sample_rate = sf.read(str(path), always_2d=True, dtype="float64")
    return audio.T, int(sample_rate)


@dataclass
class ComparisonReport:
    """The whole comparison, in the shape the JSON report is written from."""

    stems: list[StemMetrics] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)
    unexpected: list[str] = field(default_factory=list)
    sample_rate_mismatches: list[str] = field(default_factory=list)
    truth_si_sdr: dict[str, dict[str, float]] = field(default_factory=dict)

    @property
    def passed(self) -> bool:
        return (
            not self.missing
            and not self.sample_rate_mismatches
            and all(stem.passed for stem in self.stems)
        )

    def to_dict(self) -> dict:
        return {
            "passed": self.passed,
            "stems": [stem.to_dict() for stem in self.stems],
            "missing": self.missing,
            "unexpected": self.unexpected,
            "sample_rate_mismatches": self.sample_rate_mismatches,
            "truth_si_sdr_db": self.truth_si_sdr,
        }


def compare_directories(
    reference_dir: Path,
    candidate_dir: Path,
    truth_dir: Path | None = None,
    min_correlation: float = 0.99,
    min_si_sdr_db: float = 20.0,
    max_lag_samples: int = 0,
) -> ComparisonReport:
    """Score every reference stem against its candidate counterpart."""
    reference_stems = discover_stems(reference_dir)
    candidate_stems = discover_stems(candidate_dir)
    truth_stems = discover_stems(truth_dir) if truth_dir else {}

    report = ComparisonReport()

    # Extra candidate stems are reported, not failed: the Python pipeline emits
    # a residual "instrumental" alongside the six, and a runtime that does not
    # is a difference worth seeing rather than an error.
    report.unexpected = sorted(set(candidate_stems) - set(reference_stems))

    for name, reference_path in reference_stems.items():
        candidate_path = candidate_stems.get(name)

        if candidate_path is None:
            report.missing.append(name)
            continue

        reference, reference_rate = _read(reference_path)
        candidate, candidate_rate = _read(candidate_path)

        if reference_rate != candidate_rate:
            # Comparing across rates would silently score a resampling bug as a
            # quality difference, which is precisely the mistake to avoid.
            report.sample_rate_mismatches.append(
                f"{name}: reference {reference_rate} Hz, candidate {candidate_rate} Hz"
            )
            continue

        report.stems.append(
            compare_stem(
                stem=name,
                candidate=candidate,
                reference=reference,
                sample_rate=reference_rate,
                min_correlation=min_correlation,
                min_si_sdr_db=min_si_sdr_db,
                max_lag_samples=max_lag_samples,
            )
        )

        truth_path = truth_stems.get(name)

        if truth_path is not None:
            truth, _ = _read(truth_path)
            # Score the finite part, as compare_stem does: a handful of NaN
            # samples would otherwise turn the whole quality figure into nan
            # and hide how good the rest of the stem was.
            finite_candidate = np.nan_to_num(candidate, nan=0.0, posinf=0.0, neginf=0.0)
            report.truth_si_sdr[name] = {
                "reference": si_sdr(reference, truth),
                "candidate": si_sdr(finite_candidate, truth),
            }

    return report


def render(report: ComparisonReport) -> str:
    """A table meant to be read in a terminal or a CI log."""
    lines = []
    header = f"{'stem':<12} {'corr':>10} {'SI-SDR dB':>10} {'LSD dB':>8} {'lag':>7} {'peak':>9}"
    lines.append(header)
    lines.append("-" * len(header))

    for stem in report.stems:
        flag = "" if stem.passed else "  FAIL"
        lines.append(
            f"{stem.stem:<12} {stem.correlation:>10.6f} {_format_db(stem.si_sdr_db):>10} "
            f"{stem.log_spectral_distance_db:>8.2f} {stem.lag_samples:>7d} "
            f"{stem.peak_candidate:>9.3e}{flag}"
        )

    for stem in report.stems:
        for failure in stem.failures:
            lines.append(f"  FAIL {stem.stem}: {failure}")

    for name in report.missing:
        lines.append(f"  FAIL {name}: present in the reference, absent from the candidate")

    for mismatch in report.sample_rate_mismatches:
        lines.append(f"  FAIL sample rate {mismatch}")

    for name in report.unexpected:
        lines.append(f"  note {name}: candidate emitted a stem the reference did not")

    if report.truth_si_sdr:
        lines.append("")
        lines.append("Separation quality against ground truth (higher is better):")
        lines.append(f"{'stem':<12} {'reference':>12} {'candidate':>12} {'delta':>10}")

        for name, scores in report.truth_si_sdr.items():
            reference_db = scores["reference"]
            candidate_db = scores["candidate"]

            # inf minus inf is nan, which reads as a bug rather than as "both
            # were exact". Only subtract when both numbers are real.
            if math.isfinite(reference_db) and math.isfinite(candidate_db):
                delta = _format_db(candidate_db - reference_db)
            else:
                delta = "n/a"

            lines.append(
                f"{name:<12} {_format_db(reference_db):>12} "
                f"{_format_db(candidate_db):>12} {delta:>10}"
            )

    lines.append("")
    lines.append("RESULT: PASS" if report.passed else "RESULT: FAIL")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="stemlab.regression.compare",
        description="Compare candidate stems against reference stems.",
    )
    parser.add_argument("--reference", required=True, type=Path, help="known-good stem directory")
    parser.add_argument("--candidate", required=True, type=Path, help="stem directory under test")
    parser.add_argument("--truth", type=Path, help="ground-truth stems, when they exist")
    parser.add_argument("--json", type=Path, help="write the full report here")
    parser.add_argument("--min-correlation", type=float, default=0.99)
    parser.add_argument("--min-si-sdr", type=float, default=20.0)
    parser.add_argument(
        "--max-lag",
        type=int,
        default=0,
        help="tolerated alignment offset in samples; non-zero usually means an STFT difference",
    )
    args = parser.parse_args(argv)

    report = compare_directories(
        reference_dir=args.reference,
        candidate_dir=args.candidate,
        truth_dir=args.truth,
        min_correlation=args.min_correlation,
        min_si_sdr_db=args.min_si_sdr,
        max_lag_samples=args.max_lag,
    )

    print(render(report))

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report.to_dict(), indent=2), encoding="utf-8")

    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
