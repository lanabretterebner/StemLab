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

    icons = where / "icons"
    icons.mkdir()

    for size in (16, 32, 48, 64, 128, 256, 512):
        (icons / f"stemlab-{size}.png").write_text(f"the {size}px icon")

    (icons / "stemlab.svg").write_text("<svg/>")

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


class TestTheAppReachesTheApplicationsMenu:
    """Why a desktop entry, when the window already sets its own icon.

    A Linux executable has nowhere to carry an icon the way a .exe or an .app
    bundle does. The standalone publishes _NET_WM_ICON, which is what a
    taskbar draws for a window that is already open - but a launcher has to
    know the app exists before there is any window, and that is a desktop
    entry or nothing. StartupWMClass is the other half: it is what ties the
    running window back to the entry, so a dock shows one StemLab rather than
    a pinned launcher beside an unknown window.
    """

    @staticmethod
    def _entry(home: Path) -> str:
        return (home / ".local/share/applications/stemlab.desktop").read_text()

    def test_the_entry_is_written(self, tmp_path, installer_source):
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")

        assert _install(installer_source, home, app).returncode == 0

        entry = self._entry(home)

        assert "[Desktop Entry]" in entry
        assert "Name=StemLab" in entry
        assert "Icon=stemlab" in entry
        assert "Type=Application" in entry

    def test_it_launches_the_app_that_was_installed(self, tmp_path, installer_source):
        home = tmp_path / "home"
        home.mkdir()
        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        assert _install(installer_source, home, here).returncode == 0

        # Quoted, because STEMLAB_INSTALL_DIR can point at a path with a space
        # in it and Exec is parsed as a command line rather than as one path.
        assert f'Exec="{home}/.local/share/StemLab/StemLab"' in self._entry(home)

    def test_it_names_the_window_class_juce_actually_sets(
        self, tmp_path, installer_source
    ):
        # JUCE sets WM_CLASS from the application name, which for the
        # standalone is JucePlugin_Name. Anything else here and the dock keeps
        # the launcher and the running window apart.
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")

        assert _install(installer_source, home, app).returncode == 0
        assert "StartupWMClass=StemLab" in self._entry(home)

    def test_the_icon_is_filed_into_the_theme_at_every_size(
        self, tmp_path, installer_source
    ):
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")

        assert _install(installer_source, home, app).returncode == 0

        theme = home / ".local/share/icons/hicolor"

        for size in (16, 32, 48, 64, 128, 256):
            icon = theme / f"{size}x{size}/apps/stemlab.png"

            assert icon.read_text() == f"the {size}px icon"

        assert (theme / "scalable/apps/stemlab.svg").read_text() == "<svg/>"

    def test_the_icons_survive_a_by_hand_install(self, tmp_path, installer_source):
        # The extracted folder is emptied into the install directory and can
        # then be deleted, so the icons have to be carried across with the
        # rest - otherwise the next update has nothing to re-file.
        home = tmp_path / "home"
        home.mkdir()
        here = _bundle(tmp_path / "Downloads/StemLab-9.9.9-Linux-cpu")

        assert _install(installer_source, home, here).returncode == 0

        installed = home / ".local/share/StemLab/icons"

        assert (installed / "stemlab-256.png").read_text() == "the 256px icon"
        assert (installed / "stemlab.svg").exists()

    def test_a_bundle_without_icons_still_installs(self, tmp_path, installer_source):
        # An older bundle, or one built before the icons existed. The entry is
        # still worth writing: the theme falls back and the app is listed.
        home = tmp_path / "home"
        app = _bundle(home / ".local/share/StemLab")
        shutil.rmtree(app / "icons")

        result = _install(installer_source, home, app)

        assert result.returncode == 0, result.stderr
        assert "Icon=stemlab" in self._entry(home)


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
