"""What the hybrid fusion -> refinement handoff is allowed to keep in memory.

Every fused stem is a full-length ``[2, N]`` float32 array - about 100 MiB
for a five-minute track - so the difference between handing one over and
collecting all six is most of a gigabyte on a real job. These tests pin the
shape rather than the number: fusion holds nothing of its own, and only the
stems refinement actually decodes survive the stage. The equivalence test
guards the reason the handoff exists at all - the arrays must be worth
exactly what re-reading the files would have produced.
"""

from __future__ import annotations

import gc
import weakref
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from scipy import signal

import stemlab.hybrid as hybrid
import stemlab.pipeline as pipeline
from stemlab.audio import STEM_NAMES, load_audio
from stemlab.hybrid import fuse_stem_folders
from stemlab.pipeline import separate
from stemlab.refinement.pipeline import refine_stem_folder, reusable_stems

SAMPLE_RATE = 44100


def _kick(seconds: float = 0.18) -> np.ndarray:
    count = int(SAMPLE_RATE * seconds)
    time = np.arange(count, dtype=np.float32) / SAMPLE_RATE
    phase = 2 * np.pi * (52.0 * time + (135.0 - 52.0) * (1.0 - np.exp(-time * 24.0)) / 24.0)
    body = 0.90 * np.exp(-time * 22.0) * np.sin(phase)
    click = 0.10 * np.exp(-time * 170.0) * np.sin(2 * np.pi * 1700.0 * time)
    return (body + click).astype(np.float32)


def _write_material(folder: Path, seconds: float = 2.4) -> Path:
    """Six stems carrying kick bleed the refiner actually cancels.

    A no-op refinement would let a broken handoff compare equal, so the
    tonal beds sit where the matcher accepts the leak (see test_events).
    """
    folder.mkdir(parents=True, exist_ok=True)

    count = int(SAMPLE_RATE * seconds)
    time = np.arange(count, dtype=np.float32) / SAMPLE_RATE
    kick = _kick()
    leaked = signal.lfilter([1.0, -0.16], [1.0], np.roll(kick, 17)).astype(np.float32)
    hits = list(zip((0.30, 0.72, 1.12, 1.48, 1.86), (1.0, 0.78, 0.93, 0.70, 1.0), strict=True))

    for index, stem in enumerate(STEM_NAMES):
        if stem == "drums":
            # A drums stem carries no sustained bass tone, and one here
            # would swamp the low-frequency envelope detection works on.
            mono = np.zeros(count, dtype=np.float32)
        else:
            mono = (
                0.20 * np.sin(2 * np.pi * (73.4 + 4.0 * index) * time)
                + 0.06 * np.sin(2 * np.pi * (146.8 + 8.0 * index) * time)
            ).astype(np.float32)

        for at, velocity in hits:
            start = int(at * SAMPLE_RATE)
            sound = kick if stem == "drums" else leaked
            gain = velocity if stem == "drums" else 0.26 * velocity
            length = min(count - start, len(sound))
            if length > 0:
                mono[start : start + length] += gain * sound[:length]

        sf.write(
            folder / f"{stem}.wav",
            np.stack([mono, mono], axis=1),
            SAMPLE_RATE,
            subtype="FLOAT",
        )

    return folder


@pytest.fixture()
def material(tmp_path):
    return _write_material(tmp_path / "stems")


def _live_fused_arrays(monkeypatch) -> list[weakref.ref]:
    """Weak references to every array ``fuse_stem_pair`` produces.

    Weak, so counting the survivors answers what fusion pins without the
    census itself being what keeps them alive.
    """
    seen: list[weakref.ref] = []
    real = hybrid.fuse_stem_pair

    def traced(*args, **kwargs):
        audio, sr = real(*args, **kwargs)
        seen.append(weakref.ref(audio))
        return audio, sr

    monkeypatch.setattr(hybrid, "fuse_stem_pair", traced)
    return seen


def _count_live(refs: list[weakref.ref]) -> int:
    gc.collect()
    return sum(1 for ref in refs if ref() is not None)


def test_each_fused_stem_is_handed_over_once_and_matches_its_file(tmp_path, material):
    handed: list[tuple[str, np.ndarray, int]] = []

    fuse_stem_folders(
        roformer_dir=material,
        demucs_dir=material,
        output_dir=tmp_path / "fused",
        fused_callback=lambda stem, audio, sr: handed.append((stem, audio, sr)),
    )

    assert sorted(stem for stem, _audio, _sr in handed) == sorted(STEM_NAMES)

    for stem, audio, sr in handed:
        written, written_sr = load_audio(tmp_path / "fused" / f"{stem}.wav")

        assert sr == written_sr
        # The whole point of the handoff: reusing this array must be the
        # same as decoding the file, not merely close to it.
        assert np.array_equal(audio, written)


