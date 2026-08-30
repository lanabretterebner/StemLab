"""Guards around opt-in torch.compile support.

Every test here exists because the unguarded version of the same code breaks a
separation rather than merely failing to speed it up: TorchInductor reports a
missing C++ compiler by raising from the first traced call, so "just wrap the
model" turns an absent toolchain into a failed job.
"""

from __future__ import annotations

import json
import os
import sys
import types

import pytest
import torch
from torch import nn

from stemlab import compile_support


class _Stub(nn.Module):
    """Stands in for the upstream separator, which is not installed here."""

    def forward(self, audio):  # noqa: D102
        return audio * 2


def _stub_module(class_name: str = "BSRoformer") -> types.ModuleType:
    module = types.ModuleType("stemlab_test_upstream")
    stub = type(class_name, (_Stub,), {})
    setattr(module, class_name, stub)
    return module


def test_compile_is_off_unless_requested(monkeypatch):
    monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)
    assert compile_support.compile_requested() is False


@pytest.mark.parametrize("value", ["1", "true", "TRUE", "yes", "on"])
def test_truthy_values_request_compile(monkeypatch, value):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", value)
    assert compile_support.compile_requested() is True


@pytest.mark.parametrize("value", ["0", "false", "no", "off", "", "  "])
def test_falsy_values_leave_compile_off(monkeypatch, value):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", value)
    assert compile_support.compile_requested() is False


def test_cache_dir_honours_override(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path / "kernels"))
    assert compile_support.inductor_cache_dir() == tmp_path / "kernels"


def test_cache_dir_defaults_under_the_managed_directory(monkeypatch, tmp_path):
    monkeypatch.delenv("STEMLAB_TORCH_COMPILE_CACHE", raising=False)
    monkeypatch.setenv("STEMLAB_ANALYSIS_HOME", str(tmp_path))
    # A stable path is the contract with whatever warms the cache ahead of the
    # first job; a per-run temporary directory would make compiling a loss.
    assert compile_support.inductor_cache_dir() == tmp_path / "torchinductor"


def test_directml_is_rejected():
    # DirectML sits on torch's privateuseone slot and has no inductor backend.
    usable, reason = compile_support.compile_support_status("privateuseone")
    assert usable is False
    assert "TorchInductor" in reason


def test_cpu_without_a_compiler_is_rejected(monkeypatch):
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: False)
    usable, reason = compile_support.compile_support_status("cpu")
    assert usable is False
    assert "C++ compiler" in reason


def test_cpu_with_a_compiler_is_accepted(monkeypatch):
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)
    usable, _ = compile_support.compile_support_status("cpu")
    assert usable is True


def test_gpu_without_triton_is_rejected(monkeypatch):
    monkeypatch.setattr(compile_support, "_triton_available", lambda: False)
    usable, reason = compile_support.compile_support_status("cuda")
    assert usable is False
    assert "Triton" in reason


def test_rocm_on_windows_is_rejected(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(torch.version, "hip", "7.2.1", raising=False)
    usable, reason = compile_support.compile_support_status("cuda")
    assert usable is False
    assert "ROCm on Windows" in reason


def test_device_index_is_ignored(monkeypatch):
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)
    assert compile_support.compile_support_status("cpu:0")[0] is True


def test_arming_is_a_no_op_when_not_requested(monkeypatch):
    monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)
    module = _stub_module()
    assert compile_support.arm_torch_compile(modules={"m": module}) == []
    assert not getattr(module.BSRoformer, compile_support._PATCH_MARKER, False)


