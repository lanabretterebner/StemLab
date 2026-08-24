"""Official Demucs subprocess backend (``htdemucs_6s`` six-stem layout)."""

from __future__ import annotations

import locale
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .audio import STEM_NAMES
from .pretrained import _normalise_input_for_backend
from .runtime import drain_cr_lf_stream, last_progress_percent

DEFAULT_DEMUCS_MODEL = "htdemucs_6s"
DEMUCS_STEMS = STEM_NAMES


@dataclass
class DemucsResult:
    """Audio files produced by one Demucs run."""

    output_dir: Path
    files: list[Path]


class DemucsBackend:
    """Run the official Demucs Python package as a subprocess.

    ``htdemucs_6s`` matches StemLab's RoFormer layout: vocals, drums, bass,
    guitar, piano, other.
    """

    def __init__(
        self,
        model: str = DEFAULT_DEMUCS_MODEL,
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
    ) -> DemucsResult:
        """Run Demucs, then copy the six stems into a flat ``output_dir``."""
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        probe = subprocess.run(
            [
                sys.executable,
                "-c",
                "import demucs, sys; sys.stdout.write(getattr(demucs, '__version__', 'ok'))",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if probe.returncode != 0:
            raise RuntimeError(
                "Demucs is not installed in StemLab's Python environment. "
                "Run: python -m pip install -e ."
            )

        with tempfile.TemporaryDirectory(prefix="stemlab_demucs_input_") as td:
            staging = Path(td) / "input"
            staging.mkdir(parents=True, exist_ok=True)
            staged = _normalise_input_for_backend(
                input_path=input_path,
                staging_dir=staging,
                log=self._log,
            )
            raw_output = Path(td) / "demucs_output"
            command = [
                sys.executable,
                "-m",
                "demucs.separate",
                "--name",
                self.model,
                "--device",
                self.device,
                "--out",
                str(raw_output),
                str(staged),
            ]

            self._log("Starting Demucs separation...")
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
                if text:
                    self._log(text)

            drain_cr_lf_stream(process.stdout, consume_segment)
            exit_code = process.wait()
            if exit_code != 0:
                raise RuntimeError(f"Demucs failed with exit code {exit_code}")

            copied: list[Path] = []
            for stem in DEMUCS_STEMS:
                candidates = sorted(
                    raw_output.rglob(f"{stem}.wav"),
                    key=lambda path: (
                        len(path.parts),
                        len(path.name),
                        str(path).lower(),
                    ),
                )
                if not candidates:
                    raise RuntimeError(
                        f"Demucs finished but did not produce the {stem} stem."
                    )
                destination = output_dir / f"{stem}.wav"
                shutil.copy2(candidates[0], destination)
                copied.append(destination)

            self._progress(100.0)
            self._log(
                "Demucs separation complete: "
                + ", ".join(path.name for path in copied)
            )
            return DemucsResult(output_dir=output_dir, files=copied)
