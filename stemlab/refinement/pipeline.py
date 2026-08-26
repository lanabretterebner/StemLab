"""Apply conservative refinement to a complete six-stem output folder."""

from __future__ import annotations

from pathlib import Path
from typing import Callable

from ..audio import STEM_NAMES, find_stem_file, load_audio, save_audio
from .events import detect_kick_events
from .kick import (
    KickRefinementConfig,
    KickRefinementStats,
    build_kick_reference,
    refine_kick_bleed,
)


def refine_stem_folder(
    input_dir: str | Path,
    output_dir: str | Path,
    kick_targets: tuple[str, ...] = ("bass", "guitar", "piano", "other"),
    cfg: KickRefinementConfig | None = None,
    progress_callback: Callable[[int, int, str], None] | None = None,
    stage_callback: Callable[[str], None] | None = None,
) -> dict[str, KickRefinementStats]:
    """Copy all stems while reducing kick bleed in configured target stems.

    ``progress_callback(index, total, stem)`` fires at the START of each
    stem - index stems are done, ``stem`` is being worked on - and once
    more as ``(total, total, "")`` when everything is written, so a status
    line built from it always names work in progress. ``stage_callback``
    narrates the analysis that runs before the per-stem loop, which is the
    slow silent start of refinement.
    """
    input_dir = Path(input_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    def stage(label: str) -> None:
        if stage_callback:
            stage_callback(label)

    drum_path = find_stem_file(input_dir, "drums")
    if drum_path is None:
        raise FileNotFoundError("Could not find a drums stem in the input folder")

    stage("Loading the drums stem")
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

    for index, (stem, path) in enumerate(available):
        if progress_callback:
            progress_callback(index, total, stem)

        audio, _ = load_audio(path, target_sr=sr)
        out_path = output_dir / path.name

        if stem in kick_targets:
            refined, stem_stats = refine_kick_bleed(
                drums=drums,
                target=audio,
                sr=sr,
                cfg=kick_config,
                events=kick_events,
                reference=kick_reference,
            )
            save_audio(out_path, refined, sr)
            stats[stem] = stem_stats
        else:
            save_audio(out_path, audio, sr)

    if progress_callback:
        progress_callback(total, total, "")

    return stats
