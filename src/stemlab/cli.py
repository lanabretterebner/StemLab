"""Top-level CLI: separate, refine an existing stem folder, list models."""

from __future__ import annotations

import argparse
import subprocess

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
    parser.add_argument("--device", default="auto")
    parser.add_argument(
        "--no-refine",
        action="store_true",
        help="Render only pretrained baseline stems",
    )
    parser.add_argument(
        "--normalize-fused-stems",
        action="store_true",
        help=(
            "Scale each hybrid-fused stem so its own peak sits at 0.999. Off "
            "by default: the factor comes from one stem alone, so the six "
            "stop summing back to the source and their balance shifts."
        ),
    )
    args = parser.parse_args()

    result = separate(
        input_path=args.input,
        output_dir=args.output,
        model=args.model,
        device=args.device,
        engine=args.engine,
        refine=not args.no_refine,
        normalize_fused=args.normalize_fused_stems,
    )
    print(f"final: {result.final_dir}")


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
    from .model_manager import bs_roformer_download_command

    # Not the pip launcher next to the interpreter: in a shipped Engine its
    # shebang names a build machine's Python and exec'ing it fails.
    subprocess.run(bs_roformer_download_command("--list-models"), check=True)


if __name__ == "__main__":
    separate_main()
