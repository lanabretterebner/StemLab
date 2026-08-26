"""StemLab: multi-engine stem separation with optional adaptive refinement."""

import re as _re
from importlib.metadata import PackageNotFoundError, version as _distribution_version
from pathlib import Path as _Path


def _read_version() -> str:
    # pyproject.toml is the single source of the StemLab version. A source
    # checkout (editable installs included) reads it live so a bump lands
    # without a reinstall; real installs carry it in distribution metadata.
    pyproject = _Path(__file__).resolve().parents[2] / "pyproject.toml"

    if pyproject.is_file():
        match = _re.search(
            r'^version = "([^"]+)"', pyproject.read_text(encoding="utf-8"), _re.MULTILINE
        )

        if match:
            return match.group(1)

    try:
        return _distribution_version("stemlab-open")
    except PackageNotFoundError:  # an uninstalled checkout without pyproject
        return "0.0.0"


__version__ = _read_version()
