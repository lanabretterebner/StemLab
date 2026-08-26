"""One version, one source: pyproject.toml.

The release workflow refuses a tag that disagrees with pyproject.toml, and
everything else derives its number from it - the CMake project version
(which feeds JucePlugin_VersionString and the settings-menu version tag),
the Python package's __version__ (via distribution metadata), and the
Windows installer's AppVersion (passed by build_installer_windows.ps1).
These tests are tripwires against a hardcoded copy creeping back in.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

VERSION_LITERAL = re.compile(r'"[0-9]+\.[0-9]+\.[0-9]+"')


def read(relative: str) -> str:
    return (REPO / relative).read_text(encoding="utf-8")


def pyproject_version() -> str:
    match = re.search(r'^version = "([^"]+)"', read("pyproject.toml"), re.MULTILINE)

    assert match, "pyproject.toml must declare the project version"

    return match.group(1)


def test_pyproject_version_is_plain_x_y_z():
    # CMake's project() only accepts dotted integers, so a suffixed version
    # ("0.2.0rc1") would break the plugin configure step.
    assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", pyproject_version())


def test_cmake_derives_its_version_from_pyproject():
    cmake = read("src/plugin/CMakeLists.txt")

    assert "pyproject.toml" in cmake
    assert re.search(r"project\(StemLabPlugin VERSION \$\{STEMLAB_VERSION\}", cmake), (
        "the plugin version must come from pyproject.toml, not a literal"
    )


def test_python_package_version_is_not_hardcoded():
    package = read("src/stemlab/__init__.py")

    assert "pyproject.toml" in package
    assert "importlib.metadata" in package

    # The only version literal allowed is the last-resort sentinel.
    hardcoded = [hit for hit in VERSION_LITERAL.findall(package) if hit != '"0.0.0"']

    assert not hardcoded, f"hardcoded version(s) in the package: {hardcoded}"


def test_package_version_matches_pyproject():
    import stemlab

    assert stemlab.__version__ == pyproject_version()


def test_installer_refuses_to_guess_a_version():
    iss = read("scripts/win/StemLab.iss")

    assert not re.search(r"#define AppVersion " + VERSION_LITERAL.pattern, iss)
    assert re.search(r"#ifndef AppVersion\s*\n(\s*;[^\n]*\n)*\s*#error", iss), (
        "a manual compile without /DAppVersion must fail, not stamp a stale number"
    )


def test_installer_build_script_reads_pyproject():
    script = read("scripts/win/build_installer_windows.ps1")

    assert "pyproject.toml" in script
    assert "/DAppVersion=$Version" in script
