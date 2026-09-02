"""Fuse RoFormer and Demucs estimates in the time-frequency domain."""

from __future__ import annotations

import math
import os
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Callable

import numpy as np

from .audio import STEM_NAMES, find_stem_file, load_audio, peak_normalize, save_audio

# scipy is imported inside the transform below rather than here. scipy.fft
# costs tenths of a second to import, and this module is on the import path
# of every plugin-launched job - including the ones that never fuse anything:
# a single-engine separation, a --no-refine run, the model manager. Fusion
# itself pays it once, minutes into a job, where it disappears.

# RoFormer preference when the two models disagree strongly. Agreement tends
# toward an even 50/50 fusion. These are intentionally conservative rather
# than pretending that either model is universally better.
ROFORMER_PREFERENCE = {
    "vocals": 0.64,
    "drums": 0.46,
    "bass": 0.54,
    "guitar": 0.60,
    "piano": 0.60,
    "other": 0.55,
}


# The analysis window by length. A run asks for two at most: the full 2048,
# and whatever a block shorter than that clamps to. Written from two threads
# without a lock, which is safe because both would store the same window.
_WINDOWS: dict[int, np.ndarray] = {}


def _analysis_window(nperseg: int) -> np.ndarray:
    """The periodic Hann window of ``nperseg`` points.

    ``np.hanning`` is the symmetric window; dropping the last point of the
    next size up is the periodic one, which is what an overlap-added STFT
    needs. It is the same window scipy would build - identical in float32 at
    every length this asks for - so the transforms below stay comparable
    with the scipy pair they replaced.
    """
    window = _WINDOWS.get(nperseg)

    if window is None:
        window = np.hanning(nperseg + 1)[:-1].astype(np.float32)
        _WINDOWS[nperseg] = window

    return window


def _forward_stft(audio: np.ndarray, nperseg: int, noverlap: int) -> np.ndarray:
    """Transform ``[..., samples]`` to ``[..., frames, bins]``.

    This is what ``scipy.signal.stft(boundary="zeros", padded=True)`` does,
    written out, because how it does it costs more than the transform. It
    casts its window to complex before applying it, so every frame is built
    as a complex array twice the size of the real one it is immediately
    reduced to; and it returns the bins before the frames, which leaves the
    inverse transform reading down a strided axis. Together those were about
    half of the time a fused stem took.

    The frequency axis is last here for that reason. The fusion is elementwise
    and does not care which way round the two axes are, and both transforms
    then run along contiguous rows.
    """
    from scipy.fft import rfft

    nstep = nperseg - noverlap
    window = _analysis_window(nperseg)

    # boundary="zeros": half a window of silence at each end, so the samples
    # at the edges are covered by as many frames as those in the middle and
    # the inverse can restore them at full level.
    half = nperseg // 2
    padded = np.pad(audio, [(0, 0)] * (audio.ndim - 1) + [(half, half)])

    # padded=True: enough further zeros for a whole number of frames, so no
    # tail is dropped. The inverse takes them back off.
    tail = (-(padded.shape[-1] - nperseg) % nstep) % nperseg

    if tail:
        padded = np.pad(padded, [(0, 0)] * (padded.ndim - 1) + [(0, tail)])

    frames = np.lib.stride_tricks.sliding_window_view(padded, nperseg, axis=-1)[
        ..., ::nstep, :
    ]

    # scipy's 1/sum(window) scaling, kept rather than dropped as a constant
    # that would cancel: the fusion adds a fixed epsilon to magnitudes, so
    # the level this reports decides what that epsilon is worth.
    return rfft(frames * window, n=nperseg, axis=-1) * np.float32(1.0 / window.sum())


