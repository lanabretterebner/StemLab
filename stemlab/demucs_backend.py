"""Official Demucs subprocess backend (``htdemucs_6s`` six-stem layout)."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Callable

from .audio import STEM_NAMES
from .device import resolve_torch_device
from .pretrained import _normalise_input_for_backend
from .resample import rate_and_frames, restore_folder_sample_rate
from .runtime import CancellationToken, run_progress_process

# Demucs is never told a rate. separate.py loads with model.samplerate and
# saves with it, and every htdemucs/hdemucs variant sets that to 44100, so a
# 48 kHz session gets 44.1 kHz stems back unless something puts them right.
DEMUCS_MODEL_SAMPLE_RATE = 44100


def _warn_if_not_float(stems: list[Path], log: Callable[[str], None]) -> None:
    """Say so if --float32 did not take, rather than leaving it to be heard.

    Reported, never raised. A 16 bit stem is lower fidelity, not wrong - it
    plays at the right pitch, length and level - so failing a finished
    separation over it would cost the user more than the defect does. What
    it would mean is that this torchaudio cannot write PCM_F, which is worth
    knowing before the stems are gain-staged rather than after.
    """
    import soundfile as sf

    for path in stems:
        try:
            subtype = sf.info(str(path)).subtype
        except Exception:
            continue

        if subtype != "FLOAT":
            log(
                f"Demucs wrote {path.name} as {subtype}, not 32-bit float, "
                "despite --float32. The stems are usable but carry a "
                "quantisation floor the other engines do not."
            )


DEFAULT_DEMUCS_MODEL = "htdemucs_6s"
PACKAGED_DEMUCS_SIGNATURE = "5c90dfd2"
PACKAGED_DEMUCS_FILENAME = "5c90dfd2-34c22ccb.th"


class DemucsBackend:
    """Run the official Demucs Python package as a subprocess.

    ``htdemucs_6s`` matches FI-STEM's RoFormer layout: vocals, drums, bass,
    guitar, piano, other.
    """

    def __init__(
        self,
        model: str = DEFAULT_DEMUCS_MODEL,
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
        """Run Demucs, then copy the six stems into a flat ``output_dir``."""
        input_path = Path(input_path).resolve()
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        device = resolve_torch_device(self.device, self._log)

        if self.cancellation:
            self.cancellation.raise_if_cancelled()

        probe_exit_code = run_progress_process(
            [
                sys.executable,
                "-c",
                "import demucs, sys; sys.stdout.write(getattr(demucs, '__version__', 'ok'))",
            ],
            lambda _message: None,
            lambda _percent: None,
            cancellation=self.cancellation,
        )
        if probe_exit_code != 0:
            raise RuntimeError(
                "Demucs is not installed in FI-STEM's Python environment. "
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
            try:
                source_rate, source_frames = rate_and_frames(staged)
            except Exception as exc:
                # Unreadable header: nothing to conform to, and demucs is
                # about to give a clearer error than this could.
                self._log(f"Could not read the input's sample rate: {exc}")
                source_rate, source_frames = DEMUCS_MODEL_SAMPLE_RATE, None

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

            command = [
                sys.executable,
                "-m",
                "demucs.separate",
                "--name",
                model_name,
                *repo_args,
                "--device",
                device,
                # Without this demucs writes 16 bit (separate.py defaults
                # bits_per_sample to 16), while the RoFormer path publishes
                # 32 bit float. A stem is not a master - it gets gain-staged,
                # often pushed well up - and a -101 dBFS quantisation floor
                # rides up with it. Costs 2x the bytes.
                #
                # This does NOT disable demucs' prevent_clip: that runs before
                # the format branch whatever the bit depth, and the CLI
                # exposes only rescale and clamp, never none.
                "--float32",
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

            if source_rate != DEMUCS_MODEL_SAMPLE_RATE:
                self._log(f"Returning stems to {source_rate} Hz...")
                restore_folder_sample_rate(output_dir, source_rate, source_frames, self._log)

            _warn_if_not_float(copied, self._log)

            self._progress(100.0)
            self._log("Demucs separation complete: " + ", ".join(path.name for path in copied))
            return copied
