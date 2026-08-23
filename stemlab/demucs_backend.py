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

from .pretrained import _normalise_input_for_backend


DEFAULT_DEMUCS_MODEL = "htdemucs_6s"
DEMUCS_STEMS = ("vocals", "drums", "bass", "guitar", "piano", "other")


@dataclass
class DemucsResult:
    output_dir: Path
    files: list[Path]


class DemucsBackend:
    """Run the official Demucs Python package as a subprocess.

    `htdemucs_6s` is used because it produces the same six logical stems as
    StemLab's BS-RoFormer backend: vocals, drums, bass, guitar, piano, other.
    """

    def __init__(
        self,
        model: str = DEFAULT_DEMUCS_MODEL,
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
            self.progress_callback(
                max(0.0, min(100.0, float(percent)))
            )

    def separate(
        self,
        input_path: str | Path,
        output_dir: str | Path,
    ) -> DemucsResult:
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        # Check that Demucs is importable in the same interpreter StemLab uses.
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

            segment = bytearray()
            last_reported = -1
            backend_encoding = locale.getpreferredencoding(False) or "utf-8"

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

                try:
                    text = raw.decode(
                        backend_encoding,
                        errors="replace",
                    ).strip()
                except LookupError:
                    text = raw.decode(
                        "utf-8",
                        errors="replace",
                    ).strip()

                if text:
                    self._log(text)

            while True:
                byte = process.stdout.read(1)

                if not byte:
                    if segment:
                        consume_segment(bytes(segment))
                    break

                if byte in (b"\r", b"\n"):
                    if segment:
                        consume_segment(bytes(segment))
                        segment.clear()
                else:
                    segment.extend(byte)

            exit_code = process.wait()

            if exit_code != 0:
                raise RuntimeError(
                    f"Demucs failed with exit code {exit_code}"
                )

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

            return DemucsResult(
                output_dir=output_dir,
                files=copied,
            )
