"""Exercise packaged FI-STEM analysis without allowing model downloads."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

from stemlab.analysis_cache import AnalysisCache
from stemlab.beat_tracking import resolve_packaged_model
from stemlab.source_analysis import analyse_source


def main() -> None:
    engine = Path(os.environ.get("STEMLAB_ENGINE_DIR", Path(os.sys.executable).parent)).resolve()
    models = engine / "Models" / "BeatThis"
    resolve_packaged_model("fast", models)
    resolve_packaged_model("accurate", models)

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