def _inverse_stft(spectra: np.ndarray, nperseg: int, noverlap: int) -> np.ndarray:
    """Overlap-add ``[..., frames, bins]`` back to ``[..., samples]``.

    The inverse of _forward_stft, and of ``scipy.signal.istft(boundary=True)``:
    each frame is windowed a second time, the frames are added back at their
    hop, and the sum is divided by the overlap-added squared window so that
    the double windowing cancels.

    The overlap-add runs one hop phase at a time rather than one frame at a
    time. Frames a whole number of hops apart never overlap within a phase,
    so a thousand of them are added in a single numpy operation; scipy's own
    loop body runs once per frame, and at 512 samples of hop there are more
    than thirteen hundred frames in every sixteen-second chunk.
    """
    from scipy.fft import irfft

    nstep = nperseg - noverlap
    window = _analysis_window(nperseg)

    frames = irfft(spectra, n=nperseg, axis=-1) * (window * np.float32(window.sum()))
    squared = window * window

    # A frame is cut into whole hops so the phases line up. The last hop is
    # zero-filled where the window is not a whole number of hops long.
    phases = -(-nperseg // nstep)
    padding = phases * nstep - nperseg

    if padding:
        frames = np.pad(frames, [(0, 0)] * (frames.ndim - 1) + [(0, padding)])
        squared = np.pad(squared, (0, padding))

    count = frames.shape[-2]
    blocks = np.zeros(frames.shape[:-2] + (count + phases - 1, nstep), dtype=np.float32)
    weights = np.zeros((count + phases - 1, nstep), dtype=np.float32)

    frames = frames.reshape(frames.shape[:-1] + (phases, nstep))
    squared = squared.reshape(phases, nstep)

    for phase in range(phases):
        blocks[..., phase : phase + count, :] += frames[..., phase, :]
        weights[phase : phase + count, :] += squared[phase]

    length = nperseg + (count - 1) * nstep
    audio = blocks.reshape(blocks.shape[:-2] + (-1,))[..., :length]
    envelope = weights.reshape(-1)[:length]

    # Off with the half-window of boundary padding at each end.
    half = nperseg // 2
    audio = audio[..., half : length - half]
    envelope = envelope[half : length - half]

    # Where the windows sum to nothing there is nothing to recover, and
    # dividing would only amplify rounding noise. scipy's floor, kept.
    return audio / np.where(envelope > 1e-10, envelope, np.float32(1.0))


def _pad_to_length(audio: np.ndarray, length: int) -> np.ndarray:
    if audio.shape[1] == length:
        return audio

    if audio.shape[1] > length:
        return audio[:, :length]

    pad = length - audio.shape[1]
    return np.pad(
        audio,
        ((0, 0), (0, pad)),
        mode="constant",
    )


def _fuse_channels(
    roformer: np.ndarray,
    demucs: np.ndarray,
    roformer_preference: float,
    n_fft: int = 2048,
    hop: int = 512,
) -> np.ndarray:
    """Fuse matching ``[channels, samples]`` blocks in one batched transform.

    The transforms operate along the last axis, so every channel goes through
    a single call; the fusion math itself is elementwise and is bit-identical
    to transforming each channel on its own.
    """
    # A frame cannot be longer than the block it analyses, and the overlap
    # cannot reach the whole frame. Clamp both to what this block supports:
    # a sub-frame remainder is analysed with a smaller frame rather than
    # refused.
    frames = int(roformer.shape[-1])
    nperseg = min(n_fft, frames)
    noverlap = min(n_fft - hop, nperseg - 1)

    if nperseg < 2:
        # Sub-frame remainder: a straight blend is indistinguishable from a
        # transform at this length and cannot fail.
        blended = roformer_preference * roformer + (1.0 - roformer_preference) * demucs
        return blended.astype(np.float32, copy=False)

    zr = _forward_stft(roformer, nperseg, noverlap)
    zd = _forward_stft(demucs, nperseg, noverlap)

    mag_r = np.abs(zr)
    mag_d = np.abs(zd)

    eps = np.float32(1e-7)

    # 1 when the two models agree on local magnitude, approaching 0 as their
    # estimates diverge. The log-ratio makes equal relative disagreements act
    # similarly at quiet and loud levels.
    disagreement = np.abs(np.log((mag_r + eps) / (mag_d + eps)))
    agreement = np.exp(-disagreement).astype(np.float32)

    # Agreement -> 50/50 consensus.
    # Disagreement -> gently favor the model with the stronger prior for this
    # stem instead of blindly averaging contradictory estimates.
    weight_r = (0.5 * agreement + roformer_preference * (1.0 - agreement)).astype(np.float32)

    weight_d = 1.0 - weight_r

    mixed_complex = weight_r * zr + weight_d * zd

    target_magnitude = weight_r * mag_r + weight_d * mag_d

    # Straight complex averaging can accidentally cancel when the models have
    # small phase differences. Restore the intended weighted magnitude while
    # retaining the blended phase.
    mixed_magnitude = np.abs(mixed_complex)
    fused = mixed_complex * (target_magnitude / (mixed_magnitude + eps))

    audio = _inverse_stft(fused, nperseg, noverlap)

    return np.asarray(
        audio[..., :frames],
        dtype=np.float32,
    )


def _fuse_audio_chunk(
    roformer: np.ndarray,
    demucs: np.ndarray,
    stem: str,
) -> np.ndarray:
    channels = min(
        roformer.shape[0],
        demucs.shape[0],
    )

    return _fuse_channels(
        roformer[:channels],
        demucs[:channels],
        ROFORMER_PREFERENCE.get(stem, 0.55),
    )


def fuse_stem_pair(
    roformer_path: Path,
    demucs_path: Path,
    output_path: Path,
    stem: str,
    chunk_seconds: float = 16.0,
    overlap_seconds: float = 0.35,
    normalize: bool = False,
) -> tuple[np.ndarray, int]:
    """Fuse one matching pair of model outputs and write the result.

    ``normalize`` scales this stem so its own peak sits at 0.999. It is off
    by default because the scale factor is derived from this stem alone: a
    loud stem is attenuated and a quiet one is not, so the six stems stop
    summing back to the mix they came from and their balance against each
    other shifts. Stems are written as 32-bit float, where a sample above
    1.0 is exactly representable, so nothing clips in the file either way.

    Returns the exact ``[channels, samples]`` float32 audio and sample rate
    just written, so a same-process caller can keep working on the array
    without decoding the file again.
    """
    roformer, sr = load_audio(
        roformer_path,
        stereo=True,
    )

    demucs, _ = load_audio(
        demucs_path,
        target_sr=sr,
        stereo=True,
    )

    length = max(
        roformer.shape[1],
        demucs.shape[1],
    )

    roformer = _pad_to_length(roformer, length)
    demucs = _pad_to_length(demucs, length)

    result = np.zeros_like(
        roformer,
        dtype=np.float32,
    )

    weight = np.zeros(
        length,
        dtype=np.float32,
    )

    chunk = max(
        4096,
        int(round(chunk_seconds * sr)),
    )

    overlap = max(
        0,
        min(
            chunk // 3,
            int(round(overlap_seconds * sr)),
        ),
    )

    step = max(
        1,
        chunk - overlap,
    )

    # No special case for a short trailing chunk. There used to be one - a
    # tail under one analysis frame was pulled back into a full-length final
    # chunk - because scipy shrank nperseg to the block and then refused the
    # unchanged noverlap. _fuse_channels clamps both to what the block can
    # support, so any tail length transforms on its own, and the pull-back
    # could only ever fire below about 11 kHz anyway: above it the previous
    # chunk already reaches past the end of the track and the loop stops.
    for start in range(0, length, step):
        end = min(
            length,
            start + chunk,
        )

        fused = _fuse_audio_chunk(
            roformer[:, start:end],
            demucs[:, start:end],
            stem,
        )

        # _fuse_channels returns exactly as many samples as it was given, at
        # every length including the sub-frame ones, so the block below lines
        # up with the slice it is added into without padding or trimming.
        local_length = end - start

        envelope = np.ones(
            local_length,
            dtype=np.float32,
        )

        if overlap > 0:
            fade = min(
                overlap,
                local_length // 2,
            )

            if start > 0 and fade > 0:
                phase = np.linspace(
                    0.0,
                    math.pi / 2.0,
                    fade,
                    endpoint=True,
                    dtype=np.float32,
                )
                envelope[:fade] = np.sin(phase) ** 2

            if end < length and fade > 0:
                phase = np.linspace(
                    math.pi / 2.0,
                    0.0,
                    fade,
                    endpoint=True,
                    dtype=np.float32,
                )
                envelope[-fade:] = np.sin(phase) ** 2

        result[:, start:end] += fused * envelope[None, :]

        weight[start:end] += envelope

        if end >= length:
            break

    result /= np.maximum(
        weight[None, :],
        1e-6,
    )

    if normalize:
        result = peak_normalize(result, peak=0.999)

    save_audio(
        output_path,
        result,
        sr,
    )

    return result, sr


def fuse_stem_folders(
    roformer_dir: str | Path,
    demucs_dir: str | Path,
    output_dir: str | Path,
    log_callback: Callable[[str], None] | None = None,
    progress_callback: Callable[[int, int, str], None] | None = None,
    ready_callback: Callable[[str, Path], None] | None = None,
    fused_callback: Callable[[str, np.ndarray, int], None] | None = None,
    normalize: bool = False,
) -> list[Path]:
    """Fuse all six named stems from two model-output directories.

    The stems are independent, so they run on a small thread pool: scipy's
    FFT work releases the GIL, and threads share the audio buffers that
    worker processes would have to re-import and copy.

    ``progress_callback(count, total, stem)`` stays monotonic regardless of
    which stem finishes first - ``count`` is the number of finished stems,
    ``stem`` names one still in flight - and ends with ``(total, total,
    "")`` once everything is written. ``ready_callback(stem, path)`` fires
    once per stem as it lands, naming the stem that actually finished
    rather than the in-flight one the progress label carries. All callbacks
    fire on the calling thread. ``fused_callback(stem, audio, sr)`` hands
    over the ``[channels, samples]`` float32 array exactly as written to
    disk, letting a same-process caller skip re-decoding that file. It is
    a handover, not a collection: nothing here references the array once
    the callback returns, so a stem the caller does not keep is freed while
    the rest of the folder is still fusing.
    """
    roformer_dir = Path(roformer_dir)
    demucs_dir = Path(demucs_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Resolve every input before any work starts so a missing stem fails
    # the job before compute is spent, never after five stems are fused.
    jobs: list[tuple[str, Path, Path, Path]] = []

    for stem in STEM_NAMES:
        roformer_path = find_stem_file(
            roformer_dir,
            stem,
        )
        demucs_path = find_stem_file(
            demucs_dir,
            stem,
        )
        if roformer_path is None or demucs_path is None:
            missing = roformer_dir if roformer_path is None else demucs_dir
            raise FileNotFoundError(f"Could not find {stem} in {missing}")

        jobs.append((stem, roformer_path, demucs_path, output_dir / f"{stem}.wav"))

    if log_callback:
        for stem, *_ in jobs:
            log_callback(
                f"Hybrid fusion: {stem} "
                f"(RoFormer preference {ROFORMER_PREFERENCE.get(stem, 0.55):.2f})"
            )

    total = len(jobs)

    if progress_callback:
        progress_callback(0, total, jobs[0][0])

    # Cancellation arrives as an exception out of a progress or log callback
    # (or a worker); stems that have not started yet must then never start.
    abort = threading.Event()

    # A future holds its result until the future itself is dropped, and
    # as_completed keeps every future for the length of the loop below, so
    # returning fused audio would pin all six full-length arrays until the
    # whole folder is done. A worker parks its result here instead and the
    # loop lifts it straight back out, leaving only the stems still in
    # flight and whatever the caller chose to keep.
    handoff: dict[str, tuple[np.ndarray, int]] = {}

    def fuse_one(
        stem: str,
        roformer_path: Path,
        demucs_path: Path,
        output_path: Path,
    ) -> bool:
        if abort.is_set():
            return False

        fused = fuse_stem_pair(
            roformer_path=roformer_path,
            demucs_path=demucs_path,
            output_path=output_path,
            stem=stem,
            normalize=normalize,
        )

        if fused_callback is not None:
            handoff[stem] = fused

        return True

    pending = [stem for stem, *_ in jobs]
    finished = 0

    outputs = {stem: output_path for stem, _roformer, _demucs, output_path in jobs}

    # Two workers, not more: each in-flight stem holds three full-length
    # stereo arrays plus STFT scratch, and fusion runs right after the model
    # stages already pushed peak memory - halving the concurrency keeps the
    # gain (the tail is the slowest stem, not the sum) without doubling the
    # footprint again.
    with ThreadPoolExecutor(max_workers=min(2, os.cpu_count() or 1)) as pool:
        futures = {
            pool.submit(fuse_one, stem, roformer_path, demucs_path, output_path): stem
            for stem, roformer_path, demucs_path, output_path in jobs
        }

        try:
            for future in as_completed(futures):
                written = future.result()
                stem = futures[future]

                if not written:
                    continue

                if fused_callback is not None:
                    # Unpacked straight out of the parking slot: no local
                    # here outlives the call, so the array survives only as
                    # long as the callback wants it.
                    fused_callback(stem, *handoff.pop(stem))

                pending.remove(stem)
                finished += 1

                if ready_callback:
                    # The stem this future carried, not the one the progress
                    # label below names: that one is still in flight.
                    ready_callback(stem, outputs[stem])

                # Monotonic by construction: the finished count only grows,
                # and the label names work still in progress.
                if pending and progress_callback:
                    progress_callback(finished, total, pending[0])
        except BaseException:
            abort.set()
            for future in futures:
                future.cancel()
            # A propagating exception keeps this frame alive through its
            # traceback; anything already parked would ride along with it.
            handoff.clear()
            raise

    if progress_callback:
        progress_callback(total, total, "")

    return [output_path for *_, output_path in jobs]
