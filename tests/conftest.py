"""Shared test isolation, and the fakes more than one test file needs.

Every test gets its own STEMLAB_ANALYSIS_HOME so nothing that resolves the
managed analysis directory - the sqlite caches, the device-probe cache -
can leak state between tests or touch the developer's real cache.
The device probe made this load-bearing: in an environment where torch is
installed (CI), one test faking "cuda unavailable" would otherwise write a
cached answer a later test faking "cuda available" reads back as a hit.

The rest of this file is the model-backend fake and the stem writer that
drive the pipeline without a checkpoint. They lived in three test files as
three copies that had already drifted apart - one reported a single 100%,
another a whole progress schedule - so a test's behaviour depended on which
file it happened to sit in.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf


@pytest.fixture(autouse=True)
def _isolated_analysis_home(tmp_path_factory, monkeypatch):
    monkeypatch.setenv(
        "STEMLAB_ANALYSIS_HOME", str(tmp_path_factory.mktemp("analysis-home"))
    )


def write_stem_folder(folder, sample_rate: int = 22_050, seconds: float = 0.25) -> Path:
    """Write one short tone per stem: the folder a model backend leaves behind.

    Short and quiet on purpose. Everything downstream of a backend cares about
    the six names and the sample count, not the content, and a quarter of a
    second keeps a whole six-engine parametrisation affordable.
    """
    from stemlab.audio import STEM_NAMES

    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)

    samples = int(sample_rate * seconds)
    tone = 0.1 * np.sin(np.linspace(0.0, 40.0, samples, dtype=np.float64))

    for stem in STEM_NAMES:
        sf.write(folder / f"{stem}.wav", np.stack([tone, tone], axis=1), sample_rate)

    return folder


@pytest.fixture()
def write_stems():
    """``write_stem_folder``, for a test that needs a stem folder of its own."""
    return write_stem_folder


class FakeSeparationBackend:
    """Stands in for RoFormerBackend and DemucsBackend.

    The pipeline constructs a backend with keyword callbacks and then calls
    ``separate``; this reports what a real backend reports and writes what a
    real one leaves in the output folder. Everything it varies is a class
    attribute, so a test changes one on the class the ``fake_backends``
    fixture hands it rather than writing a subclass of its own.
    """

    # Percentages a first-use model download reports before separating. Empty
    # because the ordinary case is a model already on disk.
    download_percentages: tuple[float, ...] = ()

    # (percent, seconds remaining) as a separation reports them. A real
    # backend reports repeatedly and finishes at 100, which is what the
    # progress and ETA arithmetic downstream is written against.
    progress_schedule = ((0.0, 40.0), (25.0, 30.0), (50.0, 20.0), (100.0, 0.0))

    # What lands in the output folder. A test whose subject is the audio
    # itself points this at a writer of its own.
    write_output = write_stem_folder

    def __init__(self, **kwargs):
        self.progress_callback = kwargs.get("progress_callback")
        self.eta_callback = kwargs.get("eta_callback")
        self.download_callback = kwargs.get("download_callback")

    def separate(self, input_path, output_dir):
        for percent in self.download_percentages:
            if self.download_callback:
                self.download_callback(percent)

        for percent, remaining in self.progress_schedule:
            if self.progress_callback:
                self.progress_callback(percent)

            # Zero seconds remaining is the end of the stage, not an estimate.
            if self.eta_callback and remaining > 0.0:
                self.eta_callback(remaining)

        # Looked up on the class, not through ``self``: an attribute holding a
        # plain function would bind as a method and be handed the backend as
        # its first argument, so a test could only swap the writer by wrapping
        # it in ``staticmethod``.
        type(self).write_output(Path(output_dir))


@pytest.fixture()
def fake_backends(monkeypatch):
    """Replace both model backends in the pipeline with the fake above.

    Returns the class it installed - a fresh subclass for each test, so a
    test may set ``download_percentages`` or ``write_output`` on it without
    the next test inheriting the change.
    """
    # Imported here rather than at the top of the file: this conftest is
    # loaded for every test in the suite, and most of them never separate
    # anything, so the pipeline's own imports are not theirs to pay for.
    import stemlab.pipeline as pipeline

    class _InstalledBackend(FakeSeparationBackend):
        pass

    monkeypatch.setattr(pipeline, "RoFormerBackend", _InstalledBackend)
    monkeypatch.setattr(pipeline, "DemucsBackend", _InstalledBackend)

    return _InstalledBackend
