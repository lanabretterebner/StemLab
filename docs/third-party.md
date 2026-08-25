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
backend.

Relevant upstream projects include:

- OpenMIRLab / bs-roformer-infer
- BS-RoFormer by Phil Wang / lucidrains
- Music-Source-Separation-Training by ZFTurbo
- Band-Split RoPE Transformer research

StemLab does not commit pretrained checkpoint bytes to this repository.
Checkpoints are downloaded separately and may have their own attribution,
redistribution, or usage terms.

Review the terms associated with any checkpoint before redistributing or
mirroring its weights.

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

## Python / ML runtime

StemLab's development environment contains third-party components including
CPython, PyTorch, NumPy, SciPy, SoundFile, PyYAML, tqdm, Demucs, BS-RoFormer
inference dependencies, audio-separator, and their transitive dependencies.

Those packages retain their respective upstream licenses.

## FFmpeg

FFmpeg licensing obligations depend on the configuration of the specific FFmpeg
binary being redistributed. Before publishing a binary release, verify the
license/build configuration of the FFmpeg executable you ship and include any
required notices or corresponding-source obligations.

This file is informational and is not legal advice.
