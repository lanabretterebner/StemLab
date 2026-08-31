"""Offline tempo, beat, downbeat, meter, and musical-key analysis.

CLI modes:
  - Analysis:            --input SRC --output OUT [--mode ...] [stem flags]
  - Cache maintenance:   --clear-cache
  - Correction only:     --input SRC --set-correction [--correct-* ...]
                         --input SRC --forget-correction
  - Combined:            --input SRC --set-correction [--correct-* ...] --output OUT
                         (also --forget-correction with --output) applies the
                         correction to the local store first, then runs the
                         cache-aware analysis and writes OUT in one process.
"""

from __future__ import annotations

import argparse
import json
import os
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np

from .analysis_cache import AnalysisCache, source_identity
# beat_tracking defers its torch import into the inference paths, so this
# import stays light; analyse_beats only loads torch when actually called on
# a beat-cache miss.
from .beat_tracking import BEAT_ALGORITHM_VERSION, MODEL_SPECS, BeatAnalysis, analyse_beats
from .runtime import CancellationToken, JobCancelled, configure_utf8_stdio

KEY_ALGORITHM_VERSION = "fistem-tonal-ensemble-3"
# Version tag for cached per-signal tonal evidence values; bump when the
# stored evidence layout or the scoring that produces it changes shape.
_KEY_EVIDENCE_SCHEMA = 1
_ANALYSIS_SAMPLE_RATE = 22_050
_WINDOW_SECONDS = 12.0
_PITCH_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# Two established tonal-profile families are kept separate through scoring,
# then averaged across robust chroma representations and time windows.
_KEY_PROFILES = (
    (
        np.asarray([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]),
        np.asarray([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]),
    ),
    (
        np.asarray([5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0]),
        np.asarray([5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0]),
    ),
)


# Chord-root evidence helps distinguish a true tonic from a heavily emphasised
# dominant.  The original profile-only detector could mistake songs such as a
# G-minor progression with a strong D/D-major dominant for D minor.  We detect
# stable major/minor triads inside each analysis window and score how naturally
# those chords belong to each of the 24 candidate keys.
_TRIAD_TEMPLATES = np.zeros((24, 12), dtype=np.float64)
for _mode_offset, _intervals in ((0, (0, 4, 7)), (12, (0, 3, 7))):
    for _root in range(12):
        _template = np.zeros(12, dtype=np.float64)
        for _interval in _intervals:
            _template[(_root + _interval) % 12] = 1.0
        _template[_root] = 1.35
        _TRIAD_TEMPLATES[_mode_offset + _root] = _template / np.linalg.norm(_template)

_KEY_CHORD_COMPATIBILITY = np.full((24, 24), -0.18, dtype=np.float64)
for _tonic in range(12):
    # Major: I, ii, iii, IV, V, vi.  Tonic and dominant receive extra weight
    # because they carry the strongest tonal-centre information.
    for _interval, _minor, _weight in (
        (0, False, 1.80),
        (2, True, 0.65),
        (4, True, 0.55),
        (5, False, 0.90),
        (7, False, 1.20),
        (9, True, 0.70),
    ):
        _chord = (12 if _minor else 0) + ((_tonic + _interval) % 12)
        _KEY_CHORD_COMPATIBILITY[_tonic, _chord] = _weight

    # Minor: i, III, iv, v/V, VI, VII.  Both minor-v and the common harmonic
    # minor major-V are accepted; the latter is especially useful for rock/pop.
    for _interval, _minor, _weight in (
        (0, True, 1.80),
        (3, False, 0.85),
        (5, True, 0.90),
        (7, True, 0.45),
        (7, False, 1.25),
        (8, False, 0.85),
        (10, False, 0.75),
    ):
        _chord = (12 if _minor else 0) + ((_tonic + _interval) % 12)
        _row = 12 + _tonic
        _KEY_CHORD_COMPATIBILITY[_row, _chord] = max(
            _KEY_CHORD_COMPATIBILITY[_row, _chord], _weight
        )


@dataclass(frozen=True)
class KeyCandidate:
    key: str
    probability: float


