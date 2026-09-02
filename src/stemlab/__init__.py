"""StemLab: multi-engine stem separation with optional adaptive refinement."""

import re as _re
from pathlib import Path as _Path

_version: str | None = None


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

    # Imported here rather than at module scope: importlib.metadata pulls in
    # the whole packaging-metadata machinery and costs about 33 ms, which
    # every plugin-launched job used to pay just for importing the package.
    # Nothing in the engine reads __version__, so that work is deferred until
    # somebody actually asks for it.
    from importlib.metadata import PackageNotFoundError, version as _distribution_version

    try:
        return _distribution_version("stemlab-open")
    except PackageNotFoundError:  # an uninstalled checkout without pyproject
        return "0.0.0"


def __getattr__(name: str) -> str:
    # PEP 562 module attribute hook: ``stemlab.__version__`` still reads as a
    # plain string, but the pyproject read (or the metadata lookup behind it)
    # only happens on first access, not on every ``import stemlab``.
    if name == "__version__":
        global _version

        if _version is None:
            _version = _read_version()

        return _version

    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
