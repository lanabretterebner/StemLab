"""Official Demucs subprocess backend (``htdemucs_6s`` six-stem layout)."""

from __future__ import annotations

import importlib.util
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Callable

from .audio import STEM_NAMES
from .device import resolve_torch_device
from .pretrained import _clear_audio_files, _normalise_input_for_backend
from .runtime import CancellationToken, run_progress_process

DEFAULT_DEMUCS_MODEL = "htdemucs_6s"
PACKAGED_DEMUCS_SIGNATURE = "5c90dfd2"
PACKAGED_DEMUCS_FILENAME = "5c90dfd2-34c22ccb.th"

# Cross-fade fraction between Demucs' sliding analysis windows. The upstream
# default of 0.25 re-processes a quarter of every window (~17% extra forward
# passes) for smoothing headroom the htdemucs family does not need; 0.10 keeps
# the window seams inaudible while dropping that redundant compute.
DEMUCS_OVERLAP = 0.10


def _demucs_available() -> bool:
    # find_spec resolves the package on disk without importing it, so the
    # torch-heavy demucs module never loads into this process.
    return importlib.util.find_spec("demucs") is not None


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
        """Run Demucs, then copy the six stems into a flat ``output_dir``."""
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        device = resolve_torch_device(self.device, self._log)

        # Demucs writes canonical names and would overwrite its own output,
        # but a reused directory can still hold another backend's leftovers.
        _clear_audio_files(output_dir)

        if self.cancellation:
            self.cancellation.raise_if_cancelled()

        if not _demucs_available():
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
                cancellation=self.cancellation,
            )
            raw_output = Path(td) / "demucs_output"
            model_name = self.model
            packaged_repo = os.environ.get("STEMLAB_DEMUCS_MODEL_REPO")
            repo_args: list[str] = []
            if packaged_repo and self.model == DEFAULT_DEMUCS_MODEL:
                repo_path = Path(packaged_repo).resolve()
                checkpoint = repo_path / PACKAGED_DEMUCS_FILENAME
                if not checkpoint.is_file():
                    raise RuntimeError(
                        "Packaged Demucs model is missing: " + str(checkpoint)
                    )
                model_name = PACKAGED_DEMUCS_SIGNATURE
                repo_args = ["--repo", str(repo_path)]

            # Parallel worker processes only pay off on CPU; on an
            # accelerator they would multiply device-memory use instead.
            cpu_args = (
                ["-j", str(min(4, os.cpu_count() or 1))] if device == "cpu" else []
            )

            command = [
                sys.executable,
                "-m",
                "demucs.separate",
                "--name",
                model_name,
                *repo_args,
                "--device",
                device,
                "--overlap",
                str(DEMUCS_OVERLAP),
                *cpu_args,
                "--out",
                str(raw_output),
                str(staged),
            ]

            self._log("Starting Demucs separation...")
            if model_name == self.model:
                self._log(f"Model: {self.model}")
            else:
                self._log(f"Model: {self.model} (packaged {model_name})")
            self._log(f"Device: {device}")
            self._progress(0.0)

            exit_code = run_progress_process(
                command,
                self._log,
                self._progress,
                eta=self.eta_callback,
                download=self.download_callback,
                cancellation=self.cancellation,
            )
            if exit_code != 0:
                raise RuntimeError(f"Demucs failed with exit code {exit_code}")

            copied: list[Path] = []
            for stem in STEM_NAMES:
                candidates = sorted(
                    raw_output.rglob(f"{stem}.wav"),
                    key=lambda path: (
                        len(path.parts),
                        len(path.name),
                        str(path).lower(),
                    ),
                )
                if not candidates:
                    raise RuntimeError(f"Demucs finished but did not produce the {stem} stem.")
                destination = output_dir / f"{stem}.wav"
                shutil.copy2(candidates[0], destination)
                copied.append(destination)

            self._progress(100.0)
            self._log("Demucs separation complete: " + ", ".join(path.name for path in copied))
            return copied
