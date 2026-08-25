"""Relocatable launcher for the upstream ``bs-roformer-infer`` console entry.

Pip-generated Windows launchers embed an absolute ``python.exe`` path. FI-STEM
invokes this module with the active interpreter instead, then resolves the
upstream entry point from installed package metadata.
"""

from __future__ import annotations

from importlib.metadata import entry_points


def main() -> int:
    """Run the installed ``bs-roformer-infer`` console script in-process."""
    matches = [ep for ep in entry_points(group="console_scripts") if ep.name == "bs-roformer-infer"]
    if not matches:
        raise RuntimeError("bs-roformer-infer is not installed in the FI-STEM runtime.")
    result = matches[0].load()()
    return int(result) if isinstance(result, int) else 0


if __name__ == "__main__":
    raise SystemExit(main())
