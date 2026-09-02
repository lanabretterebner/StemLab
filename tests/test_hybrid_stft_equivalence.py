"""The rewritten hybrid transform must be worth exactly what scipy's was.

``hybrid._fuse_channels`` used to reach ``scipy.signal.stft``/``istft``. It
now frames the block itself and calls ``scipy.fft.rfft``, because how scipy's
legacy pair does it - a window promoted to complex, the bins returned ahead
of the frames, an overlap-add loop running once per frame - was about half of
the time a fused stem took.

Nothing else in the suite would notice the two drifting apart. A fusion that
is subtly wrong still writes six stems of the right length that still sum
roughly back to the mix, and what changed is the audio people listen to. So
this is a permanent guard rather than a one-off check on the rewrite: the
scipy path below is the code that was replaced, kept verbatim, and each block
length the fusion actually produces is fused both ways and compared.

The two are not bit-identical and are not meant to be - they reduce in a
different order, and the frames are windowed in float32 here rather than
promoted to complex - so the comparison is on relative deviation. It measures
at worst about 2e-6 across these cases, against a 1e-5 gate: room for the
last bits to move, none for the arithmetic to.
"""

from __future__ import annotations

import numpy as np
import pytest
from scipy.signal import istft, stft

import stemlab.hybrid as hybrid
from stemlab.hybrid import ROFORMER_PREFERENCE

SAMPLE_RATE = 44100

# Every block length that reaches the transform, and why each is here.
#     1  a single sample: shorter than any frame, so there is nothing to
#        transform and both paths fall back to a straight blend
#     2  the shortest block that is transformed at all
#  1000  a sub-frame remainder, where the frame is clamped to the block
#  1536  the other one, and the length that used to make scipy raise
#  2048  exactly one full analysis frame
#  5000  the first length past a full frame, so the transform runs at its
#        unclamped 2048/512 size
# 44100  one second, several hops of overlap-add
# 705600 the sixteen-second chunk the pipeline actually fuses in production
BLOCK_LENGTHS = (1, 2, 1000, 1536, 2048, 5000, 44100, 705600)

# One of the real per-stem priors rather than a made-up number, so the
# weighting the comparison runs through is the weighting a job runs through.
PREFERENCE = ROFORMER_PREFERENCE["vocals"]

# What the two paths may differ by, as a fraction of the fused peak.
TOLERANCE = 1e-5


def _estimates(length: int, channels: int) -> tuple[np.ndarray, np.ndarray]:
    """Two model estimates of the same passage, seeded from the case itself.

    They have to disagree: the fusion weights every bin by how far apart the
    two magnitudes are, so handing it one array twice would leave the whole
    agreement path - the part with the logarithm and the phase recovery in
    it - taking the same branch everywhere.
    """
    rng = np.random.default_rng((length, channels, 2026_09_02))

    time = np.arange(length, dtype=np.float32) / SAMPLE_RATE
    tone = 0.30 * np.sin(2 * np.pi * 220.0 * time) + 0.10 * np.sin(2 * np.pi * 1330.0 * time)

    roformer = tone + rng.normal(0.0, 0.05, (channels, length))
    demucs = 0.9 * tone + rng.normal(0.0, 0.08, (channels, length))

    return roformer.astype(np.float32), demucs.astype(np.float32)


def _scipy_fuse_channels(
    roformer: np.ndarray,
    demucs: np.ndarray,
    roformer_preference: float,
    n_fft: int = 2048,
    hop: int = 512,
) -> np.ndarray:
    """The fusion as it read before the rewrite, scipy transforms and all.

    Kept whole, including the fusion arithmetic between the two transforms:
    a reference that only covered the transforms would compare the halves
    that were rewritten while assuming the half that was not.
    """
    frames = int(roformer.shape[-1])
    nperseg = min(n_fft, frames)
    noverlap = min(n_fft - hop, nperseg - 1)

    if nperseg < 2:
        blended = roformer_preference * roformer + (1.0 - roformer_preference) * demucs
        return blended.astype(np.float32, copy=False)

    _, _, zr = stft(
        roformer,
        nperseg=nperseg,
        noverlap=noverlap,
        window="hann",
        boundary="zeros",
        padded=True,
    )

    _, _, zd = stft(
        demucs,
        nperseg=nperseg,
        noverlap=noverlap,
        window="hann",
        boundary="zeros",
        padded=True,
    )

    mag_r = np.abs(zr)
    mag_d = np.abs(zd)

    eps = np.float32(1e-7)

    disagreement = np.abs(np.log((mag_r + eps) / (mag_d + eps)))
    agreement = np.exp(-disagreement).astype(np.float32)

    weight_r = (0.5 * agreement + roformer_preference * (1.0 - agreement)).astype(np.float32)
    weight_d = 1.0 - weight_r

    mixed_complex = weight_r * zr + weight_d * zd
    target_magnitude = weight_r * mag_r + weight_d * mag_d

    mixed_magnitude = np.abs(mixed_complex)
    fused = mixed_complex * (target_magnitude / (mixed_magnitude + eps))

    _, audio = istft(
        fused,
        nperseg=nperseg,
        noverlap=noverlap,
        window="hann",
        input_onesided=True,
        boundary=True,
    )

    return np.asarray(audio[..., :frames], dtype=np.float32)


def _relative_deviation(fused: np.ndarray, reference: np.ndarray) -> float:
    """The largest sample-wise difference, as a fraction of the fused peak."""
    scale = max(float(np.max(np.abs(reference))), 1e-9)
    return float(np.max(np.abs(fused - reference))) / scale


@pytest.mark.parametrize("channels", (1, 2))
@pytest.mark.parametrize("length", BLOCK_LENGTHS)
def test_the_private_transform_fuses_what_scipy_fused(length: int, channels: int):
    roformer, demucs = _estimates(length, channels)

    fused = hybrid._fuse_channels(roformer, demucs, PREFERENCE)
    reference = _scipy_fuse_channels(roformer, demucs, PREFERENCE)

    # Length first: a transform that dropped or padded a tail would otherwise
    # fail below as a numerical difference, which is a misleading way to be
    # told that the fused stem no longer lines up with the track.
    assert fused.shape == (channels, length)
    assert reference.shape == fused.shape
    assert fused.dtype == np.float32

    deviation = _relative_deviation(fused, reference)

    assert deviation < TOLERANCE, (
        f"{channels} channel(s) x {length} samples: the transform now differs "
        f"from scipy's by {deviation:.2e} of the peak"
    )


def test_a_block_of_pure_silence_stays_silent():
    """The epsilons must not leak a floor into a stem that has nothing in it.

    Both paths divide by a magnitude plus 1e-7 and by an overlap-added window
    envelope; either could turn digital black into a whisper, which on a six
    stem job is six whispers under every quiet passage.
    """
    silence = np.zeros((2, 5000), dtype=np.float32)

    fused = hybrid._fuse_channels(silence, silence, PREFERENCE)

    assert fused.shape == silence.shape
    assert not np.any(fused)
