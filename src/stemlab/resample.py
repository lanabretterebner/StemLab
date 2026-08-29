"""Sample-rate conversion shared by every StemLab separation backend.

Backends disagree about what rate they emit - BS-RoFormer is fed 44.1 kHz on
purpose, and audio-separator's VR backend returns 44.1 kHz whatever it is
asked for - so more than one of them has to put audio back on the session's
rate before the plugin will play it alongside everything else. They share one
resampler so they share its length guarantee.
"""

from __future__ import annotations

from pathlib import Path

# Resampling happens in blocks, not whole files: decoding a six-minute
# stereo track and resampling it in one call measured 251 MB of extra peak
# memory against none at all streamed, and it lands immediately before the
# separator loads its model. Block and whole-file results are identical.
RESAMPLE_BLOCK_FRAMES = 1 << 16


def resample_file(
    source: Path,
    destination: Path,
    out_rate: int,
    out_frames: int | None = None,
    widen_narrow_pcm: bool = False,
) -> int:
    """Rewrite ``source`` at ``out_rate`` and return the frames written.

    ``out_frames`` is supplied by the caller rather than computed here
    because soxr's output length is not a stable function of the input
    length: resampling 48 kHz to 44.1 kHz gave ``ceil(n * out / in)`` for
    4099 frames but one frame less for 65537 and for 999983. A stem that
    is a sample longer or shorter than the session drifts against every
    other track, so the length is trimmed or zero-padded to what was asked
    for instead of being trusted.
    """
    import numpy as np
    import soundfile as sf
    import soxr

    info = sf.info(str(source))
    subtype = info.subtype

    if widen_narrow_pcm and subtype in {"PCM_S8", "PCM_U8", "PCM_16"}:
        # Only ever the model's throwaway input file, which is written once
        # and read once: widening keeps the extra trip through 44.1 kHz from
        # re-quantising it, and PCM_24 is already what the ffmpeg branch
        # stages. Stems keep whatever width they were written at - those are
        # the deliverable, and nothing may change their format silently.
        subtype = "PCM_24"

    resampler = soxr.ResampleStream(
        info.samplerate,
        out_rate,
        info.channels,
        dtype="float32",
        quality="HQ",
    )
    written = 0

    with (
        sf.SoundFile(str(source)) as reader,
        sf.SoundFile(
            str(destination),
            "w",
            samplerate=out_rate,
            channels=info.channels,
            subtype=subtype,
        ) as writer,
    ):
        while True:
            # soundfile hands back (frames, channels), which is the layout
            # soxr expects. Nothing here may transpose: a channel-major
            # array is read as a handful of frames with thousands of
            # channels and is returned silently unresampled.
            block = reader.read(RESAMPLE_BLOCK_FRAMES, dtype="float32", always_2d=True)
            last = block.shape[0] < RESAMPLE_BLOCK_FRAMES
            resampled = resampler.resample_chunk(block, last=last)

            if out_frames is not None and written + resampled.shape[0] > out_frames:
                resampled = resampled[: max(0, out_frames - written)]

            if resampled.shape[0]:
                writer.write(resampled)
                written += resampled.shape[0]

            if last:
                break

        if out_frames is not None and written < out_frames:
            writer.write(np.zeros((out_frames - written, info.channels), dtype="float32"))
            written = out_frames

    return written
