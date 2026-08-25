"""Top-level CLI: separate, refine an existing stem folder, list models."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from .pipeline import DEFAULT_ENGINE, ENGINE_CHOICES, separate
from .pretrained import DEFAULT_MODEL
from .refinement.pipeline import refine_stem_folder


def separate_main() -> None:
    """CLI entry: ``stemlab-separate``."""
    parser = argparse.ArgumentParser(
        description="StemLab multi-engine separation + adaptive refinement"
    )
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--engine", choices=ENGINE_CHOICES, default=DEFAULT_ENGINE)
    parser.add_argument("--device", default="cuda")
    parser.add_argument(
        "--no-refine",
        action="store_true",
        help="Render only pretrained baseline stems",
    )
    args = parser.parse_args()

    result = separate(
        input_path=args.input,
        output_dir=args.output,
        model=args.model,
        device=args.device,
        engine=args.engine,
        refine=not args.no_refine,
    )
    print(f"baseline: {result.baseline_dir}")
    print(f"final:    {result.final_dir}")


def refine_main() -> None:
    """CLI entry: ``stemlab-refine``."""
    parser = argparse.ArgumentParser(
        description="Run StemLab adaptive refinement on existing stems"
    )
    parser.add_argument("--stems", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    stats_by_stem = refine_stem_folder(input_dir=args.stems, output_dir=args.output)
    for stem, stats in stats_by_stem.items():
        print(
            f"{stem}: "
            f"events={stats.events_detected} "
            f"attempted={stats.cancellations_attempted} "
            f"applied={stats.cancellations_applied} "
            f"reject_event={stats.rejected_event_confidence} "
            f"reject_match={stats.rejected_match_confidence} "
            f"mean_conf={stats.mean_confidence:.3f}"
        )


def models_main() -> None:
    """CLI entry: ``stemlab-models`` — list installed RoFormer models."""
    python_dir = Path(sys.executable).resolve().parent

    local = next(
        (
            candidate
            for candidate in (
                python_dir / "bs-roformer-download.exe",
                python_dir / "bs-roformer-download",
            )
            if candidate.exists()
        ),
        None,
    )

    exe = str(local) if local is not None else shutil.which("bs-roformer-download")
    if exe is None:
        raise SystemExit(
            "bs-roformer-download was not found. Install StemLab with: python -m pip install -e ."
        )
    subprocess.run([exe, "--list-models"], check=True)


if __name__ == "__main__":
    separate_main()
