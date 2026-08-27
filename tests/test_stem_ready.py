"""STEMLAB_STEM_READY: what the plugin may act on before a job finishes.

Drives the real pipeline through fake model backends and reads the wire
the way the JUCE side does - first token is the stem, the rest of the line
is the path - asserting what progressive audition rests on: one line per
stem, naming the file the plugin keeps reading for the rest of the job,
written only once that file is complete on disk. A refine run therefore
must never name a baseline/ path, since the plugin resolves stems out of
job/refined from the moment that directory exists.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

import stemlab.pipeline as pipeline
from stemlab.audio import STEM_NAMES
from stemlab.pipeline import ENGINE_CHOICES, separate

READY_PREFIX = "STEMLAB_STEM_READY "

# Logged once refinement is skipped, so every line before it was published
# by a working stage rather than by the end-of-job backstop.
REFINE_OFF = "Refinement disabled; baseline stems are final output."


def _write_stems(folder, sr=22050, seconds=0.25):
    folder.mkdir(parents=True, exist_ok=True)
    samples = int(sr * seconds)
    tone = 0.1 * np.sin(np.linspace(0.0, 40.0, samples, dtype=np.float64))
    for stem in STEM_NAMES:
        sf.write(folder / f"{stem}.wav", np.stack([tone, tone], axis=1), sr)


class _FakeBackend:
    """Stands in for both model backends: writes a full stem folder."""

    def __init__(self, **kwargs):
        self.progress_callback = kwargs.get("progress_callback")

    def separate(self, input_path, output_dir):
        if self.progress_callback:
            self.progress_callback(100.0)
        _write_stems(output_dir)


@pytest.fixture()
def fake_backends(monkeypatch):
    monkeypatch.setattr(pipeline, "RoFormerBackend", _FakeBackend)
    monkeypatch.setattr(pipeline, "DemucsBackend", _FakeBackend)


class _Wire:
    """The engine's stdout in order, with the ready lines parsed out.

    ``existed`` is sampled inside the callback, so it records whether the
    announced file was on disk at the moment the line was written, not at
    the end of the run when everything is finished anyway.
    """

    def __init__(self):
        self.lines: list[str] = []
        self.ready: list[tuple[str, Path, bool]] = []

    def __call__(self, message: str) -> None:
        self.lines.append(message)

        if message.startswith(READY_PREFIX):
            stem, _, path = message[len(READY_PREFIX):].partition(" ")
            self.ready.append((stem, Path(path), Path(path).exists()))

    @property
    def stems(self) -> list[str]:
        return [stem for stem, _path, _existed in self.ready]

    def index_of_last_ready(self) -> int:
        return max(
            index for index, line in enumerate(self.lines) if line.startswith(READY_PREFIX)
        )


def _run(tmp_path, engine, refine) -> _Wire:
    source = tmp_path / "input.wav"
    _write_stems(tmp_path / "srcdir")
    (tmp_path / "srcdir" / "vocals.wav").replace(source)

    wire = _Wire()

    separate(
        input_path=source,
        output_dir=tmp_path / "out",
        engine=engine,
        refine=refine,
        device="cpu",
        log_callback=wire,
    )

    return wire


def _assert_wire_is_well_formed(wire: _Wire) -> None:
    """Invariants the C++ parser assumes about every announcement."""
    for stem, path, existed in wire.ready:
        assert stem in STEM_NAMES
        assert path.is_absolute(), f"{stem}: {path}"
        assert existed, f"{stem} announced before its file existed: {path}"

    assert len(wire.stems) == len(set(wire.stems)), f"stem announced twice: {wire.stems}"


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
def test_refinement_announces_each_stem_once_from_the_refined_folder(
    tmp_path, fake_backends, engine
):
    wire = _run(tmp_path, engine, refine=True)

    _assert_wire_is_well_formed(wire)

    assert sorted(wire.stems) == sorted(STEM_NAMES)

    refined = (tmp_path / "out" / "refined").resolve()
    baseline = (tmp_path / "out" / "baseline").resolve()

    # Not vacuous: the baseline stems exist and are deliberately not the
    # ones announced, because the plugin stops resolving to them the moment
    # refined/ appears.
    assert sorted(path.name for path in baseline.glob("*.wav")) == sorted(
        f"{stem}.wav" for stem in STEM_NAMES
    )

    for stem, path, _existed in wire.ready:
        assert path.parent == refined, f"{stem} announced outside the final folder: {path}"
        assert baseline not in path.parents, f"{stem} announced from baseline: {path}"


def test_refinement_announces_the_untouched_copies_first(tmp_path, fake_backends):
    wire = _run(tmp_path, "roformer", refine=True)

    # Refinement writes in STEM_NAMES order, and vocals/drums are the two
    # stems it copies rather than processes: the plugin gets a playable
    # lane from them well before the kick-bleed work finishes.
    assert wire.stems[:2] == ["vocals", "drums"]
    assert wire.stems == list(STEM_NAMES)


def test_hybrid_without_refinement_announces_from_fusion(tmp_path, fake_backends):
    wire = _run(tmp_path, "hybrid", refine=False)

    _assert_wire_is_well_formed(wire)

    assert sorted(wire.stems) == sorted(STEM_NAMES)

    baseline = (tmp_path / "out" / "baseline").resolve()

    for stem, path, _existed in wire.ready:
        assert path.parent == baseline, f"{stem} announced outside the final folder: {path}"

    # Every line landed while fusion was still running, so the stems were
    # published as they fused rather than swept up at the end of the job.
    assert wire.index_of_last_ready() < wire.lines.index(REFINE_OFF)


@pytest.mark.parametrize("engine", ("roformer", "demucs"))
def test_single_model_without_refinement_announces_after_separation(
    tmp_path, fake_backends, engine
):
    wire = _run(tmp_path, engine, refine=False)

    _assert_wire_is_well_formed(wire)

    assert sorted(wire.stems) == sorted(STEM_NAMES)

    baseline = (tmp_path / "out" / "baseline").resolve()

    for _stem, path, _existed in wire.ready:
        assert path.parent == baseline

    assert wire.index_of_last_ready() < wire.lines.index(REFINE_OFF)


@pytest.mark.parametrize("name", ("piano\nvocals.wav", "piano\x00.wav"))
def test_a_path_holding_a_control_character_is_skipped(
    tmp_path, fake_backends, monkeypatch, name
):
    resolve = pipeline.find_stem_file

    def poisoned(folder, stem):
        path = resolve(folder, stem)

        if path is not None and stem == "piano":
            return path.with_name(name)

        return path

    monkeypatch.setattr(pipeline, "find_stem_file", poisoned)

    wire = _run(tmp_path, "roformer", refine=False)

    # A newline would split one announcement into two lines, the second of
    # which reads as a valid announcement of a different stem; a null byte
    # cannot even be resolved. Either way the stem drops out of the wire
    # rather than taking the job down with it.
    assert "piano" not in wire.stems
    assert sorted(wire.stems) == sorted(stem for stem in STEM_NAMES if stem != "piano")
    assert all("\n" not in line and "\r" not in line for line in wire.lines)
