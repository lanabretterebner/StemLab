"""A deterministic synthetic corpus, so the harness can run without music.

This exists so CI has something to separate that nobody needs a licence for,
and so a catastrophic regression - a dead stem, a NaN, swapped channels, a
resampling mistake - is caught on every commit rather than on a user's track.

It is deliberately NOT a quality benchmark. Synthesised instruments are far
easier to pull apart than a real mix, so the SDR a separator scores here says
almost nothing about how it will sound on actual music. Absolute quality has
to be gated on real material; this catches the failures that are obvious once
you look and invisible until you do.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import butter, sosfilt

CORPUS_STEMS = ("bass", "drums", "vocals", "guitar", "piano", "other")

# A ii-V-I in C, one chord per bar, as MIDI note numbers.
_PROGRESSION = (
    (62, 65, 69),  # Dm
    (67, 71, 74),  # G
    (60, 64, 67),  # C
    (60, 64, 67),  # C
)


def _midi_to_hz(note: float) -> float:
    return 440.0 * (2.0 ** ((note - 69.0) / 12.0))


def _envelope(length: int, attack: int, decay: float, sample_rate: int) -> np.ndarray:
    """A percussive attack-and-decay envelope, click-free at both ends."""
    time = np.arange(length) / sample_rate
    env = np.exp(-time / decay)

    if attack > 0:
        ramp = min(attack, length)
        env[:ramp] *= np.linspace(0.0, 1.0, ramp)

    return env


def _harmonic_note(
    frequency: float,
    length: int,
    sample_rate: int,
    partials: int,
    decay: float,
    inharmonicity: float = 0.0,
    vibrato_hz: float = 0.0,
    vibrato_depth: float = 0.0,
) -> np.ndarray:
    """One note as a decaying harmonic stack."""
    time = np.arange(length) / sample_rate
    note = np.zeros(length)

    for partial in range(1, partials + 1):
        # Real strings stretch their upper partials; a little of it keeps the
        # stems from being trivially separable by exact harmonic ratios.
        ratio = partial * (1.0 + inharmonicity * partial * partial)
        phase = 2.0 * np.pi * frequency * ratio * time

        if vibrato_hz > 0.0:
            phase += vibrato_depth * np.sin(2.0 * np.pi * vibrato_hz * time)

        note += np.sin(phase) / partial

    return note * _envelope(length, int(0.005 * sample_rate), decay, sample_rate)


def _bass(bars: int, bar_samples: int, sample_rate: int) -> np.ndarray:
    track = np.zeros(bars * bar_samples)

    for bar in range(bars):
        root = _PROGRESSION[bar % len(_PROGRESSION)][0] - 24
        for eighth in range(8):
            start = bar * bar_samples + eighth * bar_samples // 8
            length = bar_samples // 8
            note = _midi_to_hz(root + (7 if eighth % 4 == 3 else 0))
            track[start : start + length] += _harmonic_note(
                note, length, sample_rate, partials=6, decay=0.28
            )

    sos = butter(4, 400.0, btype="low", fs=sample_rate, output="sos")
    return sosfilt(sos, track) * 0.8


def _drums(bars: int, bar_samples: int, sample_rate: int, rng: np.random.Generator) -> np.ndarray:
    track = np.zeros(bars * bar_samples)
    quarter = bar_samples // 4

    for bar in range(bars):
        base = bar * bar_samples

        for beat in range(4):
            start = base + beat * quarter

            if beat in (0, 2):  # kick: a fast downward pitch sweep
                length = min(quarter, int(0.18 * sample_rate))
                time = np.arange(length) / sample_rate
                sweep = 110.0 * np.exp(-time * 28.0) + 42.0
                track[start : start + length] += np.sin(
                    2.0 * np.pi * np.cumsum(sweep) / sample_rate
                ) * _envelope(length, 8, 0.10, sample_rate)

            if beat in (1, 3):  # snare: noise plus a little body
                length = min(quarter, int(0.14 * sample_rate))
                noise = rng.standard_normal(length)
                sos = butter(4, [180.0, 7000.0], btype="band", fs=sample_rate, output="sos")
                body = np.sin(2.0 * np.pi * 190.0 * np.arange(length) / sample_rate)
                track[start : start + length] += (
                    sosfilt(sos, noise) * 0.7 + body * 0.3
                ) * _envelope(length, 4, 0.06, sample_rate)

            for offset in (0, quarter // 2):  # hats on eighths
                hat_start = start + offset
                length = min(quarter // 2, int(0.04 * sample_rate))
                noise = rng.standard_normal(length)
                sos = butter(4, 8000.0, btype="high", fs=sample_rate, output="sos")
                track[hat_start : hat_start + length] += (
                    sosfilt(sos, noise) * 0.25 * _envelope(length, 2, 0.02, sample_rate)
                )

    return track * 0.7


def _vocals(bars: int, bar_samples: int, sample_rate: int) -> np.ndarray:
    track = np.zeros(bars * bar_samples)
    melody = (72, 74, 76, 74)

    for bar in range(bars):
        for half in range(2):
            start = bar * bar_samples + half * bar_samples // 2
            length = bar_samples // 2
            note = _midi_to_hz(melody[(bar * 2 + half) % len(melody)])
            track[start : start + length] += _harmonic_note(
                note,
                length,
                sample_rate,
                partials=12,
                decay=0.9,
                vibrato_hz=5.2,
                vibrato_depth=0.35,
            )

    # A broad formant bump is what makes this read as voice rather than organ.
    sos = butter(2, [300.0, 3400.0], btype="band", fs=sample_rate, output="sos")
    return sosfilt(sos, track) * 0.65


def _guitar(bars: int, bar_samples: int, sample_rate: int) -> np.ndarray:
    track = np.zeros(bars * bar_samples)

    for bar in range(bars):
        chord = _PROGRESSION[bar % len(_PROGRESSION)]
        for eighth in range(4):
            start = bar * bar_samples + eighth * bar_samples // 4
            length = bar_samples // 4
            for index, note in enumerate(chord):
                # Strum: each string a few milliseconds after the last.
                offset = int(index * 0.008 * sample_rate)
                if start + offset + length > track.size:
                    continue
                track[start + offset : start + offset + length] += 0.4 * _harmonic_note(
                    _midi_to_hz(note), length, sample_rate, partials=10, decay=0.35
                )

    return track * 0.6


def _piano(bars: int, bar_samples: int, sample_rate: int) -> np.ndarray:
    track = np.zeros(bars * bar_samples)

    for bar in range(bars):
        chord = _PROGRESSION[bar % len(_PROGRESSION)]
        start = bar * bar_samples
        length = bar_samples

        for note in chord:
            track[start : start + length] += 0.35 * _harmonic_note(
                _midi_to_hz(note + 12),
                length,
                sample_rate,
                partials=14,
                decay=0.8,
                inharmonicity=0.0004,
            )

    return track * 0.6


def _other(bars: int, bar_samples: int, sample_rate: int, rng: np.random.Generator) -> np.ndarray:
    """A sustained pad: the catch-all stem, and the hardest to pull out."""
    length = bars * bar_samples
    track = np.zeros(length)

    for bar in range(bars):
        chord = _PROGRESSION[bar % len(_PROGRESSION)]
        start = bar * bar_samples
        for note in chord:
            time = np.arange(bar_samples) / sample_rate
            detune = 1.0 + rng.uniform(-0.002, 0.002)
            track[start : start + bar_samples] += 0.25 * np.sin(
                2.0 * np.pi * _midi_to_hz(note - 12) * detune * time
            )

    # Fade the pad in and out so it never clicks at a bar edge.
    ramp = int(0.05 * sample_rate)
    track[:ramp] *= np.linspace(0.0, 1.0, ramp)
    track[-ramp:] *= np.linspace(1.0, 0.0, ramp)

    sos = butter(2, 5000.0, btype="low", fs=sample_rate, output="sos")
    return sosfilt(sos, track) * 0.5


@dataclass
class Corpus:
    """Where a generated corpus landed on disk."""

    mixture: Path
    stems: dict[str, Path]
    sample_rate: int
    duration_seconds: float


def build_corpus(
    destination: Path | str,
    duration_seconds: float = 12.0,
    sample_rate: int = 44100,
    seed: int = 20260827,
    tempo_bpm: float = 100.0,
) -> Corpus:
    """Write a deterministic mixture and its ground-truth stems.

    The same seed always produces byte-identical audio, so a stored metric is
    comparable across machines and across months.
    """
    destination = Path(destination)
    stems_dir = destination / "stems"
    stems_dir.mkdir(parents=True, exist_ok=True)

    rng = np.random.default_rng(seed)
    bar_samples = int(round(4.0 * 60.0 / tempo_bpm * sample_rate))
    bars = max(1, int(round(duration_seconds * sample_rate / bar_samples)))

    rendered = {
        "bass": _bass(bars, bar_samples, sample_rate),
        "drums": _drums(bars, bar_samples, sample_rate, rng),
        "vocals": _vocals(bars, bar_samples, sample_rate),
        "guitar": _guitar(bars, bar_samples, sample_rate),
        "piano": _piano(bars, bar_samples, sample_rate),
        "other": _other(bars, bar_samples, sample_rate, rng),
    }

    length = min(track.size for track in rendered.values())
    mixture = np.zeros(length)

    for name in CORPUS_STEMS:
        rendered[name] = rendered[name][:length]
        mixture += rendered[name]

    # One shared gain for the mix and every stem, so the stems still sum to the
    # mixture exactly - separation metrics depend on that holding.
    peak = float(np.abs(mixture).max(initial=0.0))
    gain = 0.89 / peak if peak > 0.0 else 1.0

    written: dict[str, Path] = {}

    for name in CORPUS_STEMS:
        # Faintly different left/right gains: mono-summing bugs and channel
        # swaps are invisible in a true-mono corpus.
        mono = rendered[name] * gain
        stereo = np.stack((mono * 0.98, mono * 1.02), axis=-1)
        path = stems_dir / f"{name}.wav"
        sf.write(str(path), stereo, sample_rate, subtype="FLOAT")
        written[name] = path

    mix_stereo = np.stack((mixture * gain * 0.98, mixture * gain * 1.02), axis=-1)
    mixture_path = destination / "mixture.wav"
    sf.write(str(mixture_path), mix_stereo, sample_rate, subtype="FLOAT")

    return Corpus(
        mixture=mixture_path,
        stems=written,
        sample_rate=sample_rate,
        duration_seconds=length / sample_rate,
    )
