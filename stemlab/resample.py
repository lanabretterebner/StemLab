"""Sample-rate conversion shared by the separation backends.

Backends disagree about what rate they emit. BS-RoFormer is fed 44.1 kHz on
purpose, because its band edges are fixed in FFT bins; Demucs is never asked
at all and resamples to its own model rate whatever it is given. Both
therefore have to put audio back on the source's rate before the plugin will
play it alongside everything else, and they share one resampler so they share
its length guarantee.
"""

from __future__ import annotations

from collections.abc import Callable
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


def rate_and_frames(path: Path) -> tuple[int, int]:
    """Report a file's sample rate and length without decoding its audio."""
    import soundfile as sf

    info = sf.info(str(path))
    return int(info.samplerate), int(info.frames)


def restore_folder_sample_rate(
    output_dir: Path,
    sample_rate: int,
    frames: int | None,
    log: Callable[[str], None],
) -> None:
    """Return stems written at a model's own rate to the source's rate.

    Both pretrained backends need this, for different reasons. BS-RoFormer
    is deliberately fed 44.1 kHz because its band edges are fixed in bins,
    and fusion then reads its sample rate from the RoFormer stem itself
    (``hybrid.fuse_stem_pair`` loads it with no ``target_sr``), so a stem
    left behind at 44.1 kHz would make every fused output of a 48 kHz
    session 44.1 kHz too. Demucs is not asked at all - it resamples to its
    model rate on its own and writes there - so without this a demucs-only
    job published 44.1 kHz stems whatever the session ran at.

    ``frames`` of None means the source length could not be read, which is
    not the same as a length of zero: the resampler's own output length is
    kept rather than truncating every stem to silence.
    """
    import soundfile as sf

    failed: list[str] = []

    for path in sorted(output_dir.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in {".wav", ".flac"}:
            continue

        try:
            rate = sf.info(str(path)).samplerate
        except Exception as exc:
            # Guarded, and deliberately not fatal. output_dir holds whatever
            # the model left behind, and rglob is sorted, so one file
            # soundfile cannot open - a partial write, something
            # _clear_audio_files could not unlink - used to cost every stem
            # sorting after it. A file that cannot be read is also not a file
            # this can mis-rate, so it is reported and stepped over.
            log(f"Could not read the sample rate of {path.name}: {exc}")
            continue

        if rate == sample_rate:
            continue

        restored = path.with_name(f"{path.stem}_stemlab_rate{path.suffix}")
        try:
            resample_file(path, restored, sample_rate, out_frames=frames)
            # A file cannot be rewritten underneath its own reader, so the
            # resample lands beside the stem and then takes its place.
            restored.replace(path)
        except Exception as exc:
            restored.unlink(missing_ok=True)
            # Readable, at the wrong rate, and not fixable: this one really
            # would go out mis-rated, so it is collected rather than logged
            # and forgotten.
            failed.append(path.name)
            log(f"Could not return {path.name} to {sample_rate} Hz: {exc}")

    if failed:
        # Not a warning to be scrolled past. Fusion reads its rate off the
        # RoFormer stem, so a stem left at 44.1 kHz in a 48 kHz session makes
        # every fused output 44.1 kHz - which is the exact state this function
        # exists to prevent, and it is invisible in the audio. Better to fail
        # the separation than to hand back a session that is quietly wrong.
        raise RuntimeError(
            "Could not return "
            + ", ".join(failed)
            + f" to the source rate of {sample_rate} Hz. They would "
            "otherwise be left at the model's own rate and silently "
            "mis-rate the session."
        )
