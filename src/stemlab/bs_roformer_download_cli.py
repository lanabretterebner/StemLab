"""Relocatable launcher for upstream's ``bs-roformer-download`` console entry.

The sibling of ``bs_roformer_cli`` for fetching weights rather than running
them, and here for the same reason: a shipped Engine's pip launchers point at
an interpreter that only existed on the build machine. Unlike the inference
launcher this one arms nothing - a download has no forward pass to compile,
and importing the model module to fetch a file would only be one more way for
the fetch to fail.
"""

from __future__ import annotations

from .console_entry import run_console_entry


def main() -> int:
    """Run the installed ``bs-roformer-download`` console script in-process."""
    return run_console_entry("bs-roformer-download")


if __name__ == "__main__":
    raise SystemExit(main())
