"""BS-RoFormer backend: stage input audio and run ``bs-roformer-infer``."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from .device import resolve_torch_device
from .resample import (
    rate_and_frames as _rate_and_frames,
    resample_file as _resample_file,
    restore_folder_sample_rate as _restore_stem_sample_rate,
)
from .runtime import CancellationToken, run_progress_process

DEFAULT_MODEL = "roformer-model-bs-roformer-sw-by-jarredou"

# BS-RoFormer is trained at 44.1 kHz and its band-split boundaries are fixed
# in bins, not in Hz: fed 48 kHz audio the model applies every learned band
# edge 8.8% too high (~1.5 semitones), which costs separation quality and
# adds bleed. The upstream CLI does not resample, so StemLab does it here.
ROFORMER_SAMPLE_RATE = 44100


def build_roformer_command(
    input_folder: Path,
    store_dir: Path,
    device: str,
    model: str,
) -> list[str]:
    """The argv that runs BS-RoFormer over one staging folder.

    Extracted so the cache warm-up can run byte-for-byte the same invocation
    a separation does. An inductor entry is keyed on the graph, so a warm-up
    that reached the model any other way could trace shapes no job asks for
    and fill the cache with kernels nothing reads - a failure with no error
    anywhere. One builder means there is nothing to keep in step.
    """
    return [
        sys.executable,
        "-m",
        "stemlab.bs_roformer_cli",
        "--input_folder",
        str(input_folder),
        "--store_dir",
        str(store_dir),
        "--device",
        device,
        "--model",
        model,
    ]


def _find_ffmpeg() -> str | None:
    """Locate ffmpeg next to this interpreter, then on PATH."""
    python_dir = Path(sys.executable).resolve().parent
    for candidate in (python_dir / "ffmpeg.exe", python_dir / "ffmpeg"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("ffmpeg")


def _clear_audio_files(folder: Path) -> None:
    """Remove audio left by an earlier run so lookups cannot see stale stems."""
    if not folder.is_dir():
        return

    for path in folder.rglob("*"):
        if path.is_file() and path.suffix.lower() in {".wav", ".flac"}:
            try:
                path.unlink()
            except OSError:
                # A file another process holds open is not worth failing a
                # separation over; it is overwritten or ignored downstream.
                pass


def _canonicalise_output_names(
    output_dir: Path,
    track_prefix: str,
    log: Callable[[str], None],
) -> None:
    """Rename "{track}_{instrument}.wav" outputs to plain "{instrument}.wav"."""
    prefix = f"{track_prefix}_"

    for path in sorted(output_dir.glob(f"{prefix}*")):
        if not path.is_file() or path.suffix.lower() not in {".wav", ".flac"}:
            continue

        instrument = path.stem[len(prefix) :].strip().lower()

        if not instrument:
            continue

        target = path.with_name(f"{instrument}{path.suffix.lower()}")

        if target == path:
            continue

        try:
            path.replace(target)
        except OSError as exc:
            # Keep the original name rather than losing the stem; the
            # tiered lookup in audio.find_stem_file still resolves it.
            log(f"Could not rename {path.name} to {target.name}: {exc}")


def _convert_with_soundfile(input_path: Path, staged: Path) -> bool:
    """Rewrite a losslessly-decodable input as WAV without needing ffmpeg."""
    try:
        import soundfile as sf

        # Read at the width being written. Without a dtype soundfile decodes
        # to float64 - eight bytes a sample, twice the peak this holds - for
        # data that goes straight back out as PCM_24, which is what libsndfile
        # produces from int32 by dropping the low eight bits. The whole track
        # is in memory at once here, immediately before the separator loads
        # its model, and the hybrid engine pays for it twice.
        data, sample_rate = sf.read(str(input_path), always_2d=True, dtype="int32")
        sf.write(str(staged), data, sample_rate, subtype="PCM_24")
    except Exception:
        return False

    return staged.exists()


def _normalise_input_for_backend(
    input_path: Path,
    staging_dir: Path,
    log: Callable[[str], None],
    passthrough_extensions: set[str] | None = None,
    cancellation: CancellationToken | None = None,
) -> Path:
    """Return audio the separator can decode, converting when it cannot.

    ``passthrough_extensions`` is what the calling backend reads natively.
    It matters because the backends disagree: Demucs decodes FLAC happily,
    while the BS-RoFormer CLI discovers its input with a case-sensitive
    ``glob("*.wav")`` and sees nothing else - a staged FLAC (or an
    upper-case ``SONG.WAV``) made it fail with "No .wav files found".
    """
    if passthrough_extensions is None:
        passthrough_extensions = {".wav", ".flac"}

    extension = input_path.suffix.lower()

    if extension in passthrough_extensions:
        # Always stage under the lower-cased extension so a case-sensitive
        # backend glob still finds the file on Linux.
        staged = staging_dir / f"{input_path.stem}{extension}"
        shutil.copy2(input_path, staged)
        return staged

    if extension == ".flac":
        # Lossless and already a soundfile dependency, so this needs no
        # ffmpeg install just to feed FLAC to a WAV-only backend.
        staged = staging_dir / f"{input_path.stem}_stemlab_input.wav"
        log("Converting FLAC to WAV for the separator...")

        if _convert_with_soundfile(input_path, staged):
            return staged

        log("soundfile could not decode the FLAC; falling back to ffmpeg.")

    ffmpeg = _find_ffmpeg()
    if ffmpeg is None:
        raise RuntimeError(
            f"StemLab needs FFmpeg to open {extension or 'this audio format'}, "
            "but ffmpeg was not found on PATH."
        )

    staged = staging_dir / f"{input_path.stem}_stemlab_input.wav"
    log(f"Converting {extension or 'input audio'} to WAV for the separator...")
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(input_path),
        "-vn",
        "-ac",
        "2",
        "-c:a",
        "pcm_s24le",
        str(staged),
    ]
    exit_code = run_progress_process(
        command,
        log,
        lambda _percent: None,
        cancellation=cancellation,
    )
    if exit_code != 0 or not staged.exists():
        raise RuntimeError("FFmpeg could not convert the input audio.")
    return staged


class RoFormerBackend:
    """Adapter around the documented ``bs-roformer-infer`` CLI."""

    def __init__(
        self,
        model: str = DEFAULT_MODEL,
        device: str = "cuda",
        log_callback: Callable[[str], None] | None = None,
        progress_callback: Callable[[float], None] | None = None,
        eta_callback: Callable[[float], None] | None = None,
        download_callback: Callable[[float], None] | None = None,
        cancellation: CancellationToken | None = None,
    ) -> None:
        """Configure the model, device, logging, and progress callbacks."""
        self.model = model
        self.device = device
        self.log_callback = log_callback
        self.progress_callback = progress_callback
        self.eta_callback = eta_callback
        self.download_callback = download_callback
        self.cancellation = cancellation

    def _log(self, message: str) -> None:
        if self.log_callback:
            self.log_callback(message)
        else:
            print(message, flush=True)

    def _progress(self, percent: float) -> None:
        if self.progress_callback:
            self.progress_callback(max(0.0, min(100.0, float(percent))))

    def separate(
        self,
        input_path: str | Path,
        output_dir: str | Path,
    ) -> list[Path]:
        """Run RoFormer into ``output_dir`` and return the written stem files."""
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        device = resolve_torch_device(self.device, self._log)

        # Outputs are named after the input track, so a reused output
        # directory would accumulate one set per run instead of overwriting.
        # Stem lookup would then be free to pick a previous song's audio.
        _clear_audio_files(output_dir)

        with tempfile.TemporaryDirectory(prefix="stemlab_input_") as td:
            staging = Path(td)
            staged = _normalise_input_for_backend(
                input_path=input_path,
                staging_dir=staging,
                log=self._log,
                # The upstream CLI only ever discovers "*.wav".
                passthrough_extensions={".wav"},
                cancellation=self.cancellation,
            )

            # Read the rate off the staged file rather than the original:
            # this is literally what the model opens, it is always a WAV
            # soundfile can read, and for a lossy source its frame count is
            # the alignment reference the stems have to match.
            try:
                source_rate, source_frames = _rate_and_frames(staged)
            except Exception as exc:
                # The work order's hard requirement is that a 44.1 kHz input
                # takes the path it took before. A file whose header this
                # cannot parse is one the CLI may still handle - it is the
                # model's business, not ours - so this probe must never be a
                # new place to fail ahead of it.
                self._log(f"Could not read the input sample rate ({exc}); leaving it as it is.")
                source_rate, source_frames = ROFORMER_SAMPLE_RATE, None

            if source_rate != ROFORMER_SAMPLE_RATE:
                self._log(
                    f"Resampling {source_rate} Hz input to {ROFORMER_SAMPLE_RATE} Hz "
                    "for BS-RoFormer..."
                )
                aligned = staging / f"{staged.stem}_stemlab_{ROFORMER_SAMPLE_RATE}.wav"

                frames = _resample_file(
                    staged,
                    aligned,
                    ROFORMER_SAMPLE_RATE,
                    widen_narrow_pcm=True,
                )
                if frames == 0:
                    # A source shorter than one 44.1 kHz frame resamples away
                    # to nothing, and the CLI has no answer for an empty WAV.
                    _resample_file(
                        staged,
                        aligned,
                        ROFORMER_SAMPLE_RATE,
                        out_frames=1,
                        widen_narrow_pcm=True,
                    )

                # The CLI discovers its input with glob("*.wav"), so the
                # staging directory has to hold exactly one file or both
                # rates would be separated. Keeping the original stem name
                # also keeps the output canonicalisation below unchanged.
                aligned.replace(staged)

            command = build_roformer_command(staging, output_dir, device, self.model)

            self._log("Starting pretrained BS-RoFormer separation...")
            self._log(f"Model: {self.model}")
            self._log(f"Device: {device}")
            self._progress(0.0)

            return_code = run_progress_process(
                command,
                self._log,
                self._progress,
                eta=self.eta_callback,
                download=self.download_callback,
                log_progress_lines=False,
                cancellation=self.cancellation,
            )
            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)
            self._progress(100.0)

            # The CLI writes "{track}_{instrument}.wav". Renaming those to
            # plain "{instrument}.wav" makes every downstream lookup exact:
            # a track called "guitar_take" no longer makes all six outputs
            # look like the guitar stem.
            _canonicalise_output_names(output_dir, staged.stem, self._log)

        # After the staging directory is gone, so the 44.1 kHz copy of the
        # input is not still resident while six stems are rewritten. Keyed
        # off the rate rather than the stem names, which the rename above is
        # allowed to leave alone when it fails.
        if source_rate != ROFORMER_SAMPLE_RATE:
            self._log(f"Returning stems to {source_rate} Hz...")
            _restore_stem_sample_rate(output_dir, source_rate, source_frames, self._log)

        files = sorted(
            p
            for p in output_dir.rglob("*")
            if p.is_file() and p.suffix.lower() in {".wav", ".flac"}
        )
        if not files:
            raise RuntimeError(
                "The pretrained separator finished but StemLab found no output audio."
            )
        return files
