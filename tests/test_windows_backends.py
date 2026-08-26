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
VERIFY_PATH = ROOT / "scripts" / "verify_windows_backend.py"
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
@pytest.mark.parametrize("script", ["scripts/setup_dev.ps1"])
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


@needs_powershell
def test_backend_output_suffixes_do_not_collide():
    helper = ROOT / "scripts" / "windows_backend.ps1"
    command = (
        f". '{helper}'; "
        "'nvidia','cpu','amd' | ForEach-Object { (Get-StemLabBackendConfiguration $_).Suffix }"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    suffixes = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    assert suffixes == ["NVIDIA", "CPU", "AMD-Experimental"]
    assert len(suffixes) == len(set(suffixes))


