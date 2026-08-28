"""Audio regression tooling: does a new engine still produce the same stems?

The migration off PyTorch is gated on measurement rather than inspection, and
this is the measurement. `metrics` scores one stem against another, `compare`
scores whole directories and applies the gates, and `corpus` synthesises
deterministic test material so CI has something to separate.
"""

from __future__ import annotations

from .compare import ComparisonReport, compare_directories, discover_stems, render
from .corpus import CORPUS_STEMS, Corpus, build_corpus
from .metrics import StemMetrics, compare_stem, correlation, log_spectral_distance, si_sdr

__all__ = [
    "CORPUS_STEMS",
    "ComparisonReport",
    "Corpus",
    "StemMetrics",
    "build_corpus",
    "compare_directories",
    "compare_stem",
    "correlation",
    "discover_stems",
    "log_spectral_distance",
    "render",
    "si_sdr",
]
