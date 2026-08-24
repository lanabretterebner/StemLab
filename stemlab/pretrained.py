"""BS-RoFormer backend: stage input audio and run ``bs-roformer-infer``."""

from __future__ import annotations

import locale
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .runtime import drain_cr_lf_stream, last_progress_percent

DEFAULT_MODEL = "roformer-model-bs-roformer-sw-by-jarredou"


def _find_ffmpeg() -> str | None:
    """Locate ffmpeg next to this interpreter, then on PATH."""
    python_dir = Path(sys.executable).resolve().parent
    for candidate in (python_dir / "ffmpeg.exe", python_dir / "ffmpeg"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("ffmpeg")


def _normalise_input_for_backend(
    input_path: Path,
    staging_dir: Path,
    log: Callable[[str], None],
) -> Path:
    """Return a WAV/FLAC the separator can decode, converting via ffmpeg if needed."""
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
    log(f"Converting {extension or 'input audio'} to WAV for the separator...")
    completed = subprocess.run(
        [
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
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if completed.returncode != 0 or not staged.exists():
        details = (completed.stdout or "").strip()
        if details:
            raise RuntimeError("FFmpeg could not convert the input audio: " + details)
        raise RuntimeError("FFmpeg could not convert the input audio.")
    return staged


@dataclass
class PretrainedResult:
    """Audio files produced by one RoFormer run."""

    output_dir: Path
    files: list[Path]


class RoFormerBackend:
    """Adapter around the documented ``bs-roformer-infer`` CLI."""

    def __init__(
        self,
        model: str = DEFAULT_MODEL,
        device: str = "cuda",
        log_callback: Callable[[str], None] | None = None,
        progress_callback: Callable[[float], None] | None = None,
    ) -> None:
        self.model = model
        self.device = device
        self.log_callback = log_callback
        self.progress_callback = progress_callback

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
    ) -> PretrainedResult:
        """Run RoFormer into ``output_dir`` and return the written stem files."""
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        command_prefix = [sys.executable, "-m", "stemlab.bs_roformer_cli"]

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

            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=0,
            )
            assert process.stdout is not None

            last_reported = -1
            backend_encoding = locale.getpreferredencoding(False) or "utf-8"

            def consume_segment(raw: bytes) -> None:
                nonlocal last_reported
                if not raw:
                    return
                percent = last_progress_percent(raw)
                if percent is not None:
                    rounded = int(percent)
                    if rounded != last_reported:
                        last_reported = rounded
                        self._progress(percent)
                try:
                    text = raw.decode(backend_encoding, errors="replace").strip()
                except LookupError:
                    text = raw.decode("utf-8", errors="replace").strip()
                if text and percent is None:
                    self._log(text)

            drain_cr_lf_stream(process.stdout, consume_segment)
            return_code = process.wait()
            if return_code != 0:
                raise subprocess.CalledProcessError(return_code, command)
            self._progress(100.0)

        files = sorted(
            p
            for p in output_dir.rglob("*")
            if p.is_file() and p.suffix.lower() in {".wav", ".flac"}
        )
        if not files:
            raise RuntimeError(
                "The pretrained separator finished but StemLab found no output audio."
            )
        return PretrainedResult(output_dir=output_dir, files=files)