def test_arming_patches_the_separator_class(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    module = _stub_module()
    assert compile_support.arm_torch_compile(modules={"m": module}) == ["BSRoformer"]
    assert getattr(module.BSRoformer, compile_support._PATCH_MARKER) is True


def test_arming_twice_does_not_double_wrap(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    module = _stub_module()
    scope = {"m": module}
    compile_support.arm_torch_compile(modules=scope)
    wrapped = module.BSRoformer.forward
    assert compile_support.arm_torch_compile(modules=scope) == []
    assert module.BSRoformer.forward is wrapped


def test_arming_sets_the_cache_directory(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path / "kernels"))
    monkeypatch.delenv("TORCHINDUCTOR_CACHE_DIR", raising=False)
    compile_support.arm_torch_compile(modules={"m": _stub_module()})
    assert os.environ["TORCHINDUCTOR_CACHE_DIR"] == str(tmp_path / "kernels")


def test_unsupported_device_runs_eagerly_and_says_why(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: False)

    def explode(*_args, **_kwargs):
        raise AssertionError("torch.compile must not be reached on an unsupported device")

    monkeypatch.setattr(torch, "compile", explode)

    module = _stub_module()
    messages: list[str] = []
    compile_support.arm_torch_compile(messages.append, modules={"m": module})

    audio = torch.ones(4)
    assert torch.equal(module.BSRoformer()(audio), audio * 2)
    assert any("C++ compiler" in message for message in messages)


def test_a_failing_compiled_forward_falls_back_to_eager(monkeypatch, tmp_path):
    """The case that would otherwise fail the whole separation.

    Inductor raises InvalidCxxCompiler from the first traced call, not from
    torch.compile itself, so the fallback has to live around the call.
    """
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)

    def fake_compile(fn):
        def raiser(*_args, **_kwargs):
            raise RuntimeError("InvalidCxxCompiler: No working C++ compiler found\nline two 50%")

        return raiser

    monkeypatch.setattr(torch, "compile", fake_compile)

    module = _stub_module()
    messages: list[str] = []
    compile_support.arm_torch_compile(messages.append, modules={"m": module})

    audio = torch.ones(4)
    model = module.BSRoformer()
    assert torch.equal(model(audio), audio * 2)

    failure = next(message for message in messages if "continuing eagerly" in message)
    # The parent reads this stream line by line and treats a percent next to a
    # transfer rate as download progress, so the log line stays single-line
    # and percent-free.
    assert "\n" not in failure
    assert "%" not in failure

    # Having failed once, the compiled path is abandoned rather than retried.
    messages.clear()
    assert torch.equal(model(audio), audio * 2)
    assert messages == []


def test_a_genuine_model_error_still_surfaces(monkeypatch, tmp_path):
    """Falling back must not swallow a real failure into a silent wrong answer."""
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)
    monkeypatch.setattr(torch, "compile", lambda fn: fn)

    module = types.ModuleType("stemlab_test_upstream")

    class BSRoformer(nn.Module):
        def forward(self, audio):
            raise ValueError("bad input")

    module.BSRoformer = BSRoformer
    compile_support.arm_torch_compile(modules={"m": module})

    with pytest.raises(ValueError, match="bad input"):
        BSRoformer()(torch.ones(4))


def test_compiled_forward_is_used_when_everything_is_available(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)

    calls: list[str] = []

    def fake_compile(fn):
        calls.append("compiled")

        def run(*args, **kwargs):
            calls.append("ran")
            return fn(*args, **kwargs)

        return run

    monkeypatch.setattr(torch, "compile", fake_compile)

    module = _stub_module()
    compile_support.arm_torch_compile(modules={"m": module})
    model = module.BSRoformer()
    audio = torch.ones(4)

    assert torch.equal(model(audio), audio * 2)
    assert torch.equal(model(audio), audio * 2)
    # Compiled once, reused after: recompiling per call would be a huge loss.
    assert calls == ["compiled", "ran", "ran"]


def test_mel_band_roformer_is_also_armed(monkeypatch, tmp_path):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path))
    module = _stub_module("MelBandRoformer")
    assert compile_support.arm_torch_compile(modules={"m": module}) == ["MelBandRoformer"]


