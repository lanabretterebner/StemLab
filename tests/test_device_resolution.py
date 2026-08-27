import importlib.metadata
import json
import sys
import types

import pytest

from stemlab import device
from stemlab.pipeline import resolve_device


@pytest.fixture(autouse=True)
def _isolated_probe_cache(tmp_path, monkeypatch):
    # Every test gets a private cache directory so probe answers can never
    # leak between tests or into the developer's real per-user cache.
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(tmp_path / "analysis"))


def _install_fake_torch(monkeypatch, *, cuda_available, xpu_available=False):
    torch = types.ModuleType("torch")
    torch.cuda = types.SimpleNamespace(
        is_available=lambda: cuda_available
    )
    torch.xpu = types.SimpleNamespace(
        is_available=lambda: xpu_available
    )
    monkeypatch.setitem(sys.modules, "torch", torch)


def _install_torch_metadata(monkeypatch, version):
    def fake_version(name):
        assert name == "torch"
        return version

    monkeypatch.setattr(importlib.metadata, "version", fake_version)


def _install_probing_torch(monkeypatch, *, cuda_available, calls=None):
    # The disk cache only participates when torch would need a cold import,
    # so the fake must come through the import machinery rather than
    # sys.modules.
    monkeypatch.delitem(sys.modules, "torch", raising=False)

    def fake_import_module(name):
        assert name == "torch"
        if calls is not None:
            calls.append(name)
        return types.SimpleNamespace(
            cuda=types.SimpleNamespace(is_available=lambda: cuda_available),
            xpu=None,
        )

    # The module-object form: the string form resolves its target through
    # importlib.import_module, which may itself already be patched here.
    monkeypatch.setattr(importlib, "import_module", fake_import_module)


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


def test_cached_probe_answer_skips_torch_import(monkeypatch):
    _install_torch_metadata(monkeypatch, "2.5.1")
    calls: list[str] = []
    _install_probing_torch(monkeypatch, cuda_available=True, calls=calls)

    assert resolve_device("cuda") == "cuda"
    assert calls == ["torch"]

    def refuse_import(name):
        raise AssertionError(f"a cache hit must not import {name}")

    monkeypatch.setattr(importlib, "import_module", refuse_import)

    assert resolve_device("cuda") == "cuda"


def test_probe_cache_invalidated_by_torch_version_change(monkeypatch):
    _install_torch_metadata(monkeypatch, "2.5.1")
    _install_probing_torch(monkeypatch, cuda_available=True)
    assert resolve_device("cuda") == "cuda"

    # A different wheel can answer the probe differently; the stale entry
    # must not survive the upgrade.
    _install_torch_metadata(monkeypatch, "2.6.0")
    _install_probing_torch(monkeypatch, cuda_available=False)
    assert resolve_device("cuda") == "cpu"


def test_probe_cache_invalidated_by_gpu_visibility_change(monkeypatch):
    _install_torch_metadata(monkeypatch, "2.5.1")
    monkeypatch.setenv("CUDA_VISIBLE_DEVICES", "0")
    _install_probing_torch(monkeypatch, cuda_available=True)
    assert resolve_device("cuda") == "cuda"

    # Masking the GPU changes what torch.cuda reports without touching the
    # install, so it has to be part of the cache identity.
    monkeypatch.setenv("CUDA_VISIBLE_DEVICES", "")
    _install_probing_torch(monkeypatch, cuda_available=False)
    assert resolve_device("cuda") == "cpu"


def test_corrupt_probe_cache_falls_back_to_live_probe(monkeypatch):
    _install_torch_metadata(monkeypatch, "2.5.1")
    calls: list[str] = []
    _install_probing_torch(monkeypatch, cuda_available=True, calls=calls)

    path = device._probe_cache_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("{this is not json", encoding="utf-8")

    assert resolve_device("cuda") == "cuda"
    assert calls == ["torch"]

    # The live answer also replaces the corrupt file instead of leaving it
    # to fail every future read.
    assert json.loads(path.read_text(encoding="utf-8"))["probes"]["cuda"] is True


def test_cpu_request_never_probes_or_caches(monkeypatch):
    _install_torch_metadata(monkeypatch, "2.5.1")
    monkeypatch.delitem(sys.modules, "torch", raising=False)

    def refuse_import(name):
        raise AssertionError("resolving cpu needs no torch probe")

    monkeypatch.setattr(importlib, "import_module", refuse_import)

    assert resolve_device("cpu") == "cpu"
    assert not device._probe_cache_path().exists()


def test_missing_torch_metadata_disables_the_cache(monkeypatch):
    # Without installed-package metadata there is no key to file an answer
    # under, so the live path runs and nothing is written.
    def missing(name):
        raise importlib.metadata.PackageNotFoundError(name)

    monkeypatch.setattr(importlib.metadata, "version", missing)
    _install_probing_torch(monkeypatch, cuda_available=True)

    assert resolve_device("cuda") == "cuda"
    assert not device._probe_cache_path().exists()
