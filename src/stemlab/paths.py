"""Where the Engine keeps its own files.

The mirror of the plugin's StemLabPaths, for the two directories the Python
side owns: the analysis cache and the recursive model weights. Both lived in
``~/.stemlab`` - a dotfile directory in the home folder that follows no
platform's convention, tells nothing apart, and cannot be reasoned about by
an uninstaller or a disk-cleaning tool.

They are split here by what they actually are. The analysis cache is derived
data: everything in it can be recomputed from the audio it describes, so it
belongs in the cache directory, where a system that needs the space may
delete it. The model weights are not - they are gigabytes fetched over a
slow network, and a cache sweep taking them would be a very long surprise -
so they go in the data directory, which nothing sweeps.

Nothing the user made is here. Stems, captures and recordings are the
plugin's business and live under their music folder; see StemLabPaths.
"""

from __future__ import annotations

import contextlib
import os
import shutil
from pathlib import Path

# The old home for both. Kept only so what is in it can be moved out.
LEGACY_HOME = ".stemlab"


def _windows_local() -> Path | None:
    local = os.environ.get("LOCALAPPDATA")
    return Path(local) if local else None


def _xdg(variable: str, fallback: str) -> Path:
    """An XDG base directory, honouring the spec's "must be absolute" rule."""
    value = os.environ.get(variable)

    if value and Path(value).is_absolute():
        return Path(value)

    return Path.home() / fallback


_migrated = False


def _ensure_migrated() -> None:
    """Move what is still in ``~/.stemlab`` before anything looks there.

    Lazy and once per process rather than at import: an import must not
    touch the disk, and the Engine is a short-lived subprocess, so the first
    caller that actually wants a directory is exactly when this needs to
    have happened. Two Engines racing is harmless - the mkdir and the
    existence guard mean the loser finds the destination already there and
    leaves it alone.
    """
    global _migrated

    if _migrated:
        return

    _migrated = True
    migrate_legacy_home()


def analysis_dir() -> Path:
    """Beat, key and MIDI analysis results. Regenerable, so: cache."""
    override = os.environ.get("STEMLAB_ANALYSIS_HOME")
    if override:
        return Path(override).expanduser().resolve()

    local = _windows_local()
    if local:
        # Unchanged. The Windows layout is a compatibility promise the
        # plugin already keeps, and analysis is cheap to lose but not free.
        return local / "StemLab" / "Analysis"

    _ensure_migrated()

    return _xdg("XDG_CACHE_HOME", ".cache") / "StemLab" / "analysis"


def recursive_models_dir() -> Path:
    """Downloaded weights for the recursive splits. Expensive, so: data."""
    packaged = os.environ.get("STEMLAB_RECURSIVE_MODEL_DIR")
    if packaged:
        return Path(packaged)

    local = _windows_local()
    if local:
        return local / "StemLab" / "Models" / "Recursive"

    _ensure_migrated()

    return _xdg("XDG_DATA_HOME", ".local/share") / "StemLab" / "models" / "recursive"


def _move_legacy(source: Path, destination: Path) -> bool:
    """Move ``source`` to ``destination`` if only the old one is there."""
    if not source.is_dir() or destination.exists():
        return False

    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(source), str(destination))
    except OSError:
        # Never fatal. Failing to move a cache costs a re-analysis; failing
        # to move weights costs a re-download. Both are recoverable, and
        # neither is worth refusing to run over.
        return False

    return True


def migrate_legacy_home() -> list[str]:
    """Move anything still in ``~/.stemlab`` to where it belongs now.

    Returns what moved, for the caller to log. Runs at most once in effect:
    a destination that already exists is left alone, so a half-finished
    migration resumes rather than overwriting the new copy with the old.
    """
    try:
        legacy = Path.home() / LEGACY_HOME
    except (OSError, RuntimeError):
        # No resolvable home. Nothing to migrate from.
        return []

    if not legacy.is_dir():
        return []

    moved: list[str] = []

    # Resolved directly rather than through the getters: those call back
    # into the migration, and this is the migration.
    analysis = _xdg("XDG_CACHE_HOME", ".cache") / "StemLab" / "analysis"
    models = _xdg("XDG_DATA_HOME", ".local/share") / "StemLab" / "models" / "recursive"

    if _move_legacy(legacy / "analysis", analysis):
        moved.append(f"analysis cache -> {analysis}")

    if _move_legacy(legacy / "models" / "recursive", models):
        moved.append(f"recursive models -> {models}")

    # Only if it is now empty, and only the directories we know we made.
    # Anything else in there is somebody else's and stays.
    #
    # Each is tried on its own: models/ is missing whenever only the
    # analysis cache was ever written, and that must not be the reason the
    # dot directory itself survives. Innermost first, so removing one can
    # be what empties the next.
    for directory in (legacy / "models", legacy):
        with contextlib.suppress(OSError):
            directory.rmdir()

    return moved
