"""File/UDP bridge between the JUCE app, Python pipeline, and Ableton Live."""

from __future__ import annotations

import argparse
import json
import os
import socket
import time
from datetime import datetime, timezone
from pathlib import Path

from .audio import STEM_NAMES, find_stem_file
from .pipeline import DEFAULT_ENGINE, ENGINE_CHOICES, separate
from .pretrained import DEFAULT_MODEL
from .runtime import CancellationToken, JobCancelled, configure_utf8_stdio

BRIDGE_HOST = "127.0.0.1"
BRIDGE_PORT = 39277
PROGRESS_FILE = "stemlab_progress.txt"


def write_progress(
    output_dir: str | Path,
    percent: float,
    stage: str,
) -> None:
    """Best-effort UI progress update.

    Progress reporting is cosmetic and must never be capable of aborting a
    separation job. The JUCE UI polls this file while the Python worker writes
    it. On Windows, a very short read/replace race can produce WinError 5 when
    os.replace() tries to replace a file another process momentarily has open.

    Use a per-process temporary file, retry the atomic replace briefly, and
    silently drop this one progress frame if Windows keeps the destination
    locked. The next progress callback will try again.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    target = output_dir / PROGRESS_FILE
    temp = output_dir / (f"{PROGRESS_FILE}.{os.getpid()}.tmp")

    payload = f"{max(0.0, min(100.0, float(percent))):.1f}\n{str(stage)}\n"

    try:
        temp.write_text(
            payload,
            encoding="utf-8",
        )
    except OSError:
        # A progress-file failure must never stop separation.
        return

    delays = (0.0, 0.01, 0.025, 0.05, 0.10, 0.20)

    for delay in delays:
        if delay:
            time.sleep(delay)

        try:
            os.replace(temp, target)
            return
        except PermissionError:
            # Expected transient Windows sharing violation while the JUCE
            # editor, Defender, Search Indexer, etc. has the target open.
            continue
        except OSError:
            # Progress remains non-critical for every other filesystem error.
            break

    # If every atomic-replace attempt lost the race, discard the temporary
    # update. A later callback will refresh the UI; the engine keeps running.
    try:
        temp.unlink(missing_ok=True)
    except OSError:
        pass


def manifest_audio_path(path: Path) -> str:
    """Return an absolute, JSON-safe audio path for Ableton's clip API."""
    # Live's create_audio_clip expects an absolute file path. Forward slashes
    # avoid backslash escaping problems when the JSON is read by Max JS.
    return str(path.resolve()).replace("\\", "/")


def build_manifest(
    *,
    final_dir: Path,
    input_path: Path,
    start_ppq: float,
    selected_stems: list[str],
    refined: bool,
    engine: str = DEFAULT_ENGINE,
) -> dict:
    """Build the stable JSON payload consumed by ``FI-STEMRemote``."""
    stems = []

    for stem in selected_stems:
        stem_file = find_stem_file(final_dir, stem)
        if stem_file is None:
            continue

        stems.append(
            {
                "name": stem,
                "label": f"FI-STEM - {stem.title()}",
                "path": manifest_audio_path(stem_file),
            }
        )

    return {
        "protocol": "stemlab-ableton-bridge",
        "version": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source": manifest_audio_path(input_path),
        "capture_start_ppq": max(0.0, float(start_ppq)),
        "refined": bool(refined),
        "engine": str(engine),
        "stems": stems,
    }


