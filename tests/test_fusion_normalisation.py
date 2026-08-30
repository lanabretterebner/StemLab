"""Fused stems must sum back to the source unless asked not to.

Hybrid fusion used to scale every stem so its own peak sat at 0.999. The
factor comes from one stem alone, so a loud stem was attenuated and a quiet
one was not: the six stopped summing back to the mix they came from, and
their balance against each other shifted. Stems are written as 32-bit float,
where a sample above 1.0 is exactly representable, so the normalisation was
never protecting the file from clipping.

It is still available as a setting, off by default.
"""

from __future__ import annotations

import inspect

import numpy as np
import pytest
import soundfile as sf

from stemlab.audio import peak_normalize
from stemlab.hybrid import fuse_stem_folders, fuse_stem_pair
from stemlab.pipeline import separate


def write(path, audio, rate=44100):
    sf.write(str(path), np.asarray(audio, dtype=np.float32).T, rate, subtype="FLOAT")
    return path


def loud_pair(tmp_path, name, peak):
    """A matching roformer/demucs pair whose fusion lands near ``peak``."""
    rng = np.random.default_rng(abs(hash(name)) % 2**32)
    audio = rng.normal(0.0, 0.2, (2, 44100)).astype(np.float32)
    audio *= peak / max(float(np.max(np.abs(audio))), 1e-9)
    a = write(tmp_path / f"{name}_r.wav", audio)
    b = write(tmp_path / f"{name}_d.wav", audio)
    return a, b


# --- the default ----------------------------------------------------------


def test_a_hot_stem_keeps_its_level_by_default(tmp_path):
    """The regression: this stem used to come back scaled by 0.999/1.4."""
    a, b = loud_pair(tmp_path, "drums", 1.4)
    out = tmp_path / "drums.wav"

    fused, _ = fuse_stem_pair(a, b, out, "drums")

    assert float(np.max(np.abs(fused))) == pytest.approx(1.4, abs=0.05)
    written, _ = sf.read(str(out), always_2d=True)
    assert float(np.max(np.abs(written))) == pytest.approx(1.4, abs=0.05)


def test_a_peak_above_one_survives_the_float_wav(tmp_path):
    """The reason the normalisation was never needed for the file itself."""
    a, b = loud_pair(tmp_path, "bass", 2.5)
    out = tmp_path / "bass.wav"

    fuse_stem_pair(a, b, out, "bass")

    written, _ = sf.read(str(out), always_2d=True)
    assert float(np.max(np.abs(written))) == pytest.approx(2.5, abs=0.05)
    assert sf.info(str(out)).subtype == "FLOAT"


def test_normalisation_is_off_by_default_in_every_signature():
    """A caller that forgets the argument must get the faithful behaviour."""
    for function in (fuse_stem_pair, fuse_stem_folders):
        assert inspect.signature(function).parameters["normalize"].default is False

    assert inspect.signature(separate).parameters["normalize_fused"].default is False


# --- the setting still works ---------------------------------------------


def test_turning_it_on_restores_the_old_behaviour(tmp_path):
    a, b = loud_pair(tmp_path, "vocals", 1.4)
    out = tmp_path / "vocals.wav"

    fused, _ = fuse_stem_pair(a, b, out, "vocals", normalize=True)

    assert float(np.max(np.abs(fused))) == pytest.approx(0.999, abs=0.002)


def test_a_quiet_stem_is_untouched_either_way(tmp_path):
    """peak_normalize only ever attenuates, so this is the same both ways."""
    a, b = loud_pair(tmp_path, "piano", 0.3)

    plain, _ = fuse_stem_pair(a, b, tmp_path / "p1.wav", "piano")
    normed, _ = fuse_stem_pair(a, b, tmp_path / "p2.wav", "piano", normalize=True)

    assert np.allclose(plain, normed)


# --- why it matters -------------------------------------------------------


def test_per_stem_normalisation_is_what_breaks_the_sum():
    """Pins the mechanism, so the tests above cannot pass for a wrong reason.

    Six stems that sum exactly to a mix stop doing so once each is scaled by
    its own peak, because the scale factors differ.
    """
    rng = np.random.default_rng(0)
    stems = [rng.normal(0.0, s, (2, 4096)).astype(np.float32) for s in (0.6, 0.5, 0.4)]
    stems = [s * (p / float(np.max(np.abs(s)))) for s, p in zip(stems, (1.4, 1.1, 0.5), strict=True)]
    mix = sum(stems)

    assert float(np.max(np.abs(mix - sum(stems)))) == 0.0

    normalised = [peak_normalize(s.copy(), 0.999) for s in stems]
    gains = [float(np.max(np.abs(n))) / float(np.max(np.abs(s))) for n, s in zip(normalised, stems, strict=True)]

    # Different gains per stem - that is the whole defect.
    assert gains[0] != pytest.approx(gains[2], abs=1e-6)
    assert float(np.max(np.abs(mix - sum(normalised)))) > 0.1


def test_the_flag_is_plumbed_from_the_cli_to_the_fusion():
    """Three hops; a break in any of them silently reverts the default."""
    from stemlab import cli, hybrid, pipeline

    assert '"--normalize-fused-stems"' in inspect.getsource(cli.separate_main)
    assert "normalize_fused=args.normalize_fused_stems" in inspect.getsource(cli.separate_main)
    assert "normalize=normalize_fused" in inspect.getsource(pipeline.separate)
    assert "normalize=normalize," in inspect.getsource(hybrid.fuse_stem_folders)
    assert "if normalize:" in inspect.getsource(hybrid.fuse_stem_pair)
