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

import os
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


class TestImportingTheRuntimeMovesNothing:
    """Nothing may redirect the weights into the Engine directory.

    stemlab.runtime used to do exactly that on import: with an
    Engine/ModelCaches present it pointed BS_ROFORMER_MODELS_PATH, HF_HOME,
    the Demucs repo and STEMLAB_RECURSIVE_MODEL_DIR at directories under
    Engine/. Weights download on first use now, so that directory stopped
    existing and the function returned on its first line - but had it ever
    come back, every weight would have been inside the one directory an
    update replaces wholesale, which the setup script's carry-across skips
    on purpose.

    So this is not a test that dead code is gone. It is a test that importing
    the Engine leaves the model locations where paths.py puts them, whatever
    is or is not sitting beside the interpreter.
    """

    REDIRECTED = (
        "BS_ROFORMER_MODELS_PATH",
        "STEMLAB_DEMUCS_MODEL_REPO",
        "HF_HOME",
        "HF_HUB_OFFLINE",
        "TRANSFORMERS_OFFLINE",
        "STEMLAB_RECURSIVE_MODEL_DIR",
    )

    @staticmethod
    def _import_fresh():
        """Run runtime.py's module body again, under a throwaway name.

        Not importlib.reload: that rebinds the live module's attributes while
        the rest of the suite holds references to the old ones - its process
        list, its locks, its shutdown event. This runs the same code with the
        same side effects and leaves sys.modules alone. runtime.py imports
        nothing but the standard library, so a second copy costs nothing.
        """
        import importlib.util
        import sys

        source = Path(paths.__file__).with_name("runtime.py")
        spec = importlib.util.spec_from_file_location("_runtime_under_test", source)
        module = importlib.util.module_from_spec(spec)

        # Registered while the body runs and taken out again: @dataclass
        # looks its own class up through sys.modules, and a module that is
        # not in there raises rather than decorating.
        sys.modules[spec.name] = module

        try:
            spec.loader.exec_module(module)
        finally:
            sys.modules.pop(spec.name, None)

        return module

    def test_it_sets_none_of_the_model_variables(self, tmp_path, monkeypatch):
        """With the directory that used to trigger it sitting right there.

        Standing the ModelCaches folder up beside a stand-in interpreter is
        the whole point: without it the old code returned on its first line
        and this would have passed against the bug it is guarding.
        """
        engine = tmp_path / "Engine" / "bin"
        engine.mkdir(parents=True)
        (engine / "python3").write_text("the interpreter")

        caches = engine / "ModelCaches"
        (caches / "demucs").mkdir(parents=True)
        (caches / "demucs" / "5c90dfd2-34c22ccb.th").write_text("weights")
        (caches / "bs-roformer-infer").mkdir()
        (caches / "huggingface").mkdir()

        monkeypatch.setattr("sys.executable", str(engine / "python3"))

        for name in self.REDIRECTED:
            monkeypatch.delenv(name, raising=False)

        self._import_fresh()

        for name in self.REDIRECTED:
            assert name not in os.environ, f"importing the runtime set {name}"

    def test_the_weights_still_land_in_the_data_directory(
        self, _isolated_home, monkeypatch
    ):
        monkeypatch.delenv("STEMLAB_RECURSIVE_MODEL_DIR", raising=False)

        self._import_fresh()

        assert paths.recursive_models_dir() == (
            _isolated_home / ".local" / "share" / "StemLab" / "models" / "recursive"
        )


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
