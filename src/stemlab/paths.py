"""Where the Engine keeps its own files.

The mirror of the plugin's StemLabPaths, for the two directories the Python
side owns: the analysis cache and the recursive model weights. Both lived in
``~/.stemlab`` - a dotfile directory in the home folder that followed no
platform's convention, told nothing apart, and could not be reasoned about
by an uninstaller or a disk-cleaning tool. Nothing reads it any more.

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

import os
from pathlib import Path

def _windows_local() -> Path | None:
    local = os.environ.get("LOCALAPPDATA")
    return Path(local) if local else None


def _xdg(variable: str, fallback: str) -> Path:
    """An XDG base directory, honouring the spec's "must be absolute" rule."""
    value = os.environ.get(variable)

    if value and Path(value).is_absolute():
        return Path(value)

    return Path.home() / fallback


def analysis_dir() -> Path:
    """Beat, key and MIDI analysis results. Regenerable, so: cache."""
    override = os.environ.get("STEMLAB_ANALYSIS_HOME")
    if override:
        return Path(override).expanduser().resolve()

    local = _windows_local()
    if local:
        # Windows has no XDG split; %LOCALAPPDATA% is where both belong,
        # and it is where the plugin's own data directory now points too.
        return local / "StemLab" / "Analysis"

    return _xdg("XDG_CACHE_HOME", ".cache") / "StemLab" / "analysis"


def recursive_models_dir() -> Path:
    """Downloaded weights for the recursive splits. Expensive, so: data."""
    packaged = os.environ.get("STEMLAB_RECURSIVE_MODEL_DIR")
    if packaged:
        # expanduser to match model_manager's mirror of this lookup: a "~/..."
        # value worked there and silently made a literal "~" directory here.
        return Path(packaged).expanduser()

    local = _windows_local()
    if local:
        return local / "StemLab" / "Models" / "Recursive"

    return _xdg("XDG_DATA_HOME", ".local/share") / "StemLab" / "models" / "recursive"