@dataclass(frozen=True)
class KeyAnalysis:
    key: str | None
    confidence: float
    uncertain: bool
    candidates: tuple[KeyCandidate, ...]
    tuning_cents: float

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


@dataclass(frozen=True)
class SourceAnalysis:
    """Complete local analysis contract consumed by JUCE and MIDI export."""

    key: str | None
    bpm: float | None
    key_confidence: float
    bpm_confidence: float
    key_candidates: tuple[KeyCandidate, ...]
    detected_bpm: float | None
    half_time_bpm: float | None
    double_time_bpm: float | None
    beats: tuple[float, ...]
    downbeats: tuple[float, ...]
    meter_numerator: int | None
    meter_denominator: int
    bar_one: float
    # False when the beats do not sit on one constant grid - a played or
    # drifting track. The tempo is still the best single answer, but setting
    # a host to it will not hold alignment across the whole track.
    tempo_is_steady: bool
    # Each stretch one constant tempo explains, in order. One entry is a
    # track that holds a single tempo; more than one names where each holds.
    tempo_segments: tuple[dict[str, float], ...]
    source_hash: str
    analysis_mode: str
    beat_model: str
    beat_model_version: str
    device: str
    tuning_cents: float
    corrected: bool


def _profile_correlation(chroma: np.ndarray, profile: np.ndarray, tonic: int) -> float:
    candidate = np.roll(profile, tonic)
    if float(np.std(chroma)) < 1.0e-8:
        return 0.0
    value = float(np.corrcoef(chroma, candidate)[0, 1])
    return value if np.isfinite(value) else 0.0


def _candidate_names() -> tuple[str, ...]:
    return tuple(
        f"{_PITCH_NAMES[tonic]} {mode}" for mode in ("major", "minor") for tonic in range(12)
    )


_CANDIDATE_NAMES = _candidate_names()


def _profile_scores(chroma: np.ndarray) -> np.ndarray:
    scores = np.zeros(24, dtype=np.float64)
    for major_profile, minor_profile in _KEY_PROFILES:
        for tonic in range(12):
            scores[tonic] += _profile_correlation(chroma, major_profile, tonic)
            scores[12 + tonic] += _profile_correlation(chroma, minor_profile, tonic)
    return scores / len(_KEY_PROFILES)




def _chord_key_scores(chromagram: np.ndarray) -> np.ndarray:
    """Return key-support scores from stable frame-level major/minor triads."""
    chromagram = np.asarray(chromagram, dtype=np.float64)
    if chromagram.ndim != 2 or chromagram.shape[0] != 12 or chromagram.shape[1] == 0:
        return np.zeros(24, dtype=np.float64)

    frame_norms = np.linalg.norm(chromagram, axis=0)
    valid = frame_norms > 1.0e-7
    if not np.any(valid):
        return np.zeros(24, dtype=np.float64)

    normalised = chromagram[:, valid] / frame_norms[valid][None, :]
    similarities = _TRIAD_TEMPLATES @ normalised
    best = np.argmax(similarities, axis=0)
    strength = np.max(similarities, axis=0)

    # Ambiguous/noisy frames should contribute little.  Clean triads usually
    # land around 0.75-1.0 cosine similarity with these sparse templates.
    weights = np.clip((strength - 0.48) / 0.35, 0.0, 1.0) ** 1.5
    evidence = np.bincount(best, weights=weights, minlength=24).astype(np.float64)
    total = float(np.sum(evidence))
    if total <= 1.0e-9:
        return np.zeros(24, dtype=np.float64)

    evidence /= total
    return _KEY_CHORD_COMPATIBILITY @ evidence

def _softmax(values: np.ndarray, scale: float = 5.0) -> np.ndarray:
    shifted = scale * (values - float(np.max(values)))
    probabilities = np.exp(np.clip(shifted, -60.0, 0.0))
    return probabilities / max(float(np.sum(probabilities)), 1.0e-12)


def _window_starts(length: int, sample_rate: int) -> list[int]:
    window = max(2048, int(_WINDOW_SECONDS * sample_rate))
    if length <= window:
        return [0]
    count = min(20, max(4, int(np.ceil(length / window))))
    return [int(value) for value in np.linspace(0, length - window, count)]


