"""Apply conservative refinement to a complete six-stem output folder."""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Callable

import numpy as np
import soundfile as sf

from ..audio import STEM_NAMES, find_stem_file, load_audio, save_audio
from .events import detect_kick_events
from .kick import (
    KickRefinementConfig,
    KickRefinementStats,
    build_kick_reference,
    refine_kick_bleed,
)

# Stems whose bleed is cancelled against the drums. Everything else in the
# folder is passed through untouched.
DEFAULT_KICK_TARGETS: tuple[str, ...] = ("bass", "guitar", "piano", "other")


def reusable_stems(kick_targets: tuple[str, ...] = DEFAULT_KICK_TARGETS) -> frozenset[str]:
    """Name the stems ``refine_stem_folder`` reads as audio.

    Those are the only ones worth handing over in memory: the drums drive
    kick analysis and every cancellation, and each target is cancelled
    against them. The rest are byte-copied and never decoded, so preloading
    one only pins a full-length array for the whole stage.
    """
    return frozenset({"drums", *kick_targets})


def refine_stem_folder(
    input_dir: str | Path,
    output_dir: str | Path,
    kick_targets: tuple[str, ...] = DEFAULT_KICK_TARGETS,
    cfg: KickRefinementConfig | None = None,
    progress_callback: Callable[[int, int, str], None] | None = None,
    stage_callback: Callable[[str], None] | None = None,
    ready_callback: Callable[[str, Path], None] | None = None,
    preloaded: dict[str, tuple[np.ndarray, int]] | None = None,
) -> dict[str, KickRefinementStats]:
    """Copy all stems while reducing kick bleed in configured target stems.

    ``progress_callback(index, total, stem)`` fires at the START of each
    stem - index stems are done, ``stem`` is being worked on - and once
    more as ``(total, total, "")`` when everything is written, so a status
    line built from it always names work in progress. ``stage_callback``
    narrates the analysis that runs before the per-stem loop, which is the
    slow silent start of refinement.

    ``ready_callback(stem, path)`` fires at the END of each stem, once its
    output file is written and final, so a caller may hand that one stem to
    a consumer while the rest of the folder is still being refined.

    ``preloaded`` maps stem names to ``([channels, samples] float32 audio,
    sample_rate)`` already in memory - a same-process caller that just
    wrote ``input_dir`` (hybrid fusion) hands the arrays over so no stem is
    decoded twice. An entry is only trusted when its rate matches the
    folder rate; anything else falls back to decoding the file. Only the
    stems ``reusable_stems`` names are ever read from it, so passing more
    than those pins full-length audio nothing here will look at.
    """
    input_dir = Path(input_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    def stage(label: str) -> None:
        if stage_callback:
            stage_callback(label)

    def cached(stem: str) -> tuple[np.ndarray, int] | None:
        # pop, not get: each handed-over array is consumed exactly once, and
        # releasing it here keeps the whole fused set from staying pinned in
        # memory for the duration of refinement.
        if preloaded is not None:
            return preloaded.pop(stem, None)
        return None

    drum_path = find_stem_file(input_dir, "drums")
    if drum_path is None:
        raise FileNotFoundError("Could not find a drums stem in the input folder")

    stage("Loading the drums stem")

    drums_cached = cached("drums")
    if drums_cached is not None:
        drums, sr = drums_cached
    else:
        drums, sr = load_audio(drum_path)

    # Shared across every target stem; see refine_kick_bleed.
    kick_config = cfg or KickRefinementConfig()

    stage("Detecting kick events in the drums")
    kick_events = detect_kick_events(drums, sr=sr)

    stage("Building the kick cancellation reference")
    kick_reference = build_kick_reference(drums, kick_events, sr, kick_config)

    stats = {}

    available = [(stem, find_stem_file(input_dir, stem)) for stem in STEM_NAMES]
    available = [(stem, path) for stem, path in available if path is not None]

    total = max(1, len(available))

    def stem_audio(stem: str, path: Path) -> np.ndarray:
        entry = cached(stem)
        if entry is not None and entry[1] == sr:
            return entry[0]

        if path == drum_path:
            # The folder rate is the drums' native rate, so the array loaded
            # for kick analysis is exactly what this decode would produce.
            return drums

        audio, _ = load_audio(path, target_sr=sr)
        return audio

    for index, (stem, path) in enumerate(available):
        if progress_callback:
            progress_callback(index, total, stem)

        out_path = output_dir / path.name

        if stem in kick_targets:
            refined, stem_stats = refine_kick_bleed(
                drums=drums,
                target=stem_audio(stem, path),
                sr=sr,
                cfg=kick_config,
                events=kick_events,
                reference=kick_reference,
            )
            save_audio(out_path, refined, sr)
            stats[stem] = stem_stats

            # Otherwise this name holds a full-length copy until the next
            # target reassigns it, across that target's whole cancellation.
            del refined
        else:
            info = sf.info(str(path))
            if info.samplerate == sr and info.channels > 1:
                # Untouched stem at the folder rate: a byte copy is the
                # decode + float32 re-encode with the work removed.
                shutil.copyfile(path, out_path)
            else:
                # Mismatched rate must resample; mono is still promoted to
                # stereo by the decode path.
                save_audio(out_path, stem_audio(stem, path), sr)

        if ready_callback:
            # Below the write, never beside progress_callback at the top of
            # the iteration: this states the file is complete on disk.
            ready_callback(stem, out_path)

    if progress_callback:
        progress_callback(total, total, "")

    return stats
