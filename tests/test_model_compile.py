"""Tests for warming the compiled-kernel cache.

The warm-up itself needs torch, a working toolchain and a 667 MB checkpoint,
so what is tested here is everything around the compile: that it refuses
politely when it cannot help, that it writes into the directory a separation
later reads, and above all that it drives the same command a separation does.

That last one is the whole design. An inductor entry is keyed on the graph, so
a warm-up whose shapes differ from production fills the cache with kernels no
job asks for and looks exactly like compiling having no effect - a failure
that produces no error anywhere. Both paths now build their argv through one
shared function, and the test below derives its expectation from it rather
than restating it, so there is nothing left to keep in step.
"""

from __future__ import annotations

import os
import wave
from pathlib import Path

import pytest

from stemlab import model_compile


@pytest.fixture
def compiling_on(monkeypatch):
    monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")


@pytest.fixture
def short_warm_up(monkeypatch):
    """Shorten the warm-up audio for the tests that do not depend on its length.

    _write_warm_up_audio synthesises every sample in a Python loop, so the
    shipped 25 s costs the better part of a second per call - and eight tests
    below write that file to prove things that hold at any length. A second of
    it exercises the same code and leaves the constant itself to the one test
    that is about the length (test_it_is_long_enough_to_cross_a_chunk_boundary,
    deliberately not given this fixture).
    """
    monkeypatch.setattr(model_compile, "WARM_UP_SECONDS", 1)


class TestItRefusesPolitelyWhenItCannotHelp:
    def test_a_model_nothing_compiles_is_not_an_error(self, compiling_on):
        # Demucs and the adaptive models are not patched by compile_support,
        # so there is no cache for them to warm and never will be here.
        for model_id in ("demucs", "beat-this-fast", "recursive-drums"):
            with pytest.raises(model_compile.WarmUpUnavailable):
                model_compile.warm_up(model_id)

    def test_compiling_switched_off_is_not_an_error(self, monkeypatch):
        monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)

        with pytest.raises(model_compile.WarmUpUnavailable) as caught:
            model_compile.warm_up("roformer")

        # The message has to name the switch, because an unset opt-in and a
        # missing compiler are indistinguishable to the person reading it.
        assert "STEMLAB_TORCH_COMPILE" in str(caught.value)

    def test_a_machine_that_cannot_compile_says_why(self, compiling_on, monkeypatch):
        monkeypatch.setattr(
            model_compile, "compile_support_status", lambda _d: (False, "no host C++ compiler")
        )
        monkeypatch.setattr(model_compile, "pick_best_device", lambda: "cpu")

        with pytest.raises(model_compile.WarmUpUnavailable) as caught:
            model_compile.warm_up("roformer")

        assert "no host C++ compiler" in str(caught.value)

    def test_unavailable_is_distinct_from_a_warm_up_that_broke(self):
        # model_manager reports the two differently, so they must not be the
        # same type. WarmUpUnavailable is a RuntimeError, but not every
        # RuntimeError is one.
        assert issubclass(model_compile.WarmUpUnavailable, RuntimeError)
        assert not isinstance(RuntimeError("boom"), model_compile.WarmUpUnavailable)


class TestAutoDeviceResolution:
    def test_auto_goes_through_pick_best_device(self, compiling_on, monkeypatch):
        # resolve_torch_device hands back anything it does not recognise, so
        # asking it about "auto" tests the literal string for an inductor
        # backend and always concludes there is none. Worth a test because
        # the symptom is "this machine cannot compile" on a machine that can.
        seen = {}

        monkeypatch.setattr(model_compile, "pick_best_device", lambda: "cuda")
        monkeypatch.setattr(
            model_compile,
            "compile_support_status",
            lambda device: (seen.setdefault("device", device), (False, "stop here"))[1],
        )

        with pytest.raises(model_compile.WarmUpUnavailable):
            model_compile.warm_up("roformer", device="auto")

        assert seen["device"] == "cuda"

    def test_an_explicit_device_is_resolved_not_guessed(self, compiling_on, monkeypatch):
        seen = {}

        monkeypatch.setattr(model_compile, "resolve_torch_device", lambda d: d)
        monkeypatch.setattr(
            model_compile,
            "compile_support_status",
            lambda device: (seen.setdefault("device", device), (False, "stop here"))[1],
        )

        with pytest.raises(model_compile.WarmUpUnavailable):
            model_compile.warm_up("roformer", device="cpu")

        assert seen["device"] == "cpu"