def _tonal_evidence(
    audio: np.ndarray,
    sample_rate: int,
    *,
    bass_role: bool,
    cancellation: CancellationToken,
) -> tuple[np.ndarray, float, float]:
    import librosa

    audio = np.asarray(audio, dtype=np.float32)
    if audio.size < 2048 or float(np.sqrt(np.mean(np.square(audio)))) < 1.0e-5:
        return np.zeros(24), 0.0, 0.0

    harmonic = librosa.effects.harmonic(audio, margin=3.0)
    cancellation.raise_if_cancelled()
    tuning = float(librosa.estimate_tuning(y=harmonic, sr=sample_rate))
    window_size = max(2048, int(_WINDOW_SECONDS * sample_rate))
    starts = _window_starts(harmonic.size, sample_rate)
    window_rows: list[tuple[np.ndarray, float]] = []

    for start in starts:
        cancellation.raise_if_cancelled()
        original_window = audio[start : start + window_size]
        harmonic_window = harmonic[start : start + window_size]
        if harmonic_window.size < 2048:
            continue

        rms = float(np.sqrt(np.mean(np.square(original_window))))
        harmonic_rms = float(np.sqrt(np.mean(np.square(harmonic_window))))
        if rms < 1.0e-5 or harmonic_rms < 1.0e-6:
            continue

        flatness = float(np.median(librosa.feature.spectral_flatness(y=harmonic_window)))
        if flatness > 0.22:
            continue

        # chroma_cqt and chroma_cens both reduce the same |CQT| internally
        # (fmin C1, 7 octaves, 36 bins/octave); computing it once and passing
        # C keeps their outputs bit-identical while halving the transforms.
        constant_q = np.abs(
            librosa.cqt(
                harmonic_window,
                sr=sample_rate,
                hop_length=2048,
                fmin=None,
                n_bins=7 * 36,
                bins_per_octave=36,
                tuning=tuning,
            )
        )
        chroma_cqt = librosa.feature.chroma_cqt(
            C=constant_q,
            sr=sample_rate,
            hop_length=2048,
            tuning=tuning,
        )
        chroma_cens = librosa.feature.chroma_cens(
            C=constant_q,
            sr=sample_rate,
            hop_length=2048,
            tuning=tuning,
        )
        summaries = []
        for chromagram in (chroma_cqt, chroma_cens):
            summary = np.median(chromagram, axis=1).astype(np.float64)
            summary /= float(np.linalg.norm(summary) + 1.0e-12)
            summaries.append(summary)

        chroma = np.mean(summaries, axis=0)
        profile_scores = _profile_scores(chroma)
        chord_scores = _chord_key_scores(chroma_cqt)
        # Chord/root evidence gets the larger share because it distinguishes
        # tonic from dominant more reliably than whole-song pitch-class counts.
        # Profile correlation still carries mode/scale evidence when chords are
        # sparse or highly ornamented.
        key_scores = 0.40 * profile_scores + 0.60 * chord_scores
        frame_norms = np.linalg.norm(chroma_cqt, axis=0)
        normalized_frames = chroma_cqt / np.maximum(frame_norms, 1.0e-9)
        stability = 1.0 - float(
            np.clip(np.median(np.linalg.norm(normalized_frames - chroma[:, None], axis=0)), 0, 1)
        )
        harmonic_ratio = float(np.clip(harmonic_rms / max(rms, 1.0e-9), 0.0, 1.0))
        concentration_ratio = float(np.max(chroma) / (np.mean(chroma) + 1.0e-9))
        if concentration_ratio < 1.25:
            continue
        concentration = float(np.clip(concentration_ratio / 5.0, 0, 1))
        weight = rms * (0.25 + 0.75 * stability) * (0.25 + 0.75 * harmonic_ratio)
        weight *= (0.45 + 0.55 * concentration) * (1.0 - flatness / 0.22)
        window_rows.append((key_scores, weight))

    if not window_rows:
        return np.zeros(24), 0.0, tuning * 100.0

    weights = np.asarray([row[1] for row in window_rows], dtype=np.float64)
    median_weight = float(np.median(weights[weights > 0])) if np.any(weights > 0) else 1.0
    weights = np.clip(weights / max(median_weight, 1.0e-9), 0.15, 3.0)
    scores = np.average(np.stack([row[0] for row in window_rows]), axis=0, weights=weights)

    if bass_role:
        # Bass evidence is useful for tonic resolution, but must not overwhelm
        # mode evidence from the full harmonic mixture.
        bass_chroma = librosa.feature.chroma_cqt(
            y=harmonic,
            sr=sample_rate,
            hop_length=2048,
            fmin=librosa.note_to_hz("C1"),
            n_octaves=4,
            tuning=tuning,
        )
        tonic = np.median(bass_chroma, axis=1).astype(np.float64)
        tonic /= float(np.max(tonic) + 1.0e-12)
        scores[:12] += 0.18 * tonic
        scores[12:] += 0.18 * tonic

    return scores, float(np.sum(weights)), tuning * 100.0


