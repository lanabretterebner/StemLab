from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable
import locale
import re
import shutil
import subprocess
import sys
import tempfile


DEFAULT_MODEL = "roformer-model-bs-roformer-sw-by-jarredou"


def _find_ffmpeg() -> str | None:
    """Find ffmpeg for compressed input normalization."""
    python_dir = Path(sys.executable).resolve().parent

    candidates = [
        python_dir / "ffmpeg.exe",
        python_dir / "ffmpeg",
    ]

    for candidate in candidates:
        if candidate.exists():
            return str(candidate)

    return shutil.which("ffmpeg")


def _normalise_input_for_backend(
    input_path: Path,
    staging_dir: Path,
    log: Callable[[str], None],
) -> Path:
    """Return an input file that bs-roformer-infer can reliably decode.

    WAV and FLAC are staged as-is. Compressed/container formats such as MP3,
    OGG and AIFF are converted to a stereo PCM WAV with ffmpeg first.
    """
    extension = input_path.suffix.lower()

    if extension in {".wav", ".flac"}:
        staged = staging_dir / input_path.name
        shutil.copy2(input_path, staged)
        return staged

    ffmpeg = _find_ffmpeg()

    if ffmpeg is None:
        raise RuntimeError(
            f"StemLab needs FFmpeg to open {extension or 'this audio format'}, "
            "but ffmpeg was not found on PATH."
        )

    staged = staging_dir / f"{input_path.stem}_stemlab_input.wav"

    log(
        f"Converting {extension or 'input audio'} to WAV for the separator..."
    )

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

    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )

    if completed.returncode != 0 or not staged.exists():
        details = (completed.stdout or "").strip()

        if details:
            raise RuntimeError(
                "FFmpeg could not convert the input audio: " + details
            )

        raise RuntimeError(
            "FFmpeg could not convert the input audio."
        )

    return staged



@dataclass
class PretrainedResult:
    output_dir: Path
    files: list[Path]


class RoFormerBackend:
    """Adapter around the documented bs-roformer-infer CLI."""

    def __init__(
        self,
        model: str = DEFAULT_MODEL,
        device: str = "cuda",
        log_callback: Callable[[str], None] | None = None,
        progress_callback: Callable[[float], None] | None = None,
    ):
        self.model = model
        self.device = device
        self.log_callback = log_callback
        self.progress_callback = progress_callback

    def _log(self, message: str):
        if self.log_callback:
            self.log_callback(message)
        else:
            print(message, flush=True)

    def _progress(self, percent: float):
        if self.progress_callback:
            self.progress_callback(max(0.0, min(100.0, float(percent))))

    def separate(
        self,
        input_path: str | Path,
        output_dir: str | Path,
    ) -> PretrainedResult:
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        # Always invoke the upstream CLI through this interpreter. The helper
        # module resolves bs-roformer-infer's console entry point dynamically,
        # avoiding pip's absolute-path Windows launcher and making Engine/
        # fully relocatable.
        command_prefix = [
            sys.executable,
            "-m",
            "stemlab.bs_roformer_cli",
        ]

        with tempfile.TemporaryDirectory(prefix="stemlab_input_") as td:
            staging = Path(td)

            _normalise_input_for_backend(
                input_path=input_path,
                staging_dir=staging,
                log=self._log,
            )

            command = [
                *command_prefix,
                "--input_folder",
                str(staging),
                "--store_dir",
                str(output_dir),
                "--device",
                self.device,
                "--model",
                self.model,
            ]

            self._log("Starting pretrained BS-RoFormer separation...")
            self._log(f"Model: {self.model}")
            self._log(f"Device: {self.device}")
            self._progress(0.0)

            # bs-roformer-infer uses tqdm-style carriage-return updates.
            # Iterating "for line in stdout" waits for a newline and hides
            # progress until the process finishes. Read one byte at a time so
            # both CR and LF terminate a progress message immediately.
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=0,
            )

            assert process.stdout is not None

            segment = bytearray()
            last_reported = -1

            def consume_segment(raw: bytes):
                nonlocal last_reported

                if not raw:
                    return

                matches = re.findall(
                    rb"(?<!\d)(\d{1,3}(?:\.\d+)?)%",
                    raw,
                )

                if matches:
                    try:
                        percent = float(matches[-1].decode("ascii"))
                    except ValueError:
                        percent = -1.0

                    if 0.0 <= percent <= 100.0:
                        rounded = int(percent)

                        if rounded != last_reported:
                            last_reported = rounded
                            self._progress(percent)

                # bs-roformer-infer is another Python process. On Windows,
                # when stdout is a pipe, it commonly writes using the active
                # ANSI/code-page encoding (e.g. cp1252), not UTF-8. Decoding
                # those bytes as UTF-8 turns names like "Pokémon" into a
                # replacement character and can later crash our own logger.
                backend_encoding = locale.getpreferredencoding(False) or "utf-8"

                try:
                    text = raw.decode(backend_encoding, errors="replace").strip()
                except LookupError:
                    text = raw.decode("utf-8", errors="replace").strip()

                # Keep diagnostics available behind Settings without flooding
                # the UI with every duplicate tqdm repaint.
                if text and not matches:
                    self._log(text)

            while True:
                byte = process.stdout.read(1)

                if not byte:
                    break

                if byte in (b"\r", b"\n"):
                    consume_segment(bytes(segment))
                    segment.clear()
                else:
                    segment.extend(byte)

            consume_segment(bytes(segment))

            return_code = process.wait()

            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)

            self._progress(100.0)

        files = sorted(
            p for p in output_dir.rglob("*")
            if p.is_file() and p.suffix.lower() in {".wav", ".flac"}
        )

        if not files:
            raise RuntimeError(
                "The pretrained separator finished but StemLab found no output audio."
            )

        return PretrainedResult(
            output_dir=output_dir,
            files=files,
        )
