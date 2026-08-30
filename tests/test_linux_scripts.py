"""The uninstall and update scripts in scripts/linux.

Both are driven against a synthetic HOME rather than a real install: neither
can be tested by running it for real, because the thing it operates on is a
gigabyte of Engine and the failure mode of getting it wrong is deleting
somebody's recordings.

The case that matters most is the first class here. On Linux the app's own
directory and its default install directory are the same folder, so an
uninstall that removed the install directory wholesale would take the user's
audio with it - which is exactly what a naive implementation does.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
UNINSTALL = ROOT / "scripts" / "linux" / "uninstall.sh"
UPDATE = ROOT / "scripts" / "linux" / "update.sh"

pytestmark = pytest.mark.skipif(
    sys.platform != "linux" or shutil.which("bash") is None,
    reason="these are Linux bash scripts",
)


def _install(home: Path, *, version: str = "0.1.7", flavor: str = "cuda") -> Path:
    """A synthetic install, laid out the way the bundle's install.sh leaves it."""
    app = home / ".local/share/StemLab"

    (app / "Engine/bin").mkdir(parents=True)
    (app / "StemLab.vst3").mkdir(parents=True)
    (app / "Engine/.stemlab-engine").touch()
    (app / "Engine/.stemlab-torch-flavor").write_text(f"{flavor}\n")
    (app / ".stemlab-version").write_text(f"{version}\n")

    for name in ("StemLab", "install.sh", "README.txt", "uninstall.sh", "update.sh"):
        (app / name).touch()

    config = home / ".config/StemLab"
    config.mkdir(parents=True)
    (config / "portable_engine_path.txt").write_text(f"{app}/Engine/bin/python3\n")

    (home / ".vst3").mkdir(parents=True)
    (home / ".vst3/StemLab.vst3").mkdir()

    return app


def _run(script: Path, home: Path, *args: str, env: dict[str, str] | None = None):
    environment = {
        "HOME": str(home),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        **(env or {}),
    }

    return subprocess.run(
        ["bash", str(script), *args],
        capture_output=True,
        text=True,
        env=environment,
        cwd=str(home),
    )


