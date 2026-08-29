"""Call an installed console script without executing pip's launcher.

Pip bakes an absolute interpreter path into every console script it writes -
a ``#!`` line on Unix, an embedded path on Windows. The Engine is built in one
directory and shipped to another, so on a user's machine those paths name an
interpreter that is not there.

The failure that causes is worth spelling out, because it reads as nonsense:
exec'ing a script whose shebang cannot be resolved reports ENOENT against the
*script*, not the missing interpreter, so a launcher sitting plainly on disk
comes back as::

    FileNotFoundError: [Errno 2] No such file or directory:
        '.../Engine/bin/bs-roformer-download'

Checking that the file exists before running it - which StemLab did - cannot
catch this, because the file does exist.

So the launcher is never executed. Pip records the entry point in package
metadata and generates the launcher from it; reading the same metadata and
calling the function under whatever interpreter is already running gets the
identical behaviour with nothing absolute in the path.
"""

from __future__ import annotations

from importlib.metadata import entry_points
from typing import Any, Callable


def load_console_entry(name: str) -> Callable[[], Any]:
    """Return the function pip would have generated a launcher for.

    Loading resolves and imports the owning module, which callers that need to
    patch that module before it runs depend on happening here.
    """
    matches = [entry for entry in entry_points(group="console_scripts") if entry.name == name]

    if not matches:
        raise RuntimeError(f"{name} is not installed in the StemLab runtime.")

    return matches[0].load()


def run_console_entry(name: str) -> int:
    """Run an installed console script in this process, returning its status.

    ``sys.argv`` is left alone: an entry point reads its arguments from
    ``sys.argv[1:]``, and ``python -m stemlab.<launcher> ARGS`` puts them in
    exactly the places the real launcher would have.
    """
    result = load_console_entry(name)()

    return int(result) if isinstance(result, int) else 0
