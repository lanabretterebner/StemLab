"""Entry point so the harness runs as `python -m stemlab.regression`.

Invoking the submodule directly imports it twice - once through the package's
__init__ and once as __main__ - which Python warns about and which would give
the module two sets of state. This runs it once.
"""

from __future__ import annotations

import sys

from .compare import main

if __name__ == "__main__":
    sys.exit(main())
