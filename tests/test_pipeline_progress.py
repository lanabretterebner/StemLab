"""Progress/ETA protocol invariants across every engine combination.

Drives the real pipeline - stage plan, job-level ETA, fusion, refinement,
finalize - through fake model backends for all six engine x refinement
combinations, and asserts what the JUCE display relies on: a monotonic bar
that actually covers its range, work-in-progress stage labels, and a
job-level ETA that never counts only the current stage.
"""

from __future__ import annotations

import pytest

import stemlab.pipeline as pipeline
from stemlab.audio import STEM_NAMES
from stemlab.pipeline import ENGINE_CHOICES, _JobProgress, separate


@pytest.fixture()
def run_job(tmp_path, fake_backends, write_stems):
    """Run one separation and hand back its progress reports and its ETAs.

    The fake backends and the source file come with it, so each test below
    names only what it varies: the engine, whether refinement runs, and - for
    the tests that read the raw stream - a list to collect the log lines in.
    """

    def run(engine: str, refine: bool, lines: list[str] | None = None):
        source = tmp_path / "input.wav"
        write_stems(tmp_path / "srcdir")
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

    return run


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
@pytest.mark.parametrize("refine", (True, False))
def test_progress_covers_and_never_rewinds(run_job, engine, refine):
    reports, _ = run_job(engine, refine)

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
def test_stage_labels_name_work_in_progress(run_job, engine):
    reports, _ = run_job(engine, refine=True)

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
def test_eta_is_job_level(run_job, engine):
    _, etas = run_job(engine, refine=True)

    assert etas, "model-stage estimates must reach the plugin"

    # The fake backend's first stage report says 40 seconds remain in the
    # model stage. A job-level ETA must also cover everything after that
    # stage, so the first published estimate has to be strictly larger.
    assert etas[0] > 40.0

    assert all(value >= 0.0 for value in etas)


@pytest.mark.parametrize("engine", ENGINE_CHOICES)
@pytest.mark.parametrize("refine", (True, False))
def test_stem_ready_lines_share_the_stream_without_disturbing_it(run_job, engine, refine):
    lines: list[str] = []

    reports, etas = run_job(engine, refine, lines=lines)

    ready = [line for line in lines if line.startswith("STEMLAB_STEM_READY ")]

    assert len(ready) == len(STEM_NAMES), "every stem reaches the plugin as it lands"

    # The plugin dispatches on the prefix, so a new line type must not be
    # readable as one of the older ones: no ready line may parse where a
    # progress or ETA line is expected, and the two older streams must be
    # exactly what they were before this one joined them.
    assert not any(line.startswith(("STEMLAB_PROGRESS ", "STEMLAB_ETA ")) for line in ready)
    assert len([line for line in lines if line.startswith("STEMLAB_PROGRESS ")]) == len(reports)
    assert len([line for line in lines if line.startswith("STEMLAB_ETA ")]) == len(etas)


def test_first_use_downloads_are_named_and_stay_in_their_sliver(run_job, fake_backends):
    # A model that is not on disk yet: the backends report a download before
    # they report any separation at all.
    fake_backends.download_percentages = (0.0, 50.0, 100.0)

    reports, _ = run_job("roformer", refine=False)

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