def test_fusion_pins_no_fused_audio_of_its_own(tmp_path, monkeypatch, material):
    """Without a taker, a fused stem dies while the folder is still fusing.

    The futures used to be the owner: each held its worker's return value
    until the pool closed, so all six full-length arrays were live by the
    last stem no matter what the caller wanted.
    """
    refs = _live_fused_arrays(monkeypatch)
    census: list[int] = []

    fuse_stem_folders(
        roformer_dir=material,
        demucs_dir=material,
        output_dir=tmp_path / "fused",
        ready_callback=lambda stem, path: census.append(_count_live(refs)),
    )

    assert len(census) == len(STEM_NAMES)
    assert max(census) < len(STEM_NAMES)

    # Only the stems inside a worker right now, and the pool runs two.
    assert max(census) <= 3
    assert _count_live(refs) == 0


def test_handoff_carries_only_the_stems_refinement_decodes(
    tmp_path, monkeypatch, material, fake_backends
):
    """A hybrid job hands over the reused stems and drops the rest.

    ``vocals`` is the one that never repaid its keep: refinement byte-copies
    it and never asks for its audio, so it used to sit in memory for the
    whole stage having been read by nobody.
    """
    # The shared fake writes plain tones; this job needs the kick-bleed
    # material, because a refinement that cancels nothing would let a broken
    # handoff pass.
    fake_backends.write_output = _write_material

    refs = _live_fused_arrays(monkeypatch)
    handed_to_refinement: list[set[str]] = []
    real_refine = pipeline.refine_stem_folder

    def spy(**kwargs):
        handed_to_refinement.append(set(kwargs["preloaded"]))
        return real_refine(**kwargs)

    monkeypatch.setattr(pipeline, "refine_stem_folder", spy)

    separate(
        input_path=material / "vocals.wav",
        output_dir=tmp_path / "job",
        engine="hybrid",
        refine=True,
        device="cpu",
        log_callback=lambda message: None,
    )

    assert handed_to_refinement == [reusable_stems()]
    assert "vocals" not in handed_to_refinement[0]

    # Refinement pops what it consumes, so the job ends owning none of them.
    assert _count_live(refs) == 0


def test_a_run_without_refinement_keeps_no_fused_stem_at_all(
    tmp_path, monkeypatch, material, fake_backends
):
    fake_backends.write_output = _write_material

    refs = _live_fused_arrays(monkeypatch)

    separate(
        input_path=material / "vocals.wav",
        output_dir=tmp_path / "job",
        engine="hybrid",
        refine=False,
        device="cpu",
        log_callback=lambda message: None,
    )

    assert refs, "the fusion stage has to have run"
    assert _count_live(refs) == 0


def test_preloaded_refinement_writes_what_a_disk_only_run_writes(tmp_path, material):
    """The handoff is an optimization, so it may not move a single sample."""
    fused_dir = tmp_path / "fused"
    handed: dict[str, tuple[np.ndarray, int]] = {}

    keep = reusable_stems()

    fuse_stem_folders(
        roformer_dir=material,
        demucs_dir=material,
        output_dir=fused_dir,
        fused_callback=lambda stem, audio, sr: (
            handed.__setitem__(stem, (audio, sr)) if stem in keep else None
        ),
    )

    from_memory = refine_stem_folder(
        input_dir=fused_dir,
        output_dir=tmp_path / "from_memory",
        preloaded=dict(handed),
    )
    from_disk = refine_stem_folder(
        input_dir=fused_dir,
        output_dir=tmp_path / "from_disk",
    )

    applied = sum(stats.cancellations_applied for stats in from_disk.values())
    assert applied > 0, "material that refines to a no-op cannot prove anything"

    for stem in STEM_NAMES:
        memory_audio, memory_sr = load_audio(tmp_path / "from_memory" / f"{stem}.wav")
        disk_audio, disk_sr = load_audio(tmp_path / "from_disk" / f"{stem}.wav")

        assert memory_sr == disk_sr
        assert np.array_equal(memory_audio, disk_audio), stem

    assert from_memory.keys() == from_disk.keys()
