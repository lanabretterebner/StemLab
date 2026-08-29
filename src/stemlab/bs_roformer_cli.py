"""Relocatable launcher for the upstream ``bs-roformer-infer`` console entry.

Pip-generated Windows launchers embed an absolute ``python.exe`` path. StemLab
invokes this module with the active interpreter instead, then resolves the
upstream entry point from installed package metadata.

Loading the entry point also imports the upstream model module, which is what
makes this the one place StemLab can arm compiled inference over third-party
code it does not own.
"""

from __future__ import annotations

import sys
from importlib.metadata import entry_points


def main() -> int:
    """Run the installed ``bs-roformer-infer`` console script in-process."""
    matches = [ep for ep in entry_points(group="console_scripts") if ep.name == "bs-roformer-infer"]
    if not matches:
        raise RuntimeError("bs-roformer-infer is not installed in the StemLab runtime.")

    # Resolving the entry point imports its module, so the separator classes
    # exist by the time compiling is armed - but the model itself has not run.
    entry = matches[0].load()

    from .compile_support import arm_torch_compile

    # Never let opting into compiled inference be the reason a separation
    # fails: the gate logs and returns, and every wrapped forward falls back.
    arm_torch_compile(lambda message: print(message, file=sys.stderr, flush=True))

    result = entry()
    return int(result) if isinstance(result, int) else 0


if __name__ == "__main__":
    raise SystemExit(main())
