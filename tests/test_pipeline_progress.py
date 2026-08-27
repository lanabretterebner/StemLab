"""Progress/ETA protocol invariants across every engine combination.

Drives the real pipeline - stage plan, job-level ETA, fusion, refinement,
finalize - through fake model backends for all six engine x refinement
combinations, and asserts what the JUCE display relies on: a monotonic bar
that actually covers its range, work-in-progress stage labels, and a
job-level ETA that never counts only the current stage.
"""

from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

import stemlab.pipeline as pipeline
from stemlab.audio import STEM_NAMES
from stemlab.pipeline import ENGINE_CHOICES, _JobProgress, separate


def _write_stems(folder, sr=22050, seconds=0.25):
    folder.mkdir(parents=True, exist_ok=True)
    samples = int(sr * seconds)
    tone = 0.1 * np.sin(np.linspace(0.0, 40.0, samples, dtype=np.float64))
    for stem in STEM_NAMES:
        sf.write(folder / f"{stem}.wav", np.stack([tone, tone], axis=1), sr)


class _FakeBackend:
    """Stands in for both model backends: emits progress/ETA, writes stems."""

    def __init__(self, **kwargs):
        self.progress_callback = kwargs.get("progress_callback")
        self.eta_callback = kwargs.get("eta_callback")
        self.download_callback = kwargs.get("download_callback")

    def separate(self, input_path, output_dir):
        for percent, remaining in ((0.0, 40.0), (25.0, 30.0), (50.0, 20.0), (100.0, 0.0)):
            if self.progress_callback:
                self.progress_callback(percent)
            if self.eta_callback and remaining > 0.0:
                self.eta_callback(remaining)
        _write_stems(output_dir)


@pytest.fixture()
def fake_backends(monkeypatch):
    monkeypatch.setattr(pipeline, "RoFormerBackend", _FakeBackend)
    monkeypatch.setattr(pipeline, "DemucsBackend", _FakeBackend)


def _run(tmp_path, engine, refine, lines: list[str] | None = None):
    source = tmp_path / "input.wav"
    _write_stems(tmp_path / "srcdir")
    (tmp_path / "srcdir" / "vocals.wav").replace(source)

    reports: list[tuple[float, str]] = []
    etas: list[float] = []

    def on_log(message: str) -> None:
        if lines is not None:
            lines.append(message)

        if message.startswith("STEMLAB_ETA "):
            etas.append(float(message.split()[1]))

    separate(
        input_path=source,
        output_dir=tmp_path / "out",
        engine=engine,
        refine=refine,
        device="cpu",
        log_callback=on_log,
        progress_callback=lambda percent, stage: reports.append((percent, stage)),
    )

    return reports, etas


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
@pytest.mark.parametrize("refine", (True, False))
def test_progress_covers_and_never_rewinds(tmp_path, fake_backends, engine, refine):
    reports, _ = _run(tmp_path, engine, refine)

    percents = [percent for percent, _stage in reports]

    assert percents == sorted(percents), f"bar rewound: {percents}"
    assert percents[0] <= 6.0
    assert percents[-1] == 95.0, "pipeline must hand over to the export tail at 95"

    # The refinement toggle must re-plan the bar, not leave a dead band:
    # whatever the last real work stage is (model, fusion, or refinement),
    # it has to carry the bar to the finalize band's doorstep instead of
    # parking early the way the fixed 80% plan used to with refine off.
    work = [p for p, stage in reports if "Collecting" not in stage]
    assert max(work) >= (93.0 if refine else 92.0)


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
def test_stage_labels_name_work_in_progress(tmp_path, fake_backends, engine):
    reports, _ = _run(tmp_path, engine, refine=True)

    labels = [stage for _percent, stage in reports]

    assert all(labels), "every report carries a stage label"

    # Numbered steps so the user can see where they are in the job.
    step_count = {"roformer": 3, "demucs": 3, "hybrid": 5}[engine]
    assert any(f"Step 1/{step_count}" in label for label in labels)
    assert any(f"Step {step_count}/{step_count}" in label for label in labels)

    # Refinement narrates its slow pre-loop analysis and each stem.
    assert any("kick" in label.lower() for label in labels)
    assert any("Refining" in label for label in labels)

    if engine == "hybrid":
        assert any("Fusing" in label for label in labels)


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
def test_eta_is_job_level(tmp_path, fake_backends, engine):
    _, etas = _run(tmp_path, engine, refine=True)

    assert etas, "model-stage estimates must reach the plugin"

    # The fake backend's first stage report says 40 seconds remain in the
    # model stage. A job-level ETA must also cover everything after that
    # stage, so the first published estimate has to be strictly larger.
    assert etas[0] > 40.0

    assert all(value >= 0.0 for value in etas)


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
@pytest.mark.parametrize("refine", (True, False))
def test_stem_ready_lines_share_the_stream_without_disturbing_it(
    tmp_path, fake_backends, engine, refine
):
    lines: list[str] = []

    reports, etas = _run(tmp_path, engine, refine, lines=lines)

    ready = [line for line in lines if line.startswith("STEMLAB_STEM_READY ")]

    assert len(ready) == len(STEM_NAMES), "every stem reaches the plugin as it lands"

    # The plugin dispatches on the prefix, so a new line type must not be
    # readable as one of the older ones: no ready line may parse where a
    # progress or ETA line is expected, and the two older streams must be
    # exactly what they were before this one joined them.
    assert not any(line.startswith(("STEMLAB_PROGRESS ", "STEMLAB_ETA ")) for line in ready)
    assert len([line for line in lines if line.startswith("STEMLAB_PROGRESS ")]) == len(reports)
    assert len([line for line in lines if line.startswith("STEMLAB_ETA ")]) == len(etas)


