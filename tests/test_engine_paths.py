"""The Engine's own directories, and getting out of ~/.stemlab.

Everything the Engine keeps for itself used to sit in ~/.stemlab: a dotfile
directory following no platform convention, mixing a throwaway analysis
cache with gigabytes of downloaded model weights, and telling an uninstaller
or a disk cleaner nothing about which was which.

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
    monkeypatch.setattr(paths, "_migrated", False)
    return tmp_path


# --- where things land ----------------------------------------------------


def test_analysis_is_cache_and_models_are_data(_isolated_home):
    """The split that ~/.stemlab could not express."""
    assert paths.analysis_dir() == _isolated_home / ".cache" / "StemLab" / "analysis"
    assert (
        paths.recursive_models_dir()
        == _isolated_home / ".local" / "share" / "StemLab" / "models" / "recursive"
    )


def test_nothing_is_left_in_the_dot_directory(_isolated_home):
    for directory in (paths.analysis_dir(), paths.recursive_models_dir()):
        assert paths.LEGACY_HOME not in directory.parts, directory


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


# --- the migration --------------------------------------------------------


def legacy(home: Path) -> Path:
    return home / paths.LEGACY_HOME


def test_both_directories_are_moved_out(_isolated_home):
    (legacy(_isolated_home) / "analysis").mkdir(parents=True)
    (legacy(_isolated_home) / "analysis" / "cache.db").write_text("results")
    (legacy(_isolated_home) / "models" / "recursive").mkdir(parents=True)
    (legacy(_isolated_home) / "models" / "recursive" / "m.ckpt").write_text("weights")

    moved = paths.migrate_legacy_home()

    assert len(moved) == 2, moved
    assert (paths.analysis_dir() / "cache.db").read_text() == "results"
    assert (paths.recursive_models_dir() / "m.ckpt").read_text() == "weights"


def test_the_empty_dot_directory_is_removed(_isolated_home):
    (legacy(_isolated_home) / "analysis").mkdir(parents=True)

    paths.migrate_legacy_home()

    assert not legacy(_isolated_home).exists()


def test_anything_else_in_there_is_left_alone(_isolated_home):
    """Only what we know we put there is ours to move."""
    legacy(_isolated_home).mkdir(parents=True)
    (legacy(_isolated_home) / "notes.txt").write_text("someone else's")

    paths.migrate_legacy_home()

    assert (legacy(_isolated_home) / "notes.txt").read_text() == "someone else's"


def test_an_existing_destination_is_never_overwritten(_isolated_home):
    """A half-finished migration resumes; it does not undo itself."""
    (legacy(_isolated_home) / "analysis").mkdir(parents=True)
    (legacy(_isolated_home) / "analysis" / "cache.db").write_text("old")

    destination = _isolated_home / ".cache" / "StemLab" / "analysis"
    destination.mkdir(parents=True)
    (destination / "cache.db").write_text("new")

    paths.migrate_legacy_home()

    assert (destination / "cache.db").read_text() == "new"


def test_no_legacy_directory_is_not_an_error(_isolated_home):
    assert paths.migrate_legacy_home() == []


def test_asking_for_a_directory_migrates_first(_isolated_home):
    """The Engine never calls the migration; wanting a directory does."""
    (legacy(_isolated_home) / "analysis").mkdir(parents=True)
    (legacy(_isolated_home) / "analysis" / "cache.db").write_text("results")

    resolved = paths.analysis_dir()

    assert (resolved / "cache.db").read_text() == "results"


def test_the_migration_runs_once_per_process(_isolated_home, monkeypatch):
    calls: list[int] = []
    monkeypatch.setattr(paths, "migrate_legacy_home", lambda: calls.append(1) or [])

    paths.analysis_dir()
    paths.analysis_dir()
    paths.recursive_models_dir()

    assert len(calls) == 1, calls


def test_an_unmovable_directory_does_not_stop_the_engine(_isolated_home, monkeypatch):
    """Losing a cache costs a re-analysis. Refusing to run costs the job."""
    (legacy(_isolated_home) / "analysis").mkdir(parents=True)

    def refuse(*_args, **_kwargs):
        raise OSError("permission denied")

    monkeypatch.setattr(paths.shutil, "move", refuse)

    assert paths.migrate_legacy_home() == []


# --- the callers still resolve through it ---------------------------------


def test_the_analysis_cache_and_recursive_split_share_this(_isolated_home):
    from stemlab.analysis_cache import managed_analysis_dir
    from stemlab.recursive import default_model_dir

    assert managed_analysis_dir() == paths.analysis_dir()
    assert default_model_dir() == paths.recursive_models_dir()
