"""Command-line bridge used by the JUCE plugin for adaptive stem jobs."""

from __future__ import annotations

import argparse
from pathlib import Path

from .recursive import default_model_dir, run_recursive
from .runtime import CancellationToken, JobCancelled, configure_utf8_stdio


def main() -> None:
    """CLI entry: ``stemlab-recursive-job``."""
    configure_utf8_stdio()

    parser = argparse.ArgumentParser(description="Run a FI-STEM Adaptive Stem Tree job.")
    parser.add_argument(
        "--operation",
        choices=("vocals", "drums", "deverb", "lead", "adaptive"),
        required=True,
    )
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--parent-id", required=True)
    parser.add_argument("--root-stem", required=True)
    parser.add_argument("--category", default="unknown")
    parser.add_argument("--depth", type=int, default=1)
    parser.add_argument("--model-dir", default=str(default_model_dir()))
    parser.add_argument("--cancel-file")
    args = parser.parse_args()

    cancellation = CancellationToken(Path(args.cancel_file) if args.cancel_file else None)

    def progress(percent: float, stage: str) -> None:
        cancellation.raise_if_cancelled()
        bounded = int(max(0, min(100, percent)))
        print(f"STEMLAB_PROGRESS {bounded} {stage}", flush=True)

    progress(1.0, "Starting adaptive stem split")
    manifest = run_recursive(
        operation=args.operation,
        input_path=Path(args.input),
        output_dir=Path(args.output),
        parent_id=args.parent_id,
        root_stem=args.root_stem,
        category=args.category,
        depth=args.depth,
        model_dir=Path(args.model_dir),
        progress=progress,
    )
    progress(100.0, "Adaptive stem split complete")
    print(f"STEMLAB_RECURSIVE_MANIFEST {manifest}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except JobCancelled:
        print("STEMLAB_CANCELLED", flush=True)
        raise SystemExit(130) from None
    except Exception as exc:
        print(f"STEMLAB_ERROR {exc}", flush=True)
        raise
