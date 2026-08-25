from __future__ import annotations

import json

import numpy as np
import soundfile as sf

from stemlab.adaptive.analysis import analyse_audio
from stemlab.recursive import split_lead_group


def _write_synthetic_mix(path, sample_rate: int = 22050) -> None:
    t = np.arange(sample_rate * 2, dtype=np.float32) / sample_rate
    lead = 0.24 * np.sin(2.0 * np.pi * 440.0 * t)
    left = lead + 0.17 * np.sin(2.0 * np.pi * 220.0 * t) + 0.09 * np.sin(2.0 * np.pi * 660.0 * t)
    right = lead + 0.17 * np.sin(2.0 * np.pi * 224.0 * t) - 0.09 * np.sin(2.0 * np.pi * 660.0 * t)
    sf.write(path, np.stack([left, right], axis=1).astype(np.float32), sample_rate)


def test_analysis_estimates_composite_stereo_audio(tmp_path):
    source = tmp_path / "mix.wav"
    _write_synthetic_mix(source)
    profile = analyse_audio(source)
    assert profile.estimated_source_count >= 2
    assert profile.confidence > 0.5


def test_lead_split_writes_variable_tree_manifest(tmp_path):
    source = tmp_path / "mix.wav"
    _write_synthetic_mix(source)
    manifest = split_lead_group(
        source,
        tmp_path / "split",
        parent_id="guitar",
        root_stem="guitar",
        source_category="instrument.guitar",
    )
    data = json.loads(manifest.read_text(encoding="utf-8"))
    assert data["schema"] == 2
    assert data["feature"] == "adaptive_stem_tree"
    assert len(data["children"]) >= 2
    assert data["children"][0]["category"] == "instrument.lead"
    assert data["children"][-1]["category"] == "instrument.bed"
    assert all("estimated_source_count" in child for child in data["children"])