@dataclass(frozen=True)
class _KeySource:
    """One tonal-evidence input: how to load it, weigh it, and cache it."""

    load: Callable[[], tuple[np.ndarray, int]]
    scale: float
    divisor: float
    bass_role: bool
    signal_hash: str | None = None


def _evidence_settings(bass_role: bool) -> dict[str, Any]:
    # Every per-call knob that shapes one signal's _tonal_evidence output.
    # The profile/chord split mirrors the constants inside _tonal_evidence;
    # code-level scoring changes are covered by KEY_ALGORITHM_VERSION.
    return {
        "window_seconds": _WINDOW_SECONDS,
        "bass_role": bass_role,
        "sample_rate": _ANALYSIS_SAMPLE_RATE,
        "profile_weight": 0.40,
        "chord_weight": 0.60,
    }


def _cached_tonal_evidence(
    source: _KeySource,
    *,
    cache: AnalysisCache | None,
    cancellation: CancellationToken,
) -> tuple[np.ndarray, float, float]:
    """Fetch or compute one signal's evidence; mix evidence is stem-independent."""
    settings = _evidence_settings(source.bass_role)
    cacheable = cache is not None and source.signal_hash is not None
    if cacheable:
        stored = cache.get_result(
            "key_evidence", source.signal_hash, KEY_ALGORITHM_VERSION, settings
        )
        if (
            stored is not None
            and stored.get("schema") == _KEY_EVIDENCE_SCHEMA
            and len(stored.get("scores", ())) == 24
        ):
            return (
                np.asarray(stored["scores"], dtype=np.float64),
                float(stored["weight"]),
                float(stored["tuning_cents"]),
            )

    audio, sample_rate = source.load()
    scores, weight, tuning_cents = _tonal_evidence(
        audio, sample_rate, bass_role=source.bass_role, cancellation=cancellation
    )
    if cacheable:
        cache.put_result(
            "key_evidence",
            source.signal_hash,
            KEY_ALGORITHM_VERSION,
            settings,
            {
                "schema": _KEY_EVIDENCE_SCHEMA,
                "scores": [float(value) for value in np.asarray(scores, dtype=np.float64)],
                "weight": float(weight),
                "tuning_cents": float(tuning_cents),
            },
        )
    return scores, weight, tuning_cents


def _analyse_key_sources(
    sources: Sequence[_KeySource],
    *,
    cache: AnalysisCache | None,
    cancellation: CancellationToken,
) -> KeyAnalysis:
    """Rank all 24 keys from per-signal evidence rows, cached or fresh."""
    rows: list[tuple[np.ndarray, float]] = []
    tunings: list[tuple[float, float]] = []
    for source in sources:
        scores, weight, tuning = _cached_tonal_evidence(
            source, cache=cache, cancellation=cancellation
        )
        if weight > 0:
            rows.append((scores, weight * source.scale / source.divisor))
            tunings.append((tuning, weight))

    if not rows:
        return KeyAnalysis(None, 0.0, True, (), 0.0)

    combined = np.average(
        np.stack([row[0] for row in rows]),
        axis=0,
        weights=np.asarray([row[1] for row in rows]),
    )
    probabilities = _softmax(combined)
    order = np.argsort(probabilities)[::-1]
    candidates = tuple(
        KeyCandidate(_CANDIDATE_NAMES[int(index)], round(float(probabilities[index]), 6))
        for index in order
    )
    top = candidates[0]
    runner_up = candidates[1]
    margin = top.probability - runner_up.probability
    uncertain = top.probability < 0.085 or margin < 0.018
    tuning_cents = float(
        np.average(
            np.asarray([row[0] for row in tunings]),
            weights=np.asarray([row[1] for row in tunings]),
        )
    )
    return KeyAnalysis(
        None if uncertain else top.key,
        top.probability,
        uncertain,
        candidates,
        round(tuning_cents, 3),
    )


