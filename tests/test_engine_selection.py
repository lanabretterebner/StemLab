import numpy as np
import soundfile as sf

from stemlab.device import resolve_torch_device
from stemlab.hybrid import fuse_stem_pair
from stemlab.pipeline import ENGINE_CHOICES
from stemlab import recursive
from stemlab.recursive import _require_separator


def test_engine_choices():
    assert ENGINE_CHOICES == (
        "roformer",
        "demucs",
        "hybrid",
    )


def test_cuda_request_falls_back_when_torch_has_no_cuda(monkeypatch):
    class FakeCuda:
        @staticmethod
        def is_available():
            return False

    class FakeTorch:
        cuda = FakeCuda()

    messages: list[str] = []

    def fake_import_module(name):
        assert name == "torch"
        return FakeTorch()

    monkeypatch.setattr("importlib.import_module", fake_import_module)

    assert resolve_torch_device("cuda", messages.append) == "cpu"
    assert "Falling back to CPU" in messages[0]


def test_cuda_request_uses_cuda_when_torch_supports_it(monkeypatch):
    class FakeCuda:
        @staticmethod
        def is_available():
            return True

    class FakeTorch:
        cuda = FakeCuda()

    def fake_import_module(name):
        assert name == "torch"
        return FakeTorch()

    monkeypatch.setattr("importlib.import_module", fake_import_module)

    assert resolve_torch_device("cuda", lambda _: None) == "cuda"


def test_recursive_dependency_contract():
    # audio-separator is an optional source-development extra. A minimal unit-test
    # environment should still be able to validate the failure contract, while
    # scripts/win/setup_dev.ps1 and release builds install the dependency and take the
    # success branch.
    if recursive.Separator is None:
        import pytest

        with pytest.raises(RuntimeError, match="audio-separator"):
            _require_separator()
    else:
        _require_separator()


def test_hybrid_fusion_writes_audio(tmp_path):
    sr = 8000
    seconds = 0.5
    samples = int(sr * seconds)
    t = np.arange(samples, dtype=np.float32) / sr

    a = 0.25 * np.sin(2 * np.pi * 220.0 * t)
    b = 0.22 * np.sin(2 * np.pi * 220.0 * t + 0.05)

    ro = np.stack([a, a], axis=1)
    de = np.stack([b, b], axis=1)

    ro_path = tmp_path / "ro.wav"
    de_path = tmp_path / "de.wav"
    out_path = tmp_path / "hybrid.wav"

    sf.write(ro_path, ro, sr, subtype="FLOAT")
    sf.write(de_path, de, sr, subtype="FLOAT")

    fuse_stem_pair(
        roformer_path=ro_path,
        demucs_path=de_path,
        output_path=out_path,
        stem="vocals",
        chunk_seconds=0.25,
        overlap_seconds=0.02,
    )

    assert out_path.exists()

    output, out_sr = sf.read(
        out_path,
        dtype="float32",
        always_2d=True,
    )

    assert out_sr == sr
    assert output.shape[0] > 0
    assert np.max(np.abs(output)) > 0.01
