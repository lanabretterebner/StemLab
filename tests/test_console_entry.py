"""Tests for calling an installed console script without pip's launcher.

The launcher is a small generated file with an absolute interpreter path in
it. That path is correct on the machine the Engine was built on and wrong on
the machine it is shipped to, and the error it produces names the launcher
rather than the interpreter, so it reads as a missing file that is right
there. Reading the entry point pip generated the launcher *from* has no
absolute path in it at all.
"""

from __future__ import annotations

from importlib.metadata import EntryPoint

import pytest

from stemlab import console_entry


class _Recorded:
    """An entry point whose load() hands back a function we can watch."""

    def __init__(self, name, result=None):
        self.name = name
        self.result = result
        self.loaded = False
        self.called = False

    def load(self):
        self.loaded = True

        def entry():
            self.called = True
            return self.result

        return entry


@pytest.fixture
def installed(monkeypatch):
    entries = {}

    def fake_entry_points(*, group):
        assert group == "console_scripts"
        return list(entries.values())

    monkeypatch.setattr(console_entry, "entry_points", fake_entry_points)
    return entries


class TestLoading:
    def test_it_finds_the_entry_point_by_name(self, installed):
        recorded = _Recorded("bs-roformer-download")
        installed["download"] = recorded
        installed["other"] = _Recorded("something-else")

        entry = console_entry.load_console_entry("bs-roformer-download")

        assert recorded.loaded
        assert not recorded.called
        entry()
        assert recorded.called

    def test_a_missing_entry_point_names_itself(self, installed):
        # The message reaches the user through STEMLAB_ERROR, so the name of
        # what is missing is the whole content of it.
        with pytest.raises(RuntimeError) as caught:
            console_entry.load_console_entry("bs-roformer-download")

        assert "bs-roformer-download" in str(caught.value)

    def test_loading_happens_before_calling(self, installed):
        # bs_roformer_cli patches the module the entry point imports, in
        # between the two. Fold them together and there is nowhere to stand.
        recorded = _Recorded("bs-roformer-infer")
        installed["infer"] = recorded

        console_entry.load_console_entry("bs-roformer-infer")

        assert recorded.loaded and not recorded.called


class TestRunning:
    def test_an_entry_that_returns_nothing_is_a_success(self, installed):
        installed["download"] = _Recorded("bs-roformer-download", result=None)

        assert console_entry.run_console_entry("bs-roformer-download") == 0

    def test_an_entry_that_returns_a_status_keeps_it(self, installed):
        installed["download"] = _Recorded("bs-roformer-download", result=3)

        assert console_entry.run_console_entry("bs-roformer-download") == 3

    def test_a_systemexit_is_not_swallowed(self, installed, monkeypatch):
        # argparse exits this way on a bad argument, and the child's exit code
        # is what the parent reports as the failure.
        def load(_name):
            def entry():
                raise SystemExit(2)

            return entry

        monkeypatch.setattr(console_entry, "load_console_entry", load)

        with pytest.raises(SystemExit) as caught:
            console_entry.run_console_entry("bs-roformer-download")

        assert caught.value.code == 2


class TestAgainstTheRealMetadata:
    def test_it_finds_a_console_script_this_project_installs(self):
        # Proof the lookup matches how pip actually records entry points,
        # rather than only the shape of the fake above.
        try:
            entry = console_entry.load_console_entry("stemlab-model-manager")
        except RuntimeError:
            pytest.skip("stemlab is not pip-installed in this environment")

        assert isinstance(EntryPoint, type)
        assert callable(entry)
        assert entry.__name__ == "main"
