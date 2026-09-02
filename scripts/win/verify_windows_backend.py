"""Validate and describe a packaged StemLab Windows Torch backend."""

from __future__ import annotations

import argparse
import platform
from typing import Any


BACKEND_LABELS = {
    "nvidia": "NVIDIA CUDA",
    "cpu": "CPU",
    "amd": "AMD ROCm (experimental)",
}


def inspect_backend(backend: str, torch: Any) -> dict[str, str]:
    cuda_version = getattr(torch.version, "cuda", None)
    hip_version = getattr(torch.version, "hip", None)
    available = bool(torch.cuda.is_available())

    if backend == "nvidia" and cuda_version is None:
        raise RuntimeError("NVIDIA release requires a CUDA-enabled Torch build; found CPU-only Torch.")
    if backend == "cpu" and (cuda_version is not None or hip_version is not None):
        raise RuntimeError(
            "CPU release requires CPU-only Torch; "
            f"found CUDA={cuda_version or 'None'}, HIP={hip_version or 'None'}."
        )
    if backend == "amd" and hip_version is None:
        raise RuntimeError("AMD release requires an AMD ROCm/HIP Torch build; torch.version.hip is None.")

    gpu = torch.cuda.get_device_name(0) if available else "Unavailable"
    return {
        "Backend": BACKEND_LABELS[backend],
        "Python": platform.python_version(),
        "Torch": str(torch.__version__),
        "CUDA": str(cuda_version or "None"),
        "HIP": str(hip_version or "None"),
        "Device available": str(available),
        "GPU": str(gpu),
    }


def print_report(details: dict[str, str]) -> None:
    print(f"StemLab runtime backend: {details['Backend']}")
    print(f"Torch: {details['Torch']}")
    if details["Backend"] == "NVIDIA CUDA":
        print(f"CUDA build: {details['CUDA']}")
        print(f"CUDA available: {details['Device available']}")
    elif details["Backend"].startswith("AMD"):
        print(f"HIP build: {details['HIP']}")
        print(f"ROCm device available: {details['Device available']}")
    if details["Device available"] == "True":
        print(f"GPU: {details['GPU']}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=tuple(BACKEND_LABELS), required=True)
    args = parser.parse_args()

    import torch

    print_report(inspect_backend(args.backend, torch))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
