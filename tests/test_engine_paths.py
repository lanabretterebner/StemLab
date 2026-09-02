"""The Engine's own directories.

Everything the Engine keeps for itself used to sit in ~/.stemlab: a dotfile
directory following no platform convention, mixing a throwaway analysis
cache with gigabytes of downloaded model weights, and telling an uninstaller
or a disk cleaner nothing about which was which. Nothing reads it any more.

They are split by what they are. Analysis is derived from audio and can be
recomputed, so it goes where a system may reclaim it. Weights cannot be
recomputed - only re-downloaded, slowly - so they go where nothing sweeps.
"""

from __future__ import annotations

from pathlib import Path

import pytest

import stemlab.paths as paths


@pytest.fixture(autouse=True)
def _isolated_home(tmp_path, monkeypatch):
    """A home of our own, and no Windows or override branch in the way."""
    monkeypatch.setattr(Path, "home", staticmethod(lambda: tmp_path))
    monkeypatch.delenv("LOCALAPPDATA", raising=False)
    monkeypatch.delenv("STEMLAB_ANALYSIS_HOME", raising=False)
    monkeypatch.delenv("STEMLAB_RECURSIVE_MODEL_DIR", raising=False)
    monkeypatch.delenv("XDG_CACHE_HOME", raising=False)
    monkeypatch.delenv("XDG_DATA_HOME", raising=False)
    return tmp_path


# --- where things land ----------------------------------------------------


def test_analysis_is_cache_and_models_are_data(_isolated_home):
    """The split that ~/.stemlab could not express."""
    assert paths.analysis_dir() == _isolated_home / ".cache" / "StemLab" / "analysis"
    assert (
        paths.recursive_models_dir()
        == _isolated_home / ".local" / "share" / "StemLab" / "models" / "recursive"
    )


def test_nothing_resolves_into_the_old_dot_directory(_isolated_home):
    for directory in (paths.analysis_dir(), paths.recursive_models_dir()):
        assert ".stemlab" not in directory.parts, directory


def test_xdg_variables_are_honoured(_isolated_home, monkeypatch):
    monkeypatch.setenv("XDG_CACHE_HOME", str(_isolated_home / "c"))
    monkeypatch.setenv("XDG_DATA_HOME", str(_isolated_home / "d"))

    assert paths.analysis_dir() == _isolated_home / "c" / "StemLab" / "analysis"
    assert (
        paths.recursive_models_dir()
        == _isolated_home / "d" / "StemLab" / "models" / "recursive"
    )


def test_a_relative_xdg_variable_is_ignored(_isolated_home, monkeypatch):
    """The spec says a non-absolute value must be treated as unset."""
    monkeypatch.setenv("XDG_CACHE_HOME", "relative/path")

    assert paths.analysis_dir() == _isolated_home / ".cache" / "StemLab" / "analysis"


def test_the_overrides_still_win(_isolated_home, monkeypatch):
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(_isolated_home / "elsewhere"))
    monkeypatch.setenv("STEMLAB_RECURSIVE_MODEL_DIR", str(_isolated_home / "packaged"))

    assert paths.analysis_dir() == (_isolated_home / "elsewhere").resolve()
    assert paths.recursive_models_dir() == _isolated_home / "packaged"


def test_windows_keeps_its_layout(_isolated_home, monkeypatch):
    """A documented compatibility promise, not an oversight."""
    monkeypatch.setenv("LOCALAPPDATA", str(_isolated_home / "AppData"))

    assert paths.analysis_dir() == _isolated_home / "AppData" / "StemLab" / "Analysis"
    assert (
        paths.recursive_models_dir()
        == _isolated_home / "AppData" / "StemLab" / "Models" / "Recursive"
    )


# --- the callers still resolve through it ---------------------------------


def test_the_analysis_cache_and_recursive_split_share_this(_isolated_home):
    from stemlab.analysis_cache import AnalysisCache
    from stemlab.recursive import default_model_dir

    # Asked of the cache itself rather than of a directory helper: what has to
    # hold is that the database the cache opens lands under the analysis
    # directory these tests pin down, whichever helper it reaches it through.
    assert AnalysisCache().path == paths.analysis_dir() / "analysis.sqlite3"
    assert default_model_dir() == paths.recursive_models_dir()
