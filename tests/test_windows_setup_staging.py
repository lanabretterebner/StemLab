"""scripts/win/StemLab-Windows-setup.ps1: where it puts things while it works.

The Linux sibling of these tests is test_linux_setup_staging.py, and the
defect is the same one: the script downloaded a multi-gigabyte installer and
its .bin slices into the folder it was launched from. Everything transient now
goes to a staging directory under %LOCALAPPDATA%, and the folder the script
sits in is read and never written.

Driven with pwsh rather than read as text - the cwd assumptions this replaced
were the kind that a text assertion cannot see. It needs PowerShell, so it
skips where there is none; test_installer_definition.py holds the parts that
can be checked by reading.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "scripts" / "win" / "StemLab-Windows-setup.ps1"

PWSH = shutil.which("pwsh") or shutil.which("powershell") or (
    "/opt/pwsh/pwsh" if Path("/opt/pwsh/pwsh").exists() else None
)

pytestmark = pytest.mark.skipif(
    PWSH is None, reason="needs PowerShell to run the script rather than read it"
)

SETUP_EXE = "StemLab-Setup-9.9.9-cpu.exe"
SLICE = "StemLab-Setup-9.9.9-cpu-1.bin"


def _release(tmp_path: Path, *, with_sums: bool = True) -> Path:
    """An installer and one slice, as a hand-download beside the script."""
    downloads = tmp_path / "downloads"
    downloads.mkdir()

    (downloads / SETUP_EXE).write_bytes(b"not really an installer, but it hashes\n")
    (downloads / SLICE).write_bytes(b"slice one\n")

    if with_sums:
        lines = []
        for name in (SETUP_EXE, SLICE):
            digest = hashlib.sha256((downloads / name).read_bytes()).hexdigest()
            lines.append(f"{digest}  {name}")
        (downloads / "SHA256SUMS").write_text("\n".join(lines) + "\n")

    shutil.copy(SETUP, downloads / SETUP.name)

    return downloads


def _installer_hook(tmp_path: Path) -> Path:
    """A stand-in for the real installer, which records how it was invoked.

    A native program, and one per platform, for two reasons. The script runs
    it as `& $env:STEMLAB_SETUP_INSTALLER $SetupPath` and then reads
    $LASTEXITCODE, which PowerShell sets for external processes and not for
    scripts it invokes itself - so a .ps1 stub would report no exit code at
    all. And Windows cannot execute a shell script: pointing the hook at one
    is what made every Windows job fail with "the specified module could not
    be found" while the same file passed on Linux.
    """
    marker = tmp_path / "installer-arg.txt"

    if os.name == "nt":
        hook = tmp_path / "installer-hook.cmd"
        # %~1 is the argument with its surrounding quotes stripped, and the
        # redirect goes first so a trailing space cannot join the filename.
        hook.write_text(
            "@echo off\r\n"
            f'> "{marker}" echo %~1\r\n'
            "exit /b 0\r\n"
        )
    else:
        hook = tmp_path / "installer-hook.sh"
        hook.write_text(
            "#!/usr/bin/env bash\n"
            f'printf "%s\\n" "$1" > "{marker}"\n'
            "exit 0\n"
        )
        hook.chmod(0o755)

    return hook


def _run(tmp_path: Path, *args: str, env=None):
    downloads = tmp_path / "downloads"

    elsewhere = tmp_path / "cwd"
    elsewhere.mkdir(exist_ok=True)

    home = tmp_path / "home"
    home.mkdir(exist_ok=True)

    # Inherited rather than replaced: on Windows a process handed a bare
    # environment is missing SystemRoot and friends, and how much still works
    # is luck rather than design.
    environment = dict(os.environ)
    environment.update(
        {
            "HOME": str(home),
            "LOCALAPPDATA": str(home),
            "STEMLAB_SETUP_INSTALLER": str(_installer_hook(tmp_path)),
            "STEMLAB_SETUP_STAGE": str(tmp_path / "stage"),
        }
    )
    environment.update(env or {})

    return subprocess.run(
        [PWSH, "-NoProfile", "-File", str(downloads / SETUP.name), *args],
        capture_output=True,
        text=True,
        env=environment,
        cwd=str(elsewhere),
    )


class TestNothingIsWrittenWhereTheUserPutTheInstaller:
    def test_the_working_directory_stays_empty(self, tmp_path):
        _release(tmp_path)

        result = _run(tmp_path)

        assert result.returncode == 0, result.stdout + result.stderr
        assert list((tmp_path / "cwd").iterdir()) == []

    def test_the_staging_directory_is_gone_afterwards(self, tmp_path):
        _release(tmp_path)
        stage = tmp_path / "stage"

        result = _run(tmp_path)

        assert result.returncode == 0, result.stdout + result.stderr
        assert not stage.exists()

    def test_a_relative_staging_override_is_refused(self, tmp_path):
        _release(tmp_path)

        result = _run(tmp_path, env={"STEMLAB_SETUP_STAGE": "cache/stemlab"})

        assert result.returncode != 0
        assert "must be a full path" in result.stdout

    def test_an_unfinished_download_blocks_a_second_run(self, tmp_path):
        _release(tmp_path)
        stage = tmp_path / "stage"
        stage.mkdir()
        (stage / f"{SETUP_EXE}.download").write_bytes(b"half an installer")

        result = _run(tmp_path)

        assert result.returncode != 0
        assert "unfinished download" in result.stdout


class TestTheInstallerItself:
    def test_it_is_run_from_where_its_slices_are(self, tmp_path):
        # Inno needs the .bin slices beside the .exe, so the set is used
        # where it already is rather than copied somewhere first.
        downloads = _release(tmp_path)

        assert _run(tmp_path).returncode == 0

        invoked = Path((tmp_path / "installer-arg.txt").read_text().strip())
        assert invoked.parent == downloads
        assert invoked.name == SETUP_EXE

    def test_a_damaged_download_never_reaches_the_installer(self, tmp_path):
        downloads = _release(tmp_path)
        (downloads / "SHA256SUMS").write_text(f"{'0' * 64}  {SETUP_EXE}\n")

        result = _run(tmp_path)

        assert result.returncode != 0
        assert "Checksum mismatch" in result.stdout
        assert not (tmp_path / "installer-arg.txt").exists()

    def test_an_unverifiable_download_says_so_and_continues(self, tmp_path):
        _release(tmp_path, with_sums=False)

        result = _run(tmp_path)

        assert result.returncode == 0, result.stdout + result.stderr
        assert "not being verified" in result.stdout + result.stderr

    def test_the_downloads_are_cleared_afterwards(self, tmp_path):
        downloads = _release(tmp_path)

        assert _run(tmp_path).returncode == 0

        # The script's own copy stays: unbaked placeholders mean this is the
        # source-checkout copy, which is the one copy that is not a download.
        assert sorted(p.name for p in downloads.iterdir()) == [SETUP.name]
