"""BS-RoFormer backend: stage input audio and run ``bs-roformer-infer``."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from .device import resolve_torch_device
from .resample import rate_and_frames, resample_file, restore_folder_sample_rate
from .runtime import CancellationToken, run_progress_process

DEFAULT_MODEL = "roformer-model-bs-roformer-sw-by-jarredou"


# BS-RoFormer's band split is defined in FFT bins, not in Hz, so the band
# edges only land on the frequencies the model was trained for when it is fed
# the rate it was trained at. Feeding 48 kHz moves every edge up by 8.8%.
ROFORMER_SAMPLE_RATE = 44100


def _find_ffmpeg() -> str | None:
    """Locate ffmpeg next to this interpreter, then on PATH."""
    python_dir = Path(sys.executable).resolve().parent
    for candidate in (python_dir / "ffmpeg.exe", python_dir / "ffmpeg"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("ffmpeg")


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
        # A write that died partway leaves a stub behind, and the ffmpeg
        # fallback stages under a different name: two WAVs in the folder and
        # the CLI separates the wrong one, or both. Nothing downstream can
        # tell the stub from real audio, so it goes now.
        staged.unlink(missing_ok=True)
        return False

    return staged.exists()


def _normalise_input_for_backend(
    input_path: Path,
    staging_dir: Path,
    log: Callable[[str], None],
    cancellation: CancellationToken | None = None,
    passthrough_extensions: set[str] | None = None,
) -> Path:
    """Return audio the separator can decode, converting when it cannot.

    ``passthrough_extensions`` is what the calling backend reads natively,
    and the backends disagree. Demucs opens FLAC happily. The BS-RoFormer
    CLI discovers its input with a case-sensitive ``glob("*.wav")``, so a
    staged FLAC is invisible to it however well libsndfile could read it -
    which is why that backend asks for WAV only.
    """
    if passthrough_extensions is None:
        passthrough_extensions = {".wav", ".flac"}

    extension = input_path.suffix.lower()
    if extension in passthrough_extensions:
        staged = staging_dir / input_path.name
        shutil.copy2(input_path, staged)
        return staged

    if extension == ".flac":
        # Lossless, and soundfile is already a dependency, so converting for
        # a WAV-only backend need not put ffmpeg between a user and a format
        # this project otherwise supports.
        # Named for the source rather than with the ffmpeg branch's
        # "_stemlab_input" marker: staging is a fresh temp dir holding this
        # one file, and the separator's stem names are built from it, so a
        # marker here would show up in every stem a FLAC session produces.
        staged = staging_dir / f"{input_path.stem}.wav"
        log("Converting FLAC to WAV for the separator...")

        if _convert_with_soundfile(input_path, staged):
            return staged

        log("soundfile could not decode the FLAC; falling back to ffmpeg.")

    ffmpeg = _find_ffmpeg()
    if ffmpeg is None:
        raise RuntimeError(
            f"FI-STEM needs FFmpeg to open {extension or 'this audio format'}, "
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
        cancellation: CancellationToken | None = None,
    ) -> None:
        """Configure the model, device, logging, and progress callbacks."""
        self.model = model
        self.device = device
        self.log_callback = log_callback
        self.progress_callback = progress_callback
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

        with tempfile.TemporaryDirectory(prefix="stemlab_input_") as td:
            staging = Path(td)
            staged = _normalise_input_for_backend(
                input_path=input_path,
                staging_dir=staging,
                log=self._log,
                cancellation=self.cancellation,
                # The upstream CLI only ever discovers "*.wav".
                passthrough_extensions={".wav"},
            )

            # Read the rate off the staged file rather than the original: an
            # ffmpeg conversion above may already have changed it.
            try:
                source_rate, source_frames = rate_and_frames(staged)
            except Exception as exc:
                # Not fatal. An unreadable header means the rate cannot be
                # conformed either way, and the separator is about to report
                # a far clearer error than this could.
                self._log(f"Could not read the sample rate of {staged.name}: {exc}")
                source_rate, source_frames = ROFORMER_SAMPLE_RATE, None

            if source_rate != ROFORMER_SAMPLE_RATE:
                self._log(
                    f"Resampling {source_rate} Hz input to {ROFORMER_SAMPLE_RATE} Hz "
                    "for BS-RoFormer..."
                )
                # The conformed file has to land on a .wav path, and be the
                # only one in the folder: the CLI discovers its input with
                # glob("*.wav"), so renaming a WAV onto song.flac leaves it
                # nothing to find, and leaving both separates the folder
                # twice. Staging already hands this backend a .wav for every
                # input format, which makes target == staged today; the
                # rename stays honest about the suffix rather than depending
                # on that.
                target = staged.with_suffix(".wav")
                aligned = staging / f"{staged.stem}_stemlab_{ROFORMER_SAMPLE_RATE}.wav"
                frames = resample_file(
                    staged,
                    aligned,
                    ROFORMER_SAMPLE_RATE,
                    # Throwaway input, written once and read once: widening
                    # keeps the extra trip through 44.1 kHz from re-quantising
                    # a 16-bit source. Stems keep the width they were written
                    # at; only what the model eats is widened.
                    widen_narrow_pcm=True,
                )
                if frames == 0:
                    # A source shorter than one 44.1 kHz frame resamples away
                    # to nothing, and the CLI has no answer for an empty WAV.
                    resample_file(
                        staged,
                        aligned,
                        ROFORMER_SAMPLE_RATE,
                        out_frames=1,
                        widen_narrow_pcm=True,
                    )

                if target != staged:
                    staged.unlink()

                aligned.replace(target)
                staged = target
            command = [
                sys.executable,
                "-m",
                "stemlab.bs_roformer_cli",
                "--input_folder",
                str(staging),
                "--store_dir",
                str(output_dir),
                "--device",
                device,
                "--model",
                self.model,
            ]

            self._log("Starting pretrained BS-RoFormer separation...")
            self._log(f"Model: {self.model}")
            self._log(f"Device: {device}")
            self._progress(0.0)

            return_code = run_progress_process(
                command,
                self._log,
                self._progress,
                log_progress_lines=False,
                cancellation=self.cancellation,
            )
            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)
            self._progress(100.0)

            # Fusion reads its rate off the RoFormer stem itself, so a stem
            # left at 44.1 kHz would make every fused output of a 48 kHz
            # session 44.1 kHz too.
            if source_rate != ROFORMER_SAMPLE_RATE:
                self._log(f"Returning stems to {source_rate} Hz...")
                restore_folder_sample_rate(output_dir, source_rate, source_frames, self._log)

        files = sorted(
            p
            for p in output_dir.rglob("*")
            if p.is_file() and p.suffix.lower() in {".wav", ".flac"}
        )
        if not files:
            raise RuntimeError(
                "The pretrained separator finished but FI-STEM found no output audio."
            )
        return files
