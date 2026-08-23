from __future__ import annotations

"""Relocatable launcher for the upstream bs-roformer-infer console entry point.

Pip-generated Windows console launchers normally contain an absolute path to
python.exe. That is fine inside a venv, but it breaks when a portable StemLab
folder is moved to another machine/path. StemLab calls this module with the
embedded interpreter instead, then resolves the upstream console entry point
from installed package metadata at runtime.
"""

import sys
from importlib.metadata import entry_points


def main() -> int:
    matches = [
        ep
        for ep in entry_points(group="console_scripts")
        if ep.name == "bs-roformer-infer"
    ]

    if not matches:
        raise RuntimeError(
            "bs-roformer-infer is not installed in the StemLab runtime."
        )

    result = matches[0].load()()
    return int(result) if isinstance(result, int) else 0


if __name__ == "__main__":
    raise SystemExit(main())
