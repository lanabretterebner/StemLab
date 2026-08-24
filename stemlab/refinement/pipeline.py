from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from ..audio import STEM_NAMES, load_audio, save_audio
from .kick import KickRefinementConfig, refine_kick_bleed


@dataclass
class FolderRefinementResult:
    output_files: list[Path]
    stats: dict[str, object]


def _find_stem(folder: Path, stem: str) -> Path | None:
    # Prefer exact suffixes but tolerate the backend's naming conventions.
    candidates = sorted(
        [
            p for p in folder.rglob("*.wav")
            if stem.lower() in p.stem.lower()
        ],
        key=lambda p: (len(p.name), p.name.lower()),
    )
    return candidates[0] if candidates else None


def refine_stem_folder(
    input_dir: str | Path,
    output_dir: str | Path,
    kick_targets: tuple[str, ...] = ("bass", "guitar", "piano", "other"),
    cfg: KickRefinementConfig | None = None,
    progress_callback: Callable[[int, int, str], None] | None = None,
) -> FolderRefinementResult:
    input_dir = Path(input_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    drum_path = _find_stem(input_dir, "drums")
    if drum_path is None:
        raise FileNotFoundError("Could not find a drums stem in the input folder")

    drums, sr = load_audio(drum_path)
    outputs = []
    stats = {}

    available = [
        (stem, _find_stem(input_dir, stem))
        for stem in STEM_NAMES
    ]
    available = [
        (stem, path)
        for stem, path in available
        if path is not None
    ]

    total = max(1, len(available))

    for index, (stem, path) in enumerate(available, start=1):
        audio, stem_sr = load_audio(path, target_sr=sr)
        out_path = output_dir / path.name

        if stem in kick_targets:
            refined, stem_stats = refine_kick_bleed(
                drums=drums,
                target=audio,
                sr=sr,
                cfg=cfg,
            )
            save_audio(out_path, refined, sr)
            stats[stem] = stem_stats
        else:
            save_audio(out_path, audio, sr)

        outputs.append(out_path)

        if progress_callback:
            progress_callback(index, total, stem)

    return FolderRefinementResult(
        output_files=outputs,
        stats=stats,
    )
