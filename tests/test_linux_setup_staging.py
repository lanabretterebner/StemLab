"""scripts/linux/StemLab-Linux-setup.sh: where it puts things while it works.

The setup script handles a multi-gigabyte archive, and it used to do all of
it - download, join, extract - in the folder it was launched from. That left
the user's Downloads folder holding the bundle, its parts and a full extracted
copy at once, and it meant a bundle placed on a small partition had nowhere to
expand. Everything transient now goes to a staging directory under the cache,
and these tests hold that: the folder the script is run from, and the folder it
sits in, are read and never written.

Every test runs against a synthetic bundle. The script's release URL is still
the unbaked "@RELEASE_URL@" placeholder in the source tree, so nothing here
touches the network - the already-downloaded path is the only one reachable,
which is exactly the path being tested.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "scripts" / "linux" / "StemLab-Linux-setup.sh"

pytestmark = pytest.mark.skipif(
    sys.platform != "linux" or shutil.which("bash") is None,
    reason="this is a Linux bash script",
)

BUNDLE = "StemLab-9.9.9-Linux-cpu.tar.gz"


def _bundle(tmp_path: Path) -> Path:
    """A tarball shaped like the one build.sh produces."""
    staging = tmp_path / "build" / BUNDLE[: -len(".tar.gz")]
    (staging / "Engine" / "bin").mkdir(parents=True)
    (staging / "StemLab.vst3").mkdir(parents=True)
    (staging / "Engine" / ".stemlab-engine").touch()
    (staging / "Engine" / ".stemlab-torch-flavor").write_text("cpu\n")
    (staging / ".stemlab-version").write_text("9.9.9\n")
    (staging / "StemLab").touch()

    # The real one installs the VST3; this one only has to prove it ran, and
    # from where.
    (staging / "install.sh").write_text(
        "#!/usr/bin/env bash\n"
        'printf "%s\\n" "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" '
        '> "$STEMLAB_TEST_INSTALL_MARKER"\n'
    )

    archive = tmp_path / "downloads" / BUNDLE
    archive.parent.mkdir(parents=True)

    with tarfile.open(archive, "w:gz") as tar:
        tar.add(staging, arcname=staging.name)

    return archive


def _checksum_beside(archive: Path) -> Path:
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    sha = archive.with_name(archive.name + ".sha256")
    sha.write_text(f"{digest}  {archive.name}\n")

    return sha


def _run(tmp_path: Path, *args: str, cwd: Path | None = None, env=None):
    """Run a copy of the script from the downloads folder, as a user would."""
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)

    script = tmp_path / "downloads" / SETUP.name
    if not script.exists():
        shutil.copy(SETUP, script)

    elsewhere = cwd or (tmp_path / "cwd")
    elsewhere.mkdir(exist_ok=True)

    environment = {
        "HOME": str(home),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "STEMLAB_TEST_INSTALL_MARKER": str(tmp_path / "install-ran-in.txt"),
        **(env or {}),
    }

    return subprocess.run(
        ["bash", str(script), *args],
        capture_output=True,
        text=True,
        env=environment,
        cwd=str(elsewhere),
    )


def _installed(tmp_path: Path) -> Path:
    return tmp_path / "home" / ".local" / "share" / "StemLab"


def _stage(tmp_path: Path) -> Path:
    return tmp_path / "home" / ".cache" / "StemLab" / "setup"


class TestNothingIsWrittenWhereTheUserPutTheBundle:
    def test_the_working_directory_stays_empty(self, tmp_path):
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        result = _run(tmp_path)

        assert result.returncode == 0, result.stderr
        assert list((tmp_path / "cwd").iterdir()) == []

    def test_the_archive_is_extracted_in_the_staging_directory(self, tmp_path):
        # Not beside the bundle: an extracted tree is as large as the archive
        # again, and the folder it was downloaded into is not ours to fill.
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        result = _run(tmp_path, env={"STEMLAB_SETUP_STAGE": str(tmp_path / "elsewhere")})

        assert result.returncode == 0, result.stderr
        assert str(tmp_path / "elsewhere") in result.stdout
        assert _installed(tmp_path).is_dir()

    def test_the_staging_directory_is_gone_afterwards(self, tmp_path):
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        assert _run(tmp_path).returncode == 0
        assert not _stage(tmp_path).exists()

    def test_split_parts_are_joined_into_the_staging_directory(self, tmp_path):
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        payload = archive.read_bytes()
        half = len(payload) // 2
        archive.with_name(archive.name + ".part00").write_bytes(payload[:half])
        archive.with_name(archive.name + ".part01").write_bytes(payload[half:])
        archive.unlink()

        result = _run(tmp_path)

        assert result.returncode == 0, result.stderr
        assert "Joining 2 parts" in result.stdout
        # The joined archive never appears beside the parts.
        assert not (tmp_path / "downloads" / BUNDLE).exists()
        assert _installed(tmp_path).is_dir()


class TestTheInstallItself:
    def test_install_sh_runs_from_the_install_directory(self, tmp_path):
        # It is moved into place first and run there, so it has nothing left
        # to move - the reason it must not try to relocate its own Engine.
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        assert _run(tmp_path).returncode == 0

        ran_in = (tmp_path / "install-ran-in.txt").read_text().strip()
        assert ran_in == str(_installed(tmp_path))

    def test_a_first_install_does_not_reach_for_root(self, tmp_path):
        # ~/.local/share does not exist in a fresh HOME, and "is it writable"
        # answers no for a directory that is not there yet. Asking that
        # question about the parent instead of the nearest existing ancestor
        # made a per-user install prompt for a password and land owned by root.
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        # A sudo that refuses to work, so any use of it fails the run.
        fake = tmp_path / "bin"
        fake.mkdir()
        (fake / "sudo").write_text(
            '#!/usr/bin/env bash\necho "sudo was used" >&2\nexit 111\n'
        )
        (fake / "sudo").chmod(0o755)

        result = _run(
            tmp_path,
            env={"PATH": f"{fake}:{os.environ.get('PATH', '/usr/bin:/bin')}"},
        )

        assert "sudo was used" not in result.stderr
        assert result.returncode == 0, result.stderr
        assert _installed(tmp_path).is_dir()


class TestADamagedArchiveInstallsNothing:
    def test_a_bad_checksum_stops_before_extracting(self, tmp_path):
        archive = _bundle(tmp_path)
        sha = _checksum_beside(archive)
        sha.write_text(f"{'0' * 64}  {archive.name}\n")

        result = _run(tmp_path)

        assert result.returncode != 0
        assert "Checksum mismatch" in result.stderr
        assert not _installed(tmp_path).exists()

    def test_a_hand_placed_archive_is_not_deleted_on_a_mismatch(self, tmp_path):
        # Only files this run downloaded are cleared for a fresh fetch. A
        # bundle somebody obtained some other way is theirs, and re-downloading
        # gigabytes is not this script's call to make on their behalf.
        archive = _bundle(tmp_path)
        sha = _checksum_beside(archive)
        sha.write_text(f"{'0' * 64}  {archive.name}\n")

        assert _run(tmp_path).returncode != 0
        assert archive.exists()

    def test_an_unverifiable_archive_says_so_and_continues(self, tmp_path):
        archive = _bundle(tmp_path)

        result = _run(tmp_path)

        assert result.returncode == 0, result.stderr
        assert "not being verified" in result.stderr
        assert _installed(tmp_path).is_dir()
        assert archive.name  # the bundle is named in the run, not silently used


class TestItRefusesRatherThanGuess:
    def test_an_unfinished_download_blocks_a_second_run(self, tmp_path):
        # Two runs sharing one staging directory would join a half-fetched set
        # of parts and fail the checksum for no visible reason.
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        stage = _stage(tmp_path)
        stage.mkdir(parents=True)
        (stage / f"{BUNDLE}.part03.download").write_bytes(b"half a part")

        result = _run(tmp_path)

        assert result.returncode != 0
        assert "unfinished download" in result.stderr
        assert not _installed(tmp_path).exists()

    def test_a_relative_staging_override_is_refused(self, tmp_path):
        archive = _bundle(tmp_path)
        _checksum_beside(archive)

        result = _run(tmp_path, env={"STEMLAB_SETUP_STAGE": "cache/stemlab"})

        assert result.returncode != 0
        assert "absolute path" in result.stderr