class TestUninstallKeepsWhatIsNotTheApps:
    def test_audio_in_the_install_directory_survives(self, tmp_path):
        # The regression this whole script is shaped around: older versions
        # wrote captures into the same folder the bundle unpacks into.
        home = tmp_path / "home"
        app = _install(home)

        (app / "Captures").mkdir()
        (app / "Captures/take.wav").write_text("audio")

        result = _run(UNINSTALL, home, "--yes")

        assert result.returncode == 0, result.stderr
        assert (app / "Captures/take.wav").read_text() == "audio"
        assert not (app / "Engine").exists()
        assert not (app / "StemLab.vst3").exists()

    def test_music_is_never_touched_by_default(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        media = home / "Music/StemLab/Recordings"
        media.mkdir(parents=True)
        (media / "take.wav").write_text("audio")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert (media / "take.wav").read_text() == "audio"

    def test_everything_does_take_the_music(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        media = home / "Music/StemLab/Recordings"
        media.mkdir(parents=True)
        (media / "take.wav").write_text("audio")

        assert _run(UNINSTALL, home, "--everything", "--yes").returncode == 0
        assert not (home / "Music/StemLab").exists()

    def test_model_weights_are_kept_unless_asked_for(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        weights = home / ".cache/bs-roformer-infer"
        weights.mkdir(parents=True)
        (weights / "model.ckpt").write_text("x" * 64)

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert (weights / "model.ckpt").exists()

        assert _run(UNINSTALL, home, "--models", "--yes").returncode == 0
        assert not weights.exists()

    def test_the_plugin_and_settings_go(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (home / ".vst3/StemLab.vst3").exists()
        assert not (home / ".config/StemLab").exists()


class TestUninstallRefusesWhatIsNotAnInstall:
    def test_a_directory_that_is_not_ours_is_left_alone(self, tmp_path):
        # STEMLAB_INSTALL_DIR is user-supplied, so "the install directory" is
        # not always a path this script chose. A wrong one must not be rm -rf'd.
        home = tmp_path / "home"
        home.mkdir(parents=True)

        elsewhere = tmp_path / "not-stemlab"
        elsewhere.mkdir()
        (elsewhere / "thesis.txt").write_text("years of work")

        result = _run(
            UNINSTALL, home, "--yes", env={"STEMLAB_INSTALL_DIR": str(elsewhere)}
        )

        assert result.returncode == 0
        assert (elsewhere / "thesis.txt").exists()
        assert "does not look like a StemLab install" in result.stdout

    def test_a_dry_run_removes_nothing(self, tmp_path):
        home = tmp_path / "home"
        app = _install(home)

        result = _run(UNINSTALL, home, "--dry-run")

        assert result.returncode == 0
        assert "nothing was removed" in result.stdout.lower()
        assert (app / "Engine").exists()
        assert (home / ".vst3/StemLab.vst3").exists()

    def test_answering_no_removes_nothing(self, tmp_path):
        home = tmp_path / "home"
        app = _install(home)

        result = subprocess.run(
            ["bash", str(UNINSTALL)],
            input="n\n",
            capture_output=True,
            text=True,
            env={"HOME": str(home), "PATH": os.environ.get("PATH", "/usr/bin:/bin")},
            cwd=str(home),
        )

        assert result.returncode == 0
        assert (app / "Engine").exists()

    def test_nothing_installed_is_not_an_error(self, tmp_path):
        home = tmp_path / "home"
        home.mkdir(parents=True)

        result = _run(UNINSTALL, home, "--yes")

        assert result.returncode == 0
        assert "not installed" in result.stdout


class TestUpdateComparesVersions:
    def _check(self, home: Path, tag: str, script: Path):
        return _run(script, home, "--check", env={"STEMLAB_LATEST_TAG": tag})

    @pytest.fixture
    def shipped(self, tmp_path):
        """A copy outside the checkout, which is how it reaches a user.

        In the source tree the script says so and stops - there is no bundle
        beside it to update - so testing the update path means testing the
        copy build.sh puts in the bundle.
        """
        target = tmp_path / "bundle" / "update.sh"
        target.parent.mkdir(parents=True)
        shutil.copy(UPDATE, target)

        return target

    def test_a_newer_release_is_offered(self, tmp_path, shipped):
        home = tmp_path / "home"
        _install(home, version="0.1.7")

        result = self._check(home, "v0.1.9", shipped)

        assert result.returncode == 0, result.stderr
        assert "An update is available" in result.stdout

    def test_the_same_version_is_up_to_date(self, tmp_path, shipped):
        home = tmp_path / "home"
        _install(home, version="0.1.7")

        assert "Already up to date" in self._check(home, "v0.1.7", shipped).stdout

    def test_versions_compare_numerically(self, tmp_path, shipped):
        # The one a string compare gets wrong: "0.1.10" sorts before "0.1.9".
        home = tmp_path / "home"
        _install(home, version="0.1.9")

        assert "An update is available" in self._check(home, "v0.1.10", shipped).stdout

    def test_a_newer_install_is_not_silently_downgraded(self, tmp_path, shipped):
        home = tmp_path / "home"
        _install(home, version="0.1.9")

        result = self._check(home, "v0.1.6", shipped)

        assert "newer than the latest release" in result.stdout
        assert "--force" in result.stdout

    def test_the_gpu_flavor_is_carried_over(self, tmp_path, shipped):
        # Updating a cuda install into a cpu one would read as the app
        # getting slow for no reason.
        home = tmp_path / "home"
        _install(home, version="0.1.7", flavor="rocm")

        assert "(rocm)" in self._check(home, "v0.1.9", shipped).stdout

    def test_an_install_without_a_version_marker_reads_the_readme(
        self, tmp_path, shipped
    ):
        # Bundles built before the marker existed, which are the ones most
        # likely to be out of date.
        home = tmp_path / "home"
        app = _install(home, version="0.1.7")

        (app / ".stemlab-version").unlink()
        (app / "README.txt").write_text(
            "StemLab 0.1.5 for Linux (self-contained, torch flavor: cuda)\n"
        )

        assert "0.1.5" in self._check(home, "v0.1.9", shipped).stdout

    def test_no_install_is_an_error_worth_reading(self, tmp_path, shipped):
        home = tmp_path / "home"
        home.mkdir(parents=True)

        result = self._check(home, "v0.1.9", shipped)

        assert result.returncode != 0
        assert "No StemLab install found" in result.stderr

    def test_the_source_copy_declines_and_says_why(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        result = self._check(home, "v0.1.9", UPDATE)

        assert result.returncode == 0
        assert "source tree" in result.stdout
        assert "build.sh" in result.stdout
