"""Stage checksum-pinned release models into a portable StemLab Engine."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import urllib.request
from pathlib import Path


def destination_for(engine: Path, relative: str) -> Path:
    """Resolve a manifest path while preventing writes outside ``engine``."""
    root = engine.expanduser().resolve()
    destination = (root / "Models" / relative).resolve()
    if destination != root and root not in destination.parents:
        raise ValueError(f"Model destination escapes Engine: {relative}")
    return destination


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _verify(path: Path, model: dict[str, object]) -> None:
    if not path.is_file():
        raise FileNotFoundError(path)
    expected_size = model.get("bytes")
    if expected_size is not None and path.stat().st_size != int(expected_size):
        raise RuntimeError(f"Wrong size for {path}")
    expected_hash = model.get("sha256")
    if expected_hash and _sha256(path).lower() != str(expected_hash).lower():
        raise RuntimeError(f"SHA-256 mismatch for {path}")


def _find_verified_source(
    model: dict[str, object], destination: Path, source_roots: list[Path]
) -> Path | None:
    relative = Path(str(model["file"]))
    for root_value in source_roots:
        root = root_value.expanduser().resolve()
        candidates = [root / relative.name, root / relative]
        if "ModelCaches" in relative.parts:
            index = relative.parts.index("ModelCaches")
            candidates.append(root.joinpath(*relative.parts[index + 1 :]))
        if "Recursive" in relative.parts:
            index = relative.parts.index("Recursive")
            candidates.append(root.joinpath(*relative.parts[index:]))
        if root.is_dir():
            candidates.extend(root.rglob(relative.name))
        for candidate in candidates:
            candidate = candidate.resolve()
            if candidate == destination or not candidate.is_file():
                continue
            try:
                _verify(candidate, model)
            except RuntimeError:
                continue
            return candidate
    return None


def _download(model: dict[str, object], destination: Path) -> None:
    url = str(model.get("url") or "")
    if not url:
        raise FileNotFoundError(f"No verified local source or URL for {model['file']}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".partial")
    partial.unlink(missing_ok=True)
    try:
        print(f"Downloading {model['purpose']}: {destination.name}", flush=True)
        urllib.request.urlretrieve(url, partial)
        _verify(partial, model)
        partial.replace(destination)
    finally:
        partial.unlink(missing_ok=True)


def stage_models(
    manifest: Path,
    engine: Path,
    *,
    source_roots: list[Path] | None = None,
    download_missing: bool = False,
) -> list[Path]:
    """Copy/download every manifest entry and validate pinned assets."""
    document = json.loads(manifest.read_text(encoding="utf-8"))
    if int(document.get("schema", 0)) != 1:
        raise ValueError("Unsupported release-model manifest schema")
    roots = list(source_roots or [])
    staged: list[Path] = []

    for model in document.get("models", []):
        destination = destination_for(engine, str(model["file"]))
        try:
            _verify(destination, model)
        except (FileNotFoundError, RuntimeError) as existing_error:
            source = _find_verified_source(model, destination, roots)
            if source is not None:
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
            elif download_missing:
                _download(model, destination)
            else:
                raise existing_error
        _verify(destination, model)
        staged.append(destination)
        print(f"Verified: {destination}", flush=True)
    return staged


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, action="append", default=[])
    parser.add_argument("--download-missing", action="store_true")
    args = parser.parse_args()
    stage_models(
        args.manifest,
        args.engine,
        source_roots=args.source_root,
        download_missing=args.download_missing,
    )


if __name__ == "__main__":
    main()