def analyse_key(
    audio: np.ndarray,
    sample_rate: int,
    *,
    harmony_signals: tuple[tuple[np.ndarray, int], ...] = (),
    bass_signal: tuple[np.ndarray, int] | None = None,
    cancellation: CancellationToken | None = None,
) -> KeyAnalysis:
    """Rank all 24 keys from multiple stable harmonic windows and profiles."""
    token = cancellation or CancellationToken()
    sources: list[_KeySource] = [_KeySource(lambda: (audio, sample_rate), 0.65, 1.0, False)]
    divisor = float(max(1, len(harmony_signals)))
    for signal, rate in harmony_signals:
        sources.append(
            _KeySource(lambda signal=signal, rate=rate: (signal, rate), 0.25, divisor, False)
        )
    if bass_signal is not None:
        sources.append(_KeySource(lambda: bass_signal, 0.10, 1.0, True))
    return _analyse_key_sources(sources, cache=None, cancellation=token)


def estimate_key(audio: np.ndarray, sample_rate: int) -> tuple[str | None, float]:
    """Compatibility wrapper for callers that only need the leading key."""
    result = analyse_key(audio, sample_rate)
    return result.key, result.confidence


def _load_signal(path: Path) -> tuple[np.ndarray, int]:
    import librosa

    audio, sample_rate = librosa.load(str(path), sr=_ANALYSIS_SAMPLE_RATE, mono=True)
    return np.asarray(audio, dtype=np.float32), int(sample_rate)


def _beat_from_cache(payload: dict[str, Any]) -> BeatAnalysis:
    return BeatAnalysis(
        **{
            **payload,
            "beats": tuple(payload.get("beats", ())),
            "downbeats": tuple(payload.get("downbeats", ())),
        }
    )


def _key_from_cache(payload: dict[str, Any]) -> KeyAnalysis:
    candidates = tuple(KeyCandidate(**item) for item in payload.get("candidates", ()))
    return KeyAnalysis(
        payload.get("key"),
        float(payload.get("confidence", 0.0)),
        bool(payload.get("uncertain", True)),
        candidates,
        float(payload.get("tuning_cents", 0.0)),
    )


