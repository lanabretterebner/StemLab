"""Every flag the plugin sends must be one its Engine entry point accepts.

The plugin builds argument lists for five Python modules by hand, in C++, and
argparse rejects anything it does not know with exit code 2 - before a single
sample is read. That is how ``--normalize-fused-stems`` shipped: the flag was
added to the separation command and to ``stemlab-separate``, but never to
``stemlab.plugin_job``, which is the module the plugin actually runs. Turning
the setting on failed every separation, and nothing caught it because no test
ever compared the two sides.

This reads the flags straight out of PluginProcessor.cpp rather than restating
them, so a flag added to the C++ without a home in Python fails here.
"""

from __future__ import annotations

import argparse
import importlib
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
PROCESSOR = REPO_ROOT / "src" / "plugin" / "Source" / "PluginProcessor.cpp"

# The command builders each open by naming their module, then append flags, so
# the module named most recently above a command.add owns that flag.
_MODULE = re.compile(r'(?:makePythonModuleCommand|interpreterModuleCommand)\([^)]*"(stemlab\.[a-z_]+)"')
_FLAG = re.compile(r'command\.add\("(--[a-z0-9-]+)"\)')


def _flags_by_module() -> dict[str, set[str]]:
    """Walk the C++ once, attributing each flag to the module above it."""
    found: dict[str, set[str]] = {}
    current: str | None = None

    for line in PROCESSOR.read_text(encoding="utf-8").splitlines():
        module = _MODULE.search(line)
        if module:
            current = module.group(1)
            found.setdefault(current, set())
            continue

        flag = _FLAG.search(line)
        if flag and current is not None:
            found[current].add(flag.group(1))

    return found


def _accepted_flags(module_name: str) -> set[str]:
    """Every option string the module's parser will accept.

    The parsers are built inside ``main()``, so rather than calling it - which
    would run the job - the ArgumentParser is captured as it is constructed.
    """
    module = importlib.import_module(module_name)
    captured: list[argparse.ArgumentParser] = []

    real_init = argparse.ArgumentParser.__init__

    def recording_init(self, *args, **kwargs):
        real_init(self, *args, **kwargs)
        captured.append(self)

    real_parse = argparse.ArgumentParser.parse_args

    class _Stop(Exception):
        pass

    def stop_before_running(self, *args, **kwargs):
        raise _Stop

    argparse.ArgumentParser.__init__ = recording_init
    argparse.ArgumentParser.parse_args = stop_before_running
    try:
        try:
            module.main()
        except _Stop:
            pass
    finally:
        argparse.ArgumentParser.__init__ = real_init
        argparse.ArgumentParser.parse_args = real_parse

    accepted: set[str] = set()
    for parser in captured:
        for action in parser._actions:
            accepted.update(action.option_strings)
    return accepted


def test_processor_source_is_where_we_think_it_is():
    assert PROCESSOR.is_file(), f"{PROCESSOR} moved; this test needs its new path"


def test_some_flags_were_actually_found():
    """A regex that quietly matches nothing would make every case below pass."""
    by_module = _flags_by_module()
    assert by_module, "no python module commands found in PluginProcessor.cpp"
    assert sum(len(v) for v in by_module.values()) > 10


@pytest.mark.parametrize("module_name", sorted(_flags_by_module()))
def test_engine_accepts_every_flag_the_plugin_sends(module_name):
    sent = _flags_by_module()[module_name]
    accepted = _accepted_flags(module_name)

    unknown = sorted(sent - accepted)
    assert not unknown, (
        f"{module_name} would reject {unknown} with exit code 2. "
        f"The plugin sends them from PluginProcessor.cpp; add them to that "
        f"module's parser, or stop sending them."
    )


def test_separation_accepts_fused_normalisation():
    """The specific flag that shipped broken, named so a regression is obvious."""
    assert "--normalize-fused-stems" in _accepted_flags("stemlab.plugin_job")