def write_manifest(path: Path, data: dict) -> None:
    """Write a readable UTF-8 manifest, creating its parent directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )


def encode_path_for_udp(path: Path) -> str:
    """Encode a path as one whitespace-safe hexadecimal UDP token."""
    # UDP messages entering Max are atomized on whitespace. Hex gives us a
    # transport-safe single atom regardless of spaces in Windows paths.
    return str(path.resolve()).encode("utf-8").hex().upper()


def notify_ableton(
    manifest_path: Path,
    host: str = BRIDGE_HOST,
    port: int = BRIDGE_PORT,
) -> None:
    """Tell the local Ableton Remote Script that a manifest is ready."""
    encoded = encode_path_for_udp(manifest_path)
    payload = f"stemlab_ready {encoded}".encode("ascii")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(payload, (host, port))


def run_plugin_job(
    *,
    input_path: Path,
    output_dir: Path,
    start_ppq: float,
    selected_stems: list[str],
    model: str = DEFAULT_MODEL,
    device: str = "cuda",
    engine: str = DEFAULT_ENGINE,
    refine: bool = True,
    notify: bool = True,
    cancel_file: Path | None = None,
) -> Path:
    """Separate audio, write an Ableton manifest, and optionally notify Live."""
    selected = []
    for stem in selected_stems:
        normalized = stem.lower().strip()
        if normalized not in STEM_NAMES:
            raise ValueError(f"Unknown stem: {stem}")
        if normalized not in selected:
            selected.append(normalized)

    if not selected:
        raise ValueError("At least one stem must be selected")

    cancellation = CancellationToken(cancel_file)
    cancellation.raise_if_cancelled()
    write_progress(output_dir, 3.0, "Starting")

    def on_progress(percent: float, stage: str):
        write_progress(output_dir, percent, stage)

    result = separate(
        input_path=input_path,
        output_dir=output_dir,
        model=model,
        device=device,
        engine=engine,
        refine=refine,
        progress_callback=on_progress,
        cancellation=cancellation,
    )

    write_progress(output_dir, 96.0, "Building stem list")
    final_dir = result.final_dir
    manifest = build_manifest(
        final_dir=final_dir,
        input_path=input_path,
        start_ppq=start_ppq,
        selected_stems=selected,
        refined=refine,
        engine=result.engine,
    )

    if not manifest["stems"]:
        raise RuntimeError(
            f"Separation finished, but none of the selected stems were found in {final_dir}"
        )

    manifest_path = output_dir / "stemlab_ableton_manifest.json"
    write_manifest(manifest_path, manifest)

    write_progress(output_dir, 99.0, "Writing files")
    print(f"Manifest: {manifest_path}", flush=True)
    print(f"Arrangement start: {manifest['capture_start_ppq']:.6f} beats", flush=True)
    print("Export stems:", ", ".join(x["name"] for x in manifest["stems"]), flush=True)

    if notify:
        notify_ableton(manifest_path)
        print(f"Notified Ableton bridge on UDP {BRIDGE_PORT}", flush=True)

    write_progress(output_dir, 100.0, "Done")
    return manifest_path


def main() -> None:
    """CLI entry used by ``stemlab-plugin-job`` and the JUCE process bridge."""
    configure_utf8_stdio()

    parser = argparse.ArgumentParser(
        description="Run a FI-STEM VST capture through the separator and notify Ableton."
    )
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--start-ppq", type=float, required=True)
    parser.add_argument(
        "--stems",
        nargs="+",
        choices=STEM_NAMES,
        required=True,
    )
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument(
        "--engine",
        choices=ENGINE_CHOICES,
        default=DEFAULT_ENGINE,
    )
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--no-refine", action="store_true")
    parser.add_argument("--no-notify", action="store_true")
    parser.add_argument("--cancel-file")
    args = parser.parse_args()

    run_plugin_job(
        input_path=Path(args.input),
        output_dir=Path(args.output),
        start_ppq=args.start_ppq,
        selected_stems=args.stems,
        model=args.model,
        device=args.device,
        engine=args.engine,
        refine=not args.no_refine,
        notify=not args.no_notify,
        cancel_file=Path(args.cancel_file) if args.cancel_file else None,
    )


if __name__ == "__main__":
    try:
        main()
    except JobCancelled:
        print("STEMLAB_CANCELLED", flush=True)
        raise SystemExit(130) from None
    except Exception as exc:
        print(f"STEMLAB_ERROR {exc}", flush=True)
        raise