def analyse_source(
    path: str | Path,
    *,
    mode: str = "fast",
    device: str = "auto",
    model_dir: str | Path | None = None,
    harmony_paths: tuple[str | Path, ...] = (),
    bass_path: str | Path | None = None,
    cache: AnalysisCache | None = None,
    cancellation: CancellationToken | None = None,
    progress: Callable[[float, str], None] | None = None,
) -> SourceAnalysis:
    """Run or retrieve beat and key analyses, decoding the source only on a miss."""
    if mode not in MODEL_SPECS:
        raise ValueError(f"Unknown source-analysis mode: {mode}")
    source = Path(path).expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"Source audio does not exist: {source}")

    token = cancellation or CancellationToken()
    report = progress or (lambda _fraction, _stage: None)
    report(0.01, "Hashing source audio")
    source_hash = source_identity(
        source,
        token,
        lambda value: report(0.01 + value * 0.04, "Hashing source audio"),
    )
    cache = cache or AnalysisCache()

    # Decoding is deferred and memoized: a fully cached run (beats, key, and
    # mix evidence all present) must never decode the source at all.
    decoded: dict[str, tuple[np.ndarray, int]] = {}

    def get_audio(fraction: float = 0.06) -> tuple[np.ndarray, int]:
        if "mix" not in decoded:
            report(fraction, "Decoding source audio")
            audio, sample_rate = _load_signal(source)
            if audio.size == 0:
                raise RuntimeError("Source audio is empty")
            token.raise_if_cancelled()
            decoded["mix"] = (audio, sample_rate)
        return decoded["mix"]

    beat_settings = {
        "mode": mode,
        "model": MODEL_SPECS[mode].name,
        "postprocessor": "minimal",
    }
    cached_beat = cache.get_result("beats", source_hash, BEAT_ALGORITHM_VERSION, beat_settings)
    if cached_beat is None:
        audio, sample_rate = get_audio()
        beat = analyse_beats(
            audio,
            sample_rate,
            mode=mode,
            device=device,
            model_dir=model_dir,
            cancellation=token,
            progress=lambda value, stage: report(0.08 + value * 0.58, stage),
        )
        cache.put_result(
            "beats", source_hash, BEAT_ALGORITHM_VERSION, beat_settings, beat.to_dict()
        )
    else:
        beat = _beat_from_cache(cached_beat)
        report(0.66, f"Loaded cached Beat This! {beat.model} analysis")

    harmony_files = tuple(Path(item).expanduser().resolve() for item in harmony_paths)
    bass_file = Path(bass_path).expanduser().resolve() if bass_path else None
    harmony_present = [item for item in harmony_files if item.is_file()]
    harmony_hashes = [source_identity(item, token) for item in harmony_present]
    bass_present = bass_file if bass_file and bass_file.is_file() else None
    bass_hash = source_identity(bass_present, token) if bass_present else None
    stem_hashes = harmony_hashes + ([bass_hash] if bass_hash else [])
    key_settings = {"stem_hashes": stem_hashes, "window_seconds": _WINDOW_SECONDS}
    cached_key = cache.get_result("key", source_hash, KEY_ALGORITHM_VERSION, key_settings)
    if cached_key is None:
        report(0.69, "Analysing stable harmonic sections")
        # The mix source carries its own hash so full-mix evidence computed
        # before separation is reused after separation adds stem signals.
        key_sources: list[_KeySource] = [
            _KeySource(lambda: get_audio(0.70), 0.65, 1.0, False, source_hash)
        ]
        divisor = float(max(1, len(harmony_present)))
        for stem, stem_hash in zip(harmony_present, harmony_hashes, strict=True):
            key_sources.append(
                _KeySource(lambda stem=stem: _load_signal(stem), 0.25, divisor, False, stem_hash)
            )
        if bass_present:
            key_sources.append(
                _KeySource(
                    lambda: _load_signal(bass_present), 0.10, 1.0, True, bass_hash
                )
            )
        key_analysis = _analyse_key_sources(key_sources, cache=cache, cancellation=token)
        cache.put_result(
            "key", source_hash, KEY_ALGORITHM_VERSION, key_settings, key_analysis.to_dict()
        )
    else:
        key_analysis = _key_from_cache(cached_key)
        report(0.93, "Loaded cached key analysis")

    correction = cache.get_correction(source_hash)
    bpm = beat.bpm
    key = key_analysis.key
    meter_numerator = beat.meter_numerator
    meter_denominator = beat.meter_denominator
    bar_one = beat.bar_one
    if correction:
        bpm = float(correction["bpm"]) if correction.get("bpm") is not None else bpm
        key = str(correction["key"]) if correction.get("key") else key
        if correction.get("meter_numerator") is not None:
            meter_numerator = int(correction["meter_numerator"])
        if correction.get("meter_denominator") is not None:
            meter_denominator = int(correction["meter_denominator"])
        if correction.get("bar_one") is not None:
            bar_one = float(correction["bar_one"])

    report(1.0, "Source analysis complete")
    return SourceAnalysis(
        key=key,
        bpm=round(float(bpm), 3) if bpm else None,
        key_confidence=round(key_analysis.confidence, 6),
        bpm_confidence=beat.confidence,
        key_candidates=key_analysis.candidates,
        detected_bpm=beat.detected_bpm,
        half_time_bpm=beat.half_time_bpm,
        double_time_bpm=beat.double_time_bpm,
        beats=beat.beats,
        downbeats=beat.downbeats,
        meter_numerator=meter_numerator,
        meter_denominator=meter_denominator,
        bar_one=round(bar_one, 6),
        tempo_is_steady=beat.tempo_is_steady,
        tempo_segments=tuple(
            {
                "start": round(segment.start, 6),
                "end": round(segment.end, 6),
                "bpm": segment.bpm,
            }
            for segment in beat.tempo_segments
        ),
        source_hash=source_hash,
        analysis_mode=mode,
        beat_model=beat.model,
        beat_model_version=beat.model_version,
        device=beat.device,
        tuning_cents=key_analysis.tuning_cents,
        corrected=correction is not None,
    )


