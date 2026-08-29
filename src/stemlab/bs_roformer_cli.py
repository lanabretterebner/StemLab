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

from .console_entry import load_console_entry


def main() -> int:
    """Run the installed ``bs-roformer-infer`` console script in-process."""
    # Loading the entry point imports its module, so the separator classes
    # exist by the time compiling is armed - but the model itself has not run.
    entry = load_console_entry("bs-roformer-infer")

    from .compile_support import arm_torch_compile

    # Never let opting into compiled inference be the reason a separation
    # fails: the gate logs and returns, and every wrapped forward falls back.
    arm_torch_compile(lambda message: print(message, file=sys.stderr, flush=True))

    result = entry()
    return int(result) if isinstance(result, int) else 0


if __name__ == "__main__":
    raise SystemExit(main())
