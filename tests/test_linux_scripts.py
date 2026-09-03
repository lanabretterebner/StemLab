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


def _write(path: Path, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def _install(home: Path, *, version: str = "0.1.7", flavor: str = "cuda") -> Path:
    """A synthetic install, as the bundle plus a run of the app leaves it.

    Both halves matter. install.sh lays down the first group; the second is
    what the Engine and the plugin write afterwards, and every one of those
    paths is a place an uninstaller has to know about without the bundle's
    file list mentioning it.
    """
    app = home / ".local/share/StemLab"

    (app / "Engine/bin").mkdir(parents=True)
    (app / "StemLab.vst3").mkdir(parents=True)
    (app / "Engine/.stemlab-engine").touch()
    (app / "Engine/.stemlab-torch-flavor").write_text(f"{flavor}\n")
    (app / ".stemlab-version").write_text(f"{version}\n")

    for name in ("StemLab", "install.sh", "README.txt", "uninstall.sh", "update.sh"):
        (app / name).touch()

    for size in (16, 32, 48, 64, 128, 256, 512):
        _write(app / f"icons/stemlab-{size}.png")

    _write(app / "icons/stemlab.svg")

    # What install.sh writes outside the install folder so a launcher can find
    # the app at all - see TestTheAppReachesTheApplicationsMenu in
    # test_linux_bundle_installer.py.
    _write(home / ".local/share/applications/stemlab.desktop", "[Desktop Entry]\n")

    for size in (16, 32, 48, 64, 128, 256):
        _write(home / f".local/share/icons/hicolor/{size}x{size}/apps/stemlab.png")

    _write(home / ".local/share/icons/hicolor/scalable/apps/stemlab.svg")

    # Written by the running app into the same folder - see paths.py's
    # recursive_models_dir and StemLabPaths' remoteStatusDirectory.
    _write(app / "models/recursive/UVR-BVE-4B_SN-44100-2.pth")
    _write(app / "Ableton/status.json")

    config = home / ".config/StemLab"
    config.mkdir(parents=True)
    (config / "torch_compile.txt").write_text("1\n")

    (home / ".vst3").mkdir(parents=True)
    (home / ".vst3/StemLab.vst3").mkdir()

    _write(home / ".cache/StemLab/analysis/analysis.sqlite3")
    _write(home / ".cache/StemLab/analysis/torchinductor/kernel.so")
    _write(home / ".cache/StemLab/analysis/stemlab_warm_roformer.json")

    _write(home / ".cache/bs-roformer-infer/model.ckpt")
    _write(home / ".cache/huggingface/hub/models--adefossez--HTDemucs-6s/blob")
    _write(home / ".cache/torch/hub/checkpoints/5c90dfd2-34c22ccb.th")

    return app


def _run(script: Path, home: Path, *args: str, env: dict[str, str] | None = None):
    temp = home / "tmp"
    temp.mkdir(parents=True, exist_ok=True)

    environment = {
        "HOME": str(home),
        # Pinned inside the fixture: the script sweeps $TMPDIR/StemLab, and a
        # test that reached the real /tmp would delete a running app's files.
        "TMPDIR": str(temp),
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
        assert not (home / ".local/share/StemLab").exists()

    def test_the_plugin_and_settings_go(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (home / ".vst3/StemLab.vst3").exists()
        assert not (home / ".config/StemLab").exists()


class TestUninstallFindsWhereThingsActuallyLive:
    """The directories moved out of ~/.stemlab; the uninstaller had not."""

    def test_the_analysis_cache_and_the_weights_go(self, tmp_path):
        home = tmp_path / "home"
        app = _install(home)

        analysis = _write(home / ".cache/StemLab/analysis/beats.sqlite")
        weights = _write(app / "models/recursive/model.onnx")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not analysis.exists()
        assert not weights.exists()

    def test_the_weights_are_not_removed_and_kept_at_once(self, tmp_path):
        # They live inside the install directory, which is also where anything
        # not the app's is listed as kept. One run must not claim both.
        home = tmp_path / "home"
        _install(home)

        result = _run(UNINSTALL, home, "--yes")

        assert result.returncode == 0, result.stderr
        assert "models" not in result.stdout.partition("Kept in")[2]

    def test_kept_weights_are_not_called_the_users(self, tmp_path):
        # --keep-models keeps them deliberately. Listing them under "not the
        # app's" would say the opposite of what is true.
        home = tmp_path / "home"
        _install(home)

        result = _run(UNINSTALL, home, "--keep-models", "--yes")

        assert result.returncode == 0, result.stderr
        assert "models" not in result.stdout.partition("Kept in")[2]

    def test_a_stale_dot_directory_goes(self, tmp_path):
        # Nothing writes ~/.stemlab any more, so anyone who has one is holding
        # gigabytes that will never be read again.
        home = tmp_path / "home"
        _install(home)
        _write(home / ".stemlab/models/old.onnx")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (home / ".stemlab").exists()

    def test_an_unfinished_setup_download_goes(self, tmp_path):
        # Installer debris, never anything the user made.
        home = tmp_path / "home"
        _install(home)

        stage = _write(home / ".cache/StemLab/setup/StemLab-0.1.9-cuda.tar.gz.part03")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not stage.parent.exists()

    def test_the_engines_own_directories_go_too(self, tmp_path):
        # models/ and Ableton/ are written into the install folder by the app
        # rather than laid down by the bundle, so an uninstaller working from
        # the bundle's file list alone reads them as the user's and keeps half
        # a gigabyte of weights forever.
        home = tmp_path / "home"
        app = _install(home)

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (app / "models").exists()
        assert not (app / "Ableton").exists()

    def test_temporary_files_go(self, tmp_path):
        home = tmp_path / "home"
        _install(home)
        _write(home / "tmp/StemLab/Ableton/status.json")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (home / "tmp/StemLab").exists()

    def test_captures_in_the_install_directory_still_survive(self, tmp_path):
        home = tmp_path / "home"
        app = _install(home)

        (app / "Captures").mkdir()
        (app / "Captures/take.wav").write_text("audio")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert (app / "Captures/take.wav").read_text() == "audio"


class TestUninstallTakesAllOfIt:
    """The default is meant to leave nothing of StemLab except your audio.

    Each path in the fixture has been reachable only through a flag, or
    through no flag at all, at some point in this script's life - which is the
    failure this test exists to catch. A location the app writes to and the
    uninstaller does not know about is invisible: it shows up as disk that
    never comes back.
    """

    def test_nothing_of_the_app_survives_the_default(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        media = _write(home / "Music/StemLab/Recordings/take.wav", "audio")
        foreign = _write(home / ".cache/huggingface/hub/models--someone--LLM/blob")
        other_torch = _write(home / ".cache/torch/hub/checkpoints/resnet50.pth")

        assert _run(UNINSTALL, home, "--yes").returncode == 0

        remaining = sorted(
            str(path.relative_to(home)) for path in home.rglob("*") if path.is_file()
        )

        assert remaining == sorted(
            str(path.relative_to(home)) for path in (media, foreign, other_torch)
        ), remaining


class TestUninstallLeavesOtherApplicationsAlone:
    """The shared caches, which are not StemLab's to empty."""

    def test_only_our_entry_leaves_the_huggingface_cache(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        theirs = _write(home / ".cache/huggingface/hub/models--meta--Llama/blob")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert theirs.exists()
        assert not (home / ".cache/huggingface/hub/models--adefossez--HTDemucs-6s").exists()

    def test_only_our_checkpoint_leaves_the_torch_hub_cache(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        theirs = _write(home / ".cache/torch/hub/checkpoints/resnet50-0676ba61.pth")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert theirs.exists()
        assert not (home / ".cache/torch/hub/checkpoints/5c90dfd2-34c22ccb.th").exists()


class TestKeepModels:
    def test_weights_stay_but_the_app_still_goes(self, tmp_path):
        home = tmp_path / "home"
        app = _install(home)

        assert _run(UNINSTALL, home, "--keep-models", "--yes").returncode == 0

        assert (home / ".cache/bs-roformer-infer/model.ckpt").exists()
        assert (app / "models/recursive/UVR-BVE-4B_SN-44100-2.pth").exists()
        assert (home / ".cache/huggingface/hub/models--adefossez--HTDemucs-6s").exists()

        assert not (app / "Engine").exists()
        assert not (home / ".vst3/StemLab.vst3").exists()
        assert not (home / ".cache/StemLab").exists()


class TestUninstallRefusesWhatIsNotAnInstall:
    def test_a_directory_that_is_not_ours_is_left_alone(self, tmp_path):
        # STEMLAB_INSTALL_DIR is user-supplied, so "the install directory" is
        # not always a path this script chose. A wrong one must not be rm -rf'd.
        home = tmp_path / "home"
        home.mkdir(parents=True)

        elsewhere = tmp_path / "not-stemlab"
        elsewhere.mkdir()
        (elsewhere / "thesis.txt").write_text("years of work")

        result = _run(UNINSTALL, home, "--yes", env={"STEMLAB_INSTALL_DIR": str(elsewhere)})

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


@pytest.fixture(scope="module")
def build_script() -> str:
    return (ROOT / "scripts" / "linux" / "build.sh").read_text()


class TestBothScriptsShipWithTheApp:
    """A script only in the repository helps nobody uninstalling.

    build.sh assembles the bundle that becomes the release tarball, so these
    two copy lines are the whole of "shipped". Losing one is silent: the
    bundle still builds, installs and runs, and the absence shows up only when
    somebody goes looking for how to remove it.
    """

    @pytest.mark.parametrize("name", ["uninstall.sh", "update.sh"])
    def test_it_is_copied_into_the_bundle(self, build_script, name):
        assert f'cp "$REPO_ROOT/scripts/linux/{name}" "$DIST_DIR/{name}"' in build_script

    @pytest.mark.parametrize("name", ["uninstall.sh", "update.sh"])
    def test_it_is_made_executable(self, build_script, name):
        # A shipped script that is not +x is a support ticket, not a crash.
        chmods = [row for row in build_script.splitlines() if row.startswith("chmod +x")]

        assert any(f'"$DIST_DIR/{name}"' in row for row in chmods), chmods

    def test_the_uninstaller_knows_the_bundle_it_will_be_removing(self):
        # BUNDLE_ENTRIES is a hand-written copy of what the bundle holds, so a
        # file added to the bundle and not to that list is left behind.
        listed = UNINSTALL.read_text().split("BUNDLE_ENTRIES=(")[1].split(")")[0].split()

        for name in (
            "Engine",
            "StemLab",
            "StemLab.vst3",
            "icons",
            "install.sh",
            "uninstall.sh",
            "update.sh",
            "README.txt",
            ".stemlab-version",
        ):
            assert name in listed


class TestUninstallTakesTheLauncherWithIt:
    """The desktop entry and the icon theme files install.sh writes.

    They are the only things StemLab puts outside its own folders besides the
    VST3, and they are the ones a user sees if they are left: a menu entry
    that launches nothing, with an icon, forever.
    """

    def test_the_menu_entry_goes(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert not (home / ".local/share/applications/stemlab.desktop").exists()

    def test_the_theme_icons_go(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        assert _run(UNINSTALL, home, "--yes").returncode == 0

        theme = home / ".local/share/icons/hicolor"

        for size in (16, 32, 48, 64, 128, 256):
            assert not (theme / f"{size}x{size}/apps/stemlab.png").exists()

        assert not (theme / "scalable/apps/stemlab.svg").exists()

    def test_other_applications_keep_their_icons(self, tmp_path):
        # hicolor is shared. Only the files called stemlab are ours, and an
        # uninstaller that removed the size directories would take the rest of
        # the desktop's icons with it.
        home = tmp_path / "home"
        _install(home)

        stranger = home / ".local/share/icons/hicolor/48x48/apps/audacity.png"
        _write(stranger, "not ours")

        other_entry = home / ".local/share/applications/audacity.desktop"
        _write(other_entry, "[Desktop Entry]\n")

        assert _run(UNINSTALL, home, "--yes").returncode == 0
        assert stranger.read_text() == "not ours"
        assert other_entry.exists()

    def test_they_are_named_in_the_dry_run(self, tmp_path):
        home = tmp_path / "home"
        _install(home)

        result = _run(UNINSTALL, home, "--dry-run")

        assert result.returncode == 0, result.stderr
        assert "applications/stemlab.desktop" in result.stdout
        assert "hicolor/256x256/apps/stemlab.png" in result.stdout


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

    def test_an_install_without_a_version_marker_reads_the_readme(self, tmp_path, shipped):
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