def write_analysis(path: str | Path, analysis: SourceAnalysis) -> Path:
    """Atomically write the JSON contract consumed by the JUCE UI."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(asdict(analysis), indent=2), encoding="utf-8")
    os.replace(temporary, path)
    return path


def _set_correction(args: argparse.Namespace, cache: AnalysisCache) -> None:
    source_hash = source_identity(args.input)
    cache.set_correction(
        source_hash,
        {
            "bpm": args.correct_bpm,
            "key": args.correct_key,
            "meter_numerator": args.correct_meter_numerator,
            "meter_denominator": args.correct_meter_denominator,
            "bar_one": args.correct_bar_one,
        },
    )
    print("Analysis correction saved locally", flush=True)


def main() -> None:
    """CLI entry used by JUCE's source-analysis worker."""
    configure_utf8_stdio()
    parser = argparse.ArgumentParser(
        description="Analyze source tempo, beats, meter, and key.",
        epilog=(
            "--set-correction/--forget-correction normally exit after updating the "
            "local store; combined with --output they instead continue into the "
            "cache-aware analysis and write the corrected result in one invocation."
        ),
    )
    parser.add_argument("--input")
    parser.add_argument("--output")
    parser.add_argument("--mode", choices=("fast", "accurate"), default="fast")
    parser.add_argument("--device", default="auto")
    parser.add_argument("--model-dir")
    parser.add_argument("--harmony-stem", action="append", default=[])
    parser.add_argument("--bass-stem")
    parser.add_argument("--cancel-file")
    parser.add_argument("--cache-path")
    parser.add_argument("--clear-cache", action="store_true")
    parser.add_argument("--forget-correction", action="store_true")
    parser.add_argument("--set-correction", action="store_true")
    parser.add_argument("--correct-bpm", type=float)
    parser.add_argument("--correct-key")
    parser.add_argument("--correct-meter-numerator", type=int)
    parser.add_argument("--correct-meter-denominator", type=int)
    parser.add_argument("--correct-bar-one", type=float)
    args = parser.parse_args()

    cache = AnalysisCache(args.cache_path)
    if args.clear_cache:
        print(f"Cleared {cache.clear()} local analysis entries", flush=True)
        return
    if not args.input:
        parser.error("--input is required")
    # Correction flags without --output keep their historical exit-early
    # contract; with --output the same invocation continues into analysis so
    # the plugin can correct and re-emit the contract in one process.
    if args.forget_correction:
        removed = cache.forget_correction(source_identity(args.input))
        print("Correction forgotten" if removed else "No correction was stored", flush=True)
        if not args.output:
            return
    if args.set_correction:
        _set_correction(args, cache)
        if not args.output:
            return
    if not args.output:
        parser.error("--output is required for analysis")

    token = CancellationToken(Path(args.cancel_file) if args.cancel_file else None)

    def report(fraction: float, stage: str) -> None:
        print(f"STEMLAB_PROGRESS {fraction * 100.0:.1f} {stage}", flush=True)

    result = analyse_source(
        args.input,
        mode=args.mode,
        device=args.device,
        model_dir=args.model_dir,
        harmony_paths=tuple(args.harmony_stem),
        bass_path=args.bass_stem,
        cache=cache,
        cancellation=token,
        progress=report,
    )
    output = write_analysis(args.output, result)
    print(f"Source analysis: {output}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except JobCancelled:
        print("STEMLAB_CANCELLED Source analysis cancelled", flush=True)
        raise SystemExit(130) from None
    except Exception as exc:
        print(f"STEMLAB_ERROR Source analysis failed: {exc}", flush=True)
        raise
