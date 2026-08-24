from __future__ import annotations

import argparse
import json
import os
import time
import socket
import sys
from datetime import datetime, timezone
from pathlib import Path

from .pipeline import separate, DEFAULT_ENGINE, ENGINE_CHOICES
from .pretrained import DEFAULT_MODEL


STEM_NAMES = ("vocals", "drums", "bass", "guitar", "piano", "other")
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

    target = output_dir / "stemlab_progress.txt"
    temp = output_dir / (
        f"stemlab_progress.{os.getpid()}.tmp"
    )

    payload = (
        f"{max(0.0, min(100.0, float(percent))):.1f}\n"
        f"{str(stage)}\n"
    )

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


def find_stem_file(folder: Path, stem: str) -> Path | None:
    candidates = [
        p for p in folder.rglob("*")
        if p.is_file()
        and p.suffix.lower() in {".wav", ".flac"}
        and stem.lower() in p.stem.lower()
    ]
    if not candidates:
        return None

    # Prefer shorter names because they tend to be direct stem outputs rather
    # than duplicated/debug variants.
    return sorted(candidates, key=lambda p: (len(p.name), p.name.lower()))[0]


def manifest_audio_path(path: Path) -> str:
    # Live's create_audio_clip expects an absolute file path. Forward slashes
    # avoid backslash escaping problems when the JSON is read by Max JS.
    return str(path.resolve()).replace("\\", "/")


def build_manifest(
    *,
    output_dir: Path,
    final_dir: Path,
    input_path: Path,
    start_ppq: float,
    selected_stems: list[str],
    refined: bool,
    engine: str = DEFAULT_ENGINE,
) -> dict:
    stems = []

    for stem in selected_stems:
        stem_file = find_stem_file(final_dir, stem)
        if stem_file is None:
            continue

        stems.append(
            {
                "name": stem,
                "label": f"StemLab - {stem.title()}",
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
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )


def encode_path_for_udp(path: Path) -> str:
    # UDP messages entering Max are atomized on whitespace. Hex gives us a
    # transport-safe single atom regardless of spaces in Windows paths.
    return str(path.resolve()).encode("utf-8").hex().upper()


def notify_ableton(
    manifest_path: Path,
    host: str = BRIDGE_HOST,
    port: int = BRIDGE_PORT,
) -> None:
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
) -> Path:
    selected = []
    for stem in selected_stems:
        normalized = stem.lower().strip()
        if normalized not in STEM_NAMES:
            raise ValueError(f"Unknown stem: {stem}")
        if normalized not in selected:
            selected.append(normalized)

    if not selected:
        raise ValueError("At least one stem must be selected")

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
    )

    write_progress(output_dir, 96.0, "Building stem list")
    final_dir = result.final_dir
    manifest = build_manifest(
        output_dir=output_dir,
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


def main():
    # The separation backends re-invoke sys.executable for Demucs and
    # BS-RoFormer. When this process was launched without user site-packages
    # (the plugin passes -s to the self-contained Engine), its children must
    # inherit that isolation - but a process that IS relying on user site
    # (e.g. a "pip install --user" development setup) must not lose it.
    import site

    if not getattr(site, "ENABLE_USER_SITE", True):
        os.environ.setdefault("PYTHONNOUSERSITE", "1")

    try:
        # JUCE's ChildProcess reader interprets our output as UTF-8, so make
        # that contract explicit instead of inheriting Windows cp1252.
        sys.stdout.reconfigure(
            encoding="utf-8",
            errors="replace",
            line_buffering=True,
        )
        sys.stderr.reconfigure(
            encoding="utf-8",
            errors="replace",
            line_buffering=True,
        )
    except Exception:
        pass

    parser = argparse.ArgumentParser(
        description="Run a StemLab VST capture through the separator and notify Ableton."
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
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"STEMLAB_ERROR {exc}", flush=True)
        raise