class _DownloadingBackend(_FakeBackend):
    """A backend whose model has to download before it can separate."""

    def separate(self, input_path, output_dir):
        for percent in (0.0, 50.0, 100.0):
            if self.download_callback:
                self.download_callback(percent)

        super().separate(input_path, output_dir)


def test_first_use_downloads_are_named_and_stay_in_their_sliver(tmp_path, monkeypatch):
    monkeypatch.setattr(pipeline, "RoFormerBackend", _DownloadingBackend)
    monkeypatch.setattr(pipeline, "DemucsBackend", _DownloadingBackend)

    reports, _ = _run(tmp_path, "roformer", refine=False)

    downloads = [(percent, stage) for percent, stage in reports if "Downloading the" in stage]

    assert downloads, "a first-use model download must be named in the status"
    assert any("BS-RoFormer model (50%)" in stage for _percent, stage in downloads)

    # The download creeps only the sliver at the bottom of the model band,
    # so the bar never claims separation progress that has not happened:
    # even a finished download sits at or below separation's first report.
    separating = [percent for percent, stage in reports if "separating (" in stage]

    assert separating
    assert max(percent for percent, _stage in downloads) <= min(separating) + 1e-6


def test_job_progress_extends_stage_eta_across_remaining_stages(monkeypatch):
    published: list[float] = []
    clock = {"now": 100.0}

    monkeypatch.setattr(pipeline.time, "monotonic", lambda: clock["now"])

    tracker = _JobProgress(lambda percent, stage: None, published.append)

    # Stage owns 10..40 of the bar; 30 seconds spent so far, 30 to go, so
    # the stage totals 60s => 2 s/percent => the 60 percent after it cost
    # ~120s more on top of the 30 still inside the stage.
    tracker.begin(10.0, 40.0)
    clock["now"] += 30.0
    tracker.stage(0.5, "half")
    tracker.stage_eta(30.0)

    assert published == [pytest.approx(150.0)]


def test_job_progress_eta_does_not_collapse_at_stage_end(monkeypatch):
    published: list[float] = []
    clock = {"now": 0.0}

    monkeypatch.setattr(pipeline.time, "monotonic", lambda: clock["now"])

    tracker = _JobProgress(lambda percent, stage: None, published.append)

    # A model stage covering 6..74 finishes (60s spent, ~0 left) while the
    # refinement and export bands still remain: the job ETA must reflect
    # them instead of announcing zero seconds left at 74%.
    tracker.begin(6.0, 74.0)
    clock["now"] += 60.0
    tracker.stage(1.0, "done")
    tracker.stage_eta(0.0)

    assert published[-1] == pytest.approx(60.0 / 68.0 * 26.0)


def test_job_progress_infers_eta_from_elapsed(monkeypatch):
    published: list[float] = []
    clock = {"now": 100.0}

    monkeypatch.setattr(pipeline.time, "monotonic", lambda: clock["now"])

    tracker = _JobProgress(lambda percent, stage: None, published.append)

    tracker.begin(90.0, 95.0)
    clock["now"] += 10.0
    tracker.stage(0.5, "half the finalize stage in ten seconds")
    tracker.elapsed_eta()

    # Stage totals 20s (10s bought half of it): 4 s/percent, 5 percent of
    # job left after the stage => 10s stage remainder + 20s tail.
    assert published == [pytest.approx(30.0)]