def test_one_line_flattens_and_defuses_exception_text():
    flattened = compile_support._one_line(RuntimeError("a\nb   c 40% done"))
    assert flattened == "a b c 40 pct done"


class TestASeparationsCompileIsRecorded:
    """Compiling during a real job warms the same cache a warm-up does.

    Only the Model Manager's own Compile action used to write the marker, so
    a model could be compiled on every separation and still report itself as
    never compiled - the button stayed on "Compile" forever, and there was
    nothing anywhere to explain it.
    """

    def _armed(self, monkeypatch, tmp_path, class_name="BSRoformer"):
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path / "inductor"))
        monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)
        monkeypatch.setattr(torch, "compile", lambda fn: fn)

        module = _stub_module(class_name)
        compile_support.arm_torch_compile(modules={"m": module})

        return getattr(module, class_name)()

    def _marker(self, tmp_path):
        return tmp_path / "stemlab_warm_roformer.json"

    def test_a_compiled_pass_records_the_model(self, monkeypatch, tmp_path):
        model = self._armed(monkeypatch, tmp_path)

        assert not self._marker(tmp_path).exists()

        model(torch.ones(4))

        recorded = json.loads(self._marker(tmp_path).read_text(encoding="utf-8"))

        assert recorded["torch"] == compile_support.torch_build_id()
        assert recorded["device"] == "cpu"
        assert recorded["source"] == "separation"

    def test_the_model_manager_then_reports_it_compiled(self, monkeypatch, tmp_path):
        # The whole point, end to end: what a separation records is what the
        # status probe reads back, in a process that never imports torch.
        from stemlab import model_manager

        model = self._armed(monkeypatch, tmp_path)
        model(torch.ones(4))

        assert model_manager._compile_state("roformer")["compiled"] is True

    def test_it_is_written_once_however_many_passes_run(self, monkeypatch, tmp_path):
        model = self._armed(monkeypatch, tmp_path)
        model(torch.ones(4))

        stamped = self._marker(tmp_path).stat().st_mtime_ns

        for _ in range(5):
            model(torch.ones(4))

        # A separation runs the forward once per chunk. Rewriting the same
        # fact each time would be a write per chunk for no new information.
        assert self._marker(tmp_path).stat().st_mtime_ns == stamped

    def test_a_forward_that_failed_records_nothing(self, monkeypatch, tmp_path):
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(tmp_path / "inductor"))
        monkeypatch.setattr(compile_support, "_cxx_compiler_available", lambda: True)

        def exploding(_fn):
            def run(*_args, **_kwargs):
                raise RuntimeError("no C++ compiler found")

            return run

        monkeypatch.setattr(torch, "compile", exploding)

        module = _stub_module()
        compile_support.arm_torch_compile(modules={"m": module})
        model = module.BSRoformer()

        assert torch.equal(model(torch.ones(4)), torch.ones(4) * 2)

        # It fell back to eager, so nothing was compiled and nothing may
        # claim it was.
        assert not self._marker(tmp_path).exists()

    def test_recording_cannot_break_a_separation(self, monkeypatch, tmp_path):
        # Failing to note a fact about a cache is never worth a failed job.
        def unwritable(*_args, **_kwargs):
            raise OSError("read-only file system")

        monkeypatch.setattr(compile_support, "compile_marker_path", unwritable)

        model = self._armed(monkeypatch, tmp_path)

        assert torch.equal(model(torch.ones(4)), torch.ones(4) * 2)

    def test_an_uncompiled_model_class_is_not_credited(self, monkeypatch, tmp_path):
        # The mapping is by class, so a class this does not know about warms
        # the cache without claiming to be a model in the registry.
        monkeypatch.setitem(compile_support._MODEL_IDS_BY_CLASS, "BSRoformer", None)

        model = self._armed(monkeypatch, tmp_path)
        model(torch.ones(4))

        assert not self._marker(tmp_path).exists()
