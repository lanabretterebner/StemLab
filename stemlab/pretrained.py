"""BS-RoFormer backend: stage input audio and run ``bs-roformer-infer``."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

from .device import resolve_torch_device
from .runtime import run_progress_process

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
    ) -> None:
        """Configure the model, device, logging, and progress callbacks."""
        self.model = model
        self.device = device
        self.log_callback = log_callback
        self.progress_callback = progress_callback
        self.eta_callback = eta_callback
        self.download_callback = download_callback

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
            _normalise_input_for_backend(
                input_path=input_path,
                staging_dir=staging,
                log=self._log,
            )
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
                eta=self.eta_callback,
                download=self.download_callback,
                log_progress_lines=False,
            )
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
        return files
