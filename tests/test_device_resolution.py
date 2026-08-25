import sys
import types

import pytest

from stemlab.pipeline import resolve_device


def _install_fake_torch(monkeypatch, *, cuda_available, xpu_available=False):
    torch = types.ModuleType("torch")
    torch.cuda = types.SimpleNamespace(
        is_available=lambda: cuda_available
    )
    torch.xpu = types.SimpleNamespace(
        is_available=lambda: xpu_available
    )
    monkeypatch.setitem(sys.modules, "torch", torch)


@pytest.mark.parametrize("requested", ["cuda", "auto", "", "CUDA", "  cuda  "])
def test_cuda_is_used_when_available(monkeypatch, requested):
    _install_fake_torch(monkeypatch, cuda_available=True)
    assert resolve_device(requested) == "cuda"


@pytest.mark.parametrize("requested", ["cuda", "auto", "", "CUDA"])
def test_falls_back_to_cpu_without_cuda(monkeypatch, requested):
    _install_fake_torch(monkeypatch, cuda_available=False)
    assert resolve_device(requested) == "cpu"


@pytest.mark.parametrize("requested", ["cpu", "mps", "cuda:1"])
def test_explicit_devices_are_left_alone(monkeypatch, requested):
    # An explicit non-cuda device is the user's decision, so it must survive
    # regardless of what torch reports.
    _install_fake_torch(monkeypatch, cuda_available=False)
    assert resolve_device(requested) == requested


def test_unimportable_torch_falls_back_to_cpu(monkeypatch):
    import builtins

    real_import = builtins.__import__

    def explode(name, *args, **kwargs):
        if name == "torch":
            raise ImportError("no torch here")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", explode)

    # A broken torch install is reported by the backends themselves; device
    # resolution must not turn it into a crash of its own.
    assert resolve_device("cuda") == "cpu"


@pytest.mark.parametrize("requested", ["auto", ""])
def test_auto_prefers_cuda_over_xpu(monkeypatch, requested):
    _install_fake_torch(monkeypatch, cuda_available=True, xpu_available=True)
    assert resolve_device(requested) == "cuda"


@pytest.mark.parametrize("requested", ["auto", ""])
def test_auto_uses_xpu_when_cuda_is_missing(monkeypatch, requested):
    _install_fake_torch(monkeypatch, cuda_available=False, xpu_available=True)
    assert resolve_device(requested) == "xpu"


def test_explicit_xpu_resolves_when_available(monkeypatch):
    _install_fake_torch(monkeypatch, cuda_available=False, xpu_available=True)
    assert resolve_device("xpu") == "xpu"


def test_explicit_xpu_falls_back_to_cpu(monkeypatch):
    # Requesting xpu on a build without the backend must degrade, not crash.
    _install_fake_torch(monkeypatch, cuda_available=False, xpu_available=False)
    assert resolve_device("xpu") == "cpu"
