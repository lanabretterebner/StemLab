function Get-StemLabBackendConfiguration([string]$Backend) {
    switch ($Backend.ToLowerInvariant()) {
        "nvidia" {
            return @{
                Name = "nvidia"
                Label = "NVIDIA CUDA"
                Suffix = "NVIDIA"
                Python = "3.11"
                DefaultEnvironment = ".venv"
                TorchIndex = "https://download.pytorch.org/whl/cu128"
            }
        }
        "cpu" {
            return @{
                Name = "cpu"
                Label = "CPU"
                Suffix = "CPU"
                Python = "3.11"
                DefaultEnvironment = ".venv"
                TorchIndex = "https://download.pytorch.org/whl/cpu"
            }
        }
        "amd" {
            return @{
                Name = "amd"
                Label = "AMD ROCm (experimental)"
                Suffix = "AMD-Experimental"
                Python = "3.12"
                DefaultEnvironment = ".venv-amd"
            }
        }
        default { throw "Unsupported StemLab Windows backend: $Backend" }
    }
}

function Install-StemLabTorchBackend([string]$Python, [hashtable]$Configuration) {
    if ($Configuration.Name -in @("nvidia", "cpu")) {
        & $Python -m pip install --no-cache-dir `
            "torch==2.9.1" "torchaudio==2.9.1" `
            --index-url $Configuration.TorchIndex
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        return
    }

    # These are AMD's pinned ROCm 7.2.1 Windows packages, not a generic or
    # NVIDIA PyTorch index. AMD currently requires Python 3.12 and Windows 11.
    $RocmRoot = "https://repo.radeon.com/rocm/windows/rocm-rel-7.2.1"
    & $Python -m pip install --no-cache-dir `
        "$RocmRoot/rocm_sdk_core-7.2.1-py3-none-win_amd64.whl" `
        "$RocmRoot/rocm_sdk_devel-7.2.1-py3-none-win_amd64.whl" `
        "$RocmRoot/rocm_sdk_libraries_custom-7.2.1-py3-none-win_amd64.whl" `
        "$RocmRoot/rocm-7.2.1.tar.gz"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $Python -m pip install --no-cache-dir `
        "$RocmRoot/torch-2.9.1%2Brocm7.2.1-cp312-cp312-win_amd64.whl" `
        "$RocmRoot/torchaudio-2.9.1%2Brocm7.2.1-cp312-cp312-win_amd64.whl" `
        "$RocmRoot/torchvision-0.24.1%2Brocm7.2.1-cp312-cp312-win_amd64.whl"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
