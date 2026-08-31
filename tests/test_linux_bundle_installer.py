"""The install.sh that scripts/linux/build.sh writes into the bundle.

It is generated rather than checked in, so it is pulled back out of build.sh
and run here. Two callers reach it and they need opposite things:

  * the setup script moves the extracted folder to become the install
    directory and runs install.sh from inside it, so everything is already
    where it belongs and there is nothing to move;
  * somebody who extracted the tarball by hand runs it from wherever they
    put it, and then it has to move everything.

Telling them apart is load-bearing. A version of this script that always
moved did "rm -rf $INSTALL_DIR/Engine" followed by "mv $HERE/Engine" with
$HERE and $INSTALL_DIR the same folder - which deletes the Engine and then
fails, on the ordinary path every downloader takes.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "scripts" / "linux" / "build.sh"

pytestmark = pytest.mark.skipif(
    sys.platform != "linux" or shutil.which("bash") is None,
    reason="these are Linux bash scripts",
)


@pytest.fixture(scope="module")
def installer_source() -> str:
    match = re.search(
        r"cat > \"\$DIST_DIR/install\.sh\" <<'INSTALLER'\n(.*?)\nINSTALLER\n",
        BUILD.read_text(),
        re.S,
    )

    assert match is not None, "build.sh no longer writes install.sh as a heredoc"

    return match.group(1)


def _bundle(where: Path) -> Path:
    """A bundle folder with the pieces install.sh looks at."""
    where.mkdir(parents=True, exist_ok=True)

    (where / "Engine" / "bin").mkdir(parents=True)
    (where / "Engine" / "bin" / "python3").write_text("the interpreter")
    (where / "Engine" / ".stemlab-engine").touch()
    (where / "StemLab.vst3" / "Contents").mkdir(parents=True)
    (where / "StemLab.vst3" / "Contents" / "module.so").write_text("the plugin")
    (where / "StemLab").write_text("the app")
    (where / ".stemlab-version").write_text("9.9.9\n")

    for script in ("uninstall.sh", "update.sh"):
        (where / script).write_text("#!/usr/bin/env bash\nexit 0\n")

    (where / "README.txt").write_text("StemLab 9.9.9 for Linux\n")

    return where


def _install(installer_source: str, home: Path, here: Path):
    script = here / "install.sh"
    script.write_text(installer_source)

    return subprocess.run(
        ["bash", str(script)],
        capture_output=True,
        text=True,
        env={"HOME": str(home), "PATH": os.environ.get("PATH", "/usr/bin:/bin")},
        cwd=str(here),
    )


class TestRunFromInsideTheInstallDirectory:
    """The path the setup script takes, and the one that used to self-destruct."""

    def test_the_engine_survives(self, tmp_path, installer_source):
        home = tmp_path / "home"
        app = home / ".local/share/StemLab"
        _bundle(app)

        result = _install(installer_source, home, app)

        assert result.returncode == 0, result.stderr
        assert (app / "Engine/bin/python3").read_text() == "the interpreter"

    def test_the_vst3_is_still_registered(self, tmp_path, installer_source):
        home = tmp_path / "home"
        app = home / ".local/share/StemLab"
        _bundle(app)

        assert _install(installer_source, home, app).returncode == 0
        assert (
            home / ".vst3/StemLab.vst3/Contents/module.so"
        ).read_text() == "the plugin"


class TestRunFromWhereverItWasExtracted:
    def test_everything_moves_to_the_install_directory(self, tmp_path, installer_source):
        home = tmp_path / "home"
        home.mkdir()
        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        result = _install(installer_source, home, here)
        app = home / ".local/share/StemLab"

        assert result.returncode == 0, result.stderr
        assert (app / "Engine/bin/python3").read_text() == "the interpreter"
        assert (app / "StemLab").read_text() == "the app"
        assert (app / ".stemlab-version").read_text() == "9.9.9\n"
        assert (home / ".vst3/StemLab.vst3/Contents/module.so").exists()

    def test_removing_and_updating_are_installed_beside_the_app(
        self, tmp_path, installer_source
    ):
        # Finding the repository to uninstall is not a reasonable ask, and
        # uninstall.sh already expects to find itself next to the app.
        home = tmp_path / "home"
        home.mkdir()
        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        assert _install(installer_source, home, here).returncode == 0
        app = home / ".local/share/StemLab"

        for script in ("uninstall.sh", "update.sh"):
            assert (app / script).exists()
            assert os.access(app / script, os.X_OK)

    def test_the_engine_is_moved_rather_than_copied(self, tmp_path, installer_source):
        # It is gigabytes, and a bundle that has been installed has no further
        # use for its own copy.
        home = tmp_path / "home"
        home.mkdir()
        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        assert _install(installer_source, home, here).returncode == 0
        assert not (here / "Engine").exists()

    def test_a_previous_engine_is_replaced(self, tmp_path, installer_source):
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")
        (app / "Engine/bin/python3").write_text("the old interpreter")

        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        assert _install(installer_source, home, here).returncode == 0
        assert (app / "Engine/bin/python3").read_text() == "the interpreter"


class TestItRefusesAnIncompleteFolder:
    def test_a_missing_engine_changes_nothing(self, tmp_path, installer_source):
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")

        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")
        shutil.rmtree(here / "Engine")

        result = _install(installer_source, home, here)

        assert result.returncode != 0
        assert "incomplete" in result.stderr
        assert (app / "Engine/bin/python3").read_text() == "the interpreter"