class TestWarmUpAudio:
    def test_it_writes_the_rate_and_shape_the_model_requires(self, tmp_path, short_warm_up):
        path = tmp_path / "warm.wav"
        model_compile._write_warm_up_audio(path)

        with wave.open(str(path)) as handle:
            # BS-RoFormer's band splits are fixed in bins, so anything but
            # 44.1 kHz would be resampled and change the shapes traced.
            assert handle.getframerate() == model_compile.WARM_UP_RATE
            assert handle.getnchannels() == 2
            assert handle.getsampwidth() == 2
            assert handle.getnframes() == (
                model_compile.WARM_UP_RATE * model_compile.WARM_UP_SECONDS
            )

    def test_it_is_not_silence(self, tmp_path, short_warm_up):
        # A separator fed digital black can take an early exit, and a graph
        # that was never traced is one the real job still has to compile.
        path = tmp_path / "warm.wav"
        model_compile._write_warm_up_audio(path)

        with wave.open(str(path)) as handle:
            frames = handle.readframes(handle.getnframes())

        assert any(byte != 0 for byte in frames[:4096])

    def test_it_is_long_enough_to_cross_a_chunk_boundary(self):
        # Shapes settle after the first two passes, and the shorter final
        # chunk costs one more compile. A warm-up that stopped inside the
        # first chunk would leave the real job to compile what this exists
        # to precompile.
        assert model_compile.WARM_UP_SECONDS >= 20


class TestItDrivesTheSameCommandASeparationDoes:
    def _capture(self, monkeypatch):
        captured = {}

        def fake_run(command, log, progress, **kwargs):
            captured["command"] = list(command)
            captured["env"] = dict(os.environ)
            progress(1.0)
            return 0

        monkeypatch.setattr(model_compile, "run_progress_process", fake_run)
        monkeypatch.setattr(model_compile, "pick_best_device", lambda: "cpu")
        monkeypatch.setattr(model_compile, "compile_support_status", lambda _d: (True, "ok"))
        return captured

    def test_the_argv_is_the_separation_path_verbatim(
        self, compiling_on, short_warm_up, monkeypatch
    ):
        from stemlab.pretrained import DEFAULT_MODEL, build_roformer_command

        captured = self._capture(monkeypatch)
        model_compile.warm_up("roformer")

        command = captured["command"]

        staging = Path(command[command.index("--input_folder") + 1])
        store = Path(command[command.index("--store_dir") + 1])

        # Derived from the builder a separation uses, not restated here: a
        # copy of the expected argv would be one more thing to drift, and
        # drift in this particular value is silent.
        assert command == build_roformer_command(staging, store, "cpu", DEFAULT_MODEL)
        assert staging != store

    def test_the_child_is_told_to_compile(self, compiling_on, short_warm_up, monkeypatch):
        captured = self._capture(monkeypatch)
        model_compile.warm_up("roformer")

        assert captured["env"]["STEMLAB_TORCH_COMPILE"] == "1"

    def test_kernels_land_where_the_separation_will_read_them(
        self, compiling_on, short_warm_up, monkeypatch
    ):
        # The single line that decides whether warming was worth doing.
        from stemlab.compile_support import inductor_cache_dir

        captured = self._capture(monkeypatch)
        model_compile.warm_up("roformer")

        assert captured["env"]["TORCHINDUCTOR_CACHE_DIR"] == str(inductor_cache_dir())

    def test_the_warm_up_audio_exists_when_the_child_runs(
        self, compiling_on, short_warm_up, monkeypatch
    ):
        captured = self._capture(monkeypatch)

        def fake_run(command, log, progress, **kwargs):
            staging = Path(command[command.index("--input_folder") + 1])
            captured["wavs"] = sorted(p.name for p in staging.glob("*.wav"))
            return 0

        monkeypatch.setattr(model_compile, "run_progress_process", fake_run)
        model_compile.warm_up("roformer")

        # The upstream CLI discovers its input with glob("*.wav"), so exactly
        # one file has to be there.
        assert len(captured["wavs"]) == 1

    def test_a_failing_child_raises_with_what_it_said(
        self, compiling_on, short_warm_up, monkeypatch
    ):
        self._capture(monkeypatch)

        def failing(command, log, progress, **kwargs):
            log("something went wrong in the child")
            return 3

        monkeypatch.setattr(model_compile, "run_progress_process", failing)

        with pytest.raises(RuntimeError) as caught:
            model_compile.warm_up("roformer")

        assert not isinstance(caught.value, model_compile.WarmUpUnavailable)
        assert "something went wrong in the child" in str(caught.value)

    def test_it_reports_progress_and_returns_elapsed_seconds(
        self, compiling_on, short_warm_up, monkeypatch
    ):
        self._capture(monkeypatch)
        seen: list[tuple[float, str]] = []

        elapsed = model_compile.warm_up(
            "roformer", progress=lambda fraction, stage: seen.append((fraction, stage))
        )

        assert elapsed > 0.0
        assert seen[0][0] == 0.0
        assert seen[-1][0] == 1.0
        assert all(0.0 <= fraction <= 1.0 for fraction, _ in seen)


class TestTheManagerTreatsUnavailableAsAState:
    def test_compile_model_reports_it_as_unavailable_not_a_failure(
        self, tmp_path, monkeypatch
    ):
        from stemlab import model_manager

        weights = tmp_path / "roformer.ckpt"
        weights.write_bytes(b"x")

        monkeypatch.setattr(model_manager, "locate", lambda _id: weights)
        monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)

        # A run whose downloads all succeeded should not come back red just
        # because compiling was switched off.
        with pytest.raises(model_manager.CompileUnavailable):
            model_manager.compile_model("roformer")
