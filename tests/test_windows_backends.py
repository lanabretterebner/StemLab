from __future__ import annotations

import importlib.util
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]

# The .ps1 entry points themselves only execute on Windows; the pure-Python
# backend inspection above them runs everywhere.
needs_powershell = pytest.mark.skipif(
    shutil.which("powershell.exe") is None, reason="requires Windows PowerShell"
)
VERIFY_PATH = ROOT / "scripts" / "win" / "verify_windows_backend.py"
SPEC = importlib.util.spec_from_file_location("verify_windows_backend", VERIFY_PATH)
assert SPEC and SPEC.loader
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class FakeCuda:
    def __init__(self, available: bool = False) -> None:
        self.available = available

    def is_available(self) -> bool:
        return self.available

    def get_device_name(self, _index: int) -> str:
        return "Test GPU"


class FakeVersion:
    def __init__(self, *, cuda=None, hip=None) -> None:
        self.cuda = cuda
        self.hip = hip


class FakeTorch:
    __version__ = "2.9.1+test"

    def __init__(self, *, cuda=None, hip=None, available: bool = False) -> None:
        self.version = FakeVersion(cuda=cuda, hip=hip)
        self.cuda = FakeCuda(available)


def test_nvidia_rejects_cpu_only_torch():
    with pytest.raises(RuntimeError, match="CPU-only Torch"):
        VERIFY.inspect_backend("nvidia", FakeTorch())


def test_cpu_accepts_cpu_torch():
    details = VERIFY.inspect_backend("cpu", FakeTorch())
    assert details["Backend"] == "CPU"
    assert details["CUDA"] == "None"


def test_amd_requires_hip_torch():
    with pytest.raises(RuntimeError, match="torch.version.hip is None"):
        VERIFY.inspect_backend("amd", FakeTorch())
    details = VERIFY.inspect_backend("amd", FakeTorch(hip="7.2.1", available=True))
    assert details["HIP"] == "7.2.1"
    assert details["GPU"] == "Test GPU"


# The build scripts take no backend argument on purpose: the portable
# payload records the torch flavor it actually contains and the installer
# names itself from that record, so only the dev setup selects a backend.
@needs_powershell
@pytest.mark.parametrize("script", ["scripts/win/setup_dev.ps1"])
def test_invalid_backend_is_rejected(script: str):
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-File", str(ROOT / script), "-Backend", "invalid"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "ValidateSet" in result.stderr or "valid values" in result.stderr


PORTABLE_BUILD = ROOT / "scripts" / "win" / "build_portable_windows.ps1"


def test_engine_copy_excludes_test_suites_by_exact_plural_name():
    """The exclusion is a string in an array; nothing else guards it.

    'tests' -> 'test*' looks like a harmless widening and is not: torch ships
    real, imported directories called torch/test and torch/include/c10/test,
    and a glob would take them. The Linux side has a test for exactly this
    mutation (tests/test_engine_prune.py); this is its Windows counterpart,
    and it runs on Linux because the .ps1 itself cannot.
    """
    lines = PORTABLE_BUILD.read_text(encoding="utf-8").splitlines()
    # The invocation, not the whole file: the comment above it necessarily
    # discusses the singular name in order to explain why it is excluded.
    calls = [ln for ln in lines if "Invoke-Robocopy $VenvSitePackages" in ln]

    assert len(calls) == 1, calls
    assert '@("/XD", "__pycache__", "tests")' in calls[0]
    # Any of these would also match torch's singular, real directories.
    for widened in ('"test*"', '"*test*"', '"test"'):
        assert widened not in calls[0], widened


def test_the_windows_engine_still_excludes_pycache_everywhere_it_copies():
    """Three robocopy calls build the Engine; all three must agree.

    Linux deliberately KEEPS __pycache__ (measured: dropping it costs ~2.2x
    on first import, and on a read-only install every run). Windows has never
    shipped it. The asymmetry is deliberate but easy to erase by accident, so
    the three call sites are pinned together rather than individually.
    """
    script = PORTABLE_BUILD.read_text(encoding="utf-8")

    assert script.count('"/XD", "__pycache__"') == 3
