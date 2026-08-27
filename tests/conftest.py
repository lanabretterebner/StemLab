"""Shared test isolation.

Every test gets its own STEMLAB_ANALYSIS_HOME so nothing that resolves the
managed analysis directory - the sqlite caches, the device-probe cache -
can leak state between tests or touch the developer's real ~/.stemlab.
The device probe made this load-bearing: in an environment where torch is
installed (CI), one test faking "cuda unavailable" would otherwise write a
cached answer a later test faking "cuda available" reads back as a hit.
"""

import pytest


@pytest.fixture(autouse=True)
def _isolated_analysis_home(tmp_path_factory, monkeypatch):
    monkeypatch.setenv(
        "STEMLAB_ANALYSIS_HOME", str(tmp_path_factory.mktemp("analysis-home"))
    )
