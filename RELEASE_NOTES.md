# StemLab 0.9.9

StemLab is a Windows stem-separation application with optional Ableton Live integration.

It includes the required Python/ML runtime, so you do **not** need to install Python, FFmpeg, Demucs, or the separation models separately.

## Installation

1. Download **`StemLab-Setup-0.9.9.exe`** from the GitHub release.
2. Run the installer.
3. Choose whether you want:

   * **StemLab Desktop App**
   * **Ableton Live Integration**
   * Desktop / Start Menu shortcuts
4. Finish the installation and launch StemLab.

The installer is a single self-contained file. No additional `.bin` files or runtime downloads are required.

## Using StemLab Standalone

Open StemLab and load an audio file using **Select File** or drag an audio file into the application.

Choose the separation engine and options you want, then start separation.

StemLab supports:

* BS-RoFormer
* Demucs `htdemucs_6s`
* Hybrid separation
* Optional StemLab refinement
* Up to six separated stems
* Individual stem waveform previews
* Stem auditioning before export
* Selectable stem export
* Windows system-audio recording

The bundled runtime is installed automatically with StemLab.

---

# Ableton Live Integration

StemLab can send separated stems directly into Ableton Live.

## Install the Ableton Integration

During the StemLab installer, enable:

**Ableton Live Integration**

The installer will install:

* `StemLab.vst3`
* `StemLabRemote`

StemLab will attempt to detect your Ableton User Library automatically.

If it cannot find it, select your Ableton **User Library** folder manually when prompted.

A typical location is:

`Documents\Ableton\User Library`

## Configure Ableton Live

After installation, completely restart Ableton Live.

Then open:

**Settings → Link, Tempo & MIDI**

Find an empty **Control Surface** slot and configure:

* **Control Surface:** `StemLabRemote`
* **Input:** `None`
* **Output:** `None`

No Max for Live device is required.

## Using StemLab Inside Ableton

1. Add the **StemLab VST3** to an audio track.
2. Select an audio clip in Ableton's Arrangement View.
3. Open StemLab.
4. Choose **Use Live Clip**.
5. Separate the audio.
6. Audition the generated stems inside StemLab.
7. Select the stems you want.
8. Choose **Send Selected**.

StemLab will send the selected results back into Ableton as separate audio tracks.

If the stems have already been separated, they can be sent again without rerunning the separation process.

## Repairing Ableton Integration

If StemLab does not appear in Ableton or `StemLabRemote` is missing, reinstall or repair the Ableton integration from StemLab.

After repairing, fully restart Ableton Live before checking the VST3 and Control Surface lists again.

---

## System Notes

StemLab 0.9.9 is currently distributed for **64-bit Windows**.

GPU acceleration is supported when a compatible CUDA-capable NVIDIA GPU is available. Processing speed depends on the selected separation engine, audio length, and hardware.

The first separation may take longer while the engine initializes.

## What's Included

StemLab 0.9.9 includes:

* Standalone Windows application
* Ableton Live VST3
* StemLabRemote Ableton integration
* Embedded Python/ML runtime
* FFmpeg
* Separation models and supporting dependencies
* StemLab waveform-based interface and application branding
