# Third-party components

StemLab's original source code is licensed under the MIT License.

Third-party projects, libraries, runtimes, frameworks, and pretrained models
retain their own copyrights, licenses, and terms. They are not relicensed under
StemLab's MIT License.

## JUCE

StemLab's Standalone application and VST3 frontend are built with JUCE.

This repository currently pins JUCE 9.0.0 through CMake FetchContent rather
than committing JUCE source into the StemLab repository.

JUCE has its own dual licensing model (open-source AGPLv3 or a commercial JUCE
license). Anyone building or distributing StemLab binaries is responsible for
complying with the JUCE license applicable to that distribution. The VST3 SDK
included by current JUCE releases has separate terms.

## BS-RoFormer

StemLab can use `bs-roformer-infer` as a pretrained BS-RoFormer inference
backend (upstream: OpenMIRLab/bs-roformer-infer, lucidrains' BS-RoFormer,
ZFTurbo's Music-Source-Separation-Training).

No checkpoint bytes are committed to this repository. Checkpoints are
downloaded separately and may carry their own attribution, redistribution,
or usage terms - review them before redistributing or mirroring weights.

## Demucs

StemLab can use the upstream `demucs` Python package and the `htdemucs_6s`
pretrained model.

Demucs code and model artifacts remain third-party components and retain their
upstream licensing/terms.

## Inter font

The plugin interface embeds the Inter typeface (static Regular and Medium
instances under `plugin/Resources/fonts/`), copyright The Inter Project
Authors, licensed under the SIL Open Font License 1.1. The full license text
ships beside the font files as `plugin/Resources/fonts/OFL.txt` and must
accompany any redistribution of the font files.

## Beat This!

Optional beat/downbeat analysis uses Beat This! 1.1.0 and the upstream
`small0` and `final0` checkpoints. Beat This! is MIT licensed. Release staging
validates the exact files listed in `packaging/models.json`.

## Python / ML runtime

StemLab's development environment contains third-party components including
CPython, PyTorch, NumPy, SciPy, SoundFile, librosa, Mido, PyYAML, tqdm, Demucs, BS-RoFormer
inference dependencies, audio-separator, and their transitive dependencies.

Those packages retain their respective upstream licenses.

## FFmpeg

FFmpeg licensing depends on the build configuration of the binary being
redistributed. Verify the executable you ship and include any required
notices or corresponding-source obligations.

This file is informational and is not legal advice.
