"""Exercise packaged StemLab analysis end to end, download included.

The bundle ships the Engine, not the weights, so the first analysis fetches
its checkpoint. That fetch is the part of a release most likely to be broken
by something outside the repository - a moved file, a dead host - and the
least likely to be noticed on a developer machine, where the model is
already cached. Running it here means a release build fails rather than a
user's first separation.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

from stemlab.analysis_cache import AnalysisCache
from stemlab.beat_tracking import ensure_packaged_model
from stemlab.source_analysis import analyse_source


def main() -> None:
    engine = Path(os.environ.get("STEMLAB_ENGINE_DIR", Path(os.sys.executable).parent)).resolve()
    print(f"Engine under test: {engine}", flush=True)

    sample_rate = 22_050
    duration = 8.0
    time = np.arange(int(sample_rate * duration), dtype=np.float32) / sample_rate
    audio = 0.18 * np.sin(2.0 * np.pi * 220.0 * time)
    click = np.exp(-np.arange(int(0.03 * sample_rate)) / (0.006 * sample_rate))
    for start in np.arange(0.0, duration, 0.5):
        index = int(start * sample_rate)
        audio[index : index + click.size] += 0.7 * click

    with tempfile.TemporaryDirectory(prefix="fi_stem_offline_smoke_") as temporary:
        root = Path(temporary)

        # Deliberately not the bundle's own Models directory: the bundle
        # ships no weights, and a smoke test that left one behind would
        # quietly change what the installer contains.
        models = root / "Models" / "BeatThis"
        checkpoint = ensure_packaged_model(
            "fast",
            models,
            progress=lambda _fraction, stage: print(stage, flush=True),
        )
        print(f"Beat This! model downloaded and verified: {checkpoint}", flush=True)

        source = root / "source.wav"
        sf.write(source, audio, sample_rate)
        result = analyse_source(
            source,
            mode="fast",
            model_dir=models,
            cache=AnalysisCache(root / "analysis.sqlite3"),
        )

    if result.bpm is None or not 115.0 <= result.bpm <= 125.0 or len(result.beats) < 8:
        raise RuntimeError(f"Portable Beat This smoke test failed: {result}")
    print(
        f"Portable offline smoke passed: {result.bpm:.1f} BPM, {len(result.beats)} beats, "
        f"{result.beat_model} on {result.device}"
    )


if __name__ == "__main__":
    main()
