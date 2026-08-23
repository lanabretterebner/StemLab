## Packaging revision r13

- Rebased packaging on the last stable r11 build scripts.
- Uses the new SL waveform-atom branding.
- Re-encoded the JUCE icon source as a smaller RGBA PNG and the Windows installer/app icon as a multi-size RGBA ICO to avoid the r12 icon build regression.
- Installer builds now write `StemLab-installer-build.log` so failures stay diagnosable.

# StemLab 0.9.9

## Highlights

- BS-RoFormer, Demucs `htdemucs_6s`, and Hybrid separation
- Optional adaptive StemLab refinement
- JUCE Standalone + Ableton VST3
- Invisible StemLabRemote integration
- Selectable stem audition/import
- Resizable waveform UI
- Windows WASAPI system-audio recording
- Windows-safe progress reporting
- Portable embedded Python/ML runtime architecture
- Custom StemLab / StemLab Windows application icon

## Distribution notes

The GitHub repository is source-focused and does not contain model checkpoints,
a development virtual environment, CMake/JUCE build output, or packaged ML
runtime binaries.

Use `build_portable_windows.ps1` on Windows to assemble the end-user release
under `dist/`.


## Build-script fix

The Windows JUCE build script now anchors CMake source/build paths to
`plugin\` using `$PSScriptRoot`. This fixes portable builds launched from the
repository root, where the previous `cmake -S .` incorrectly searched for a
root-level `CMakeLists.txt`.


## Display title

The Standalone UI header and native Windows title bar now display `StemLab`.
The executable, VST3 product name, internal IDs, and Ableton compatibility
remain `StemLab`.


## r5 artwork + Ableton installer

- Replaced repository/app artwork with the exact user-supplied pink low-contrast logo.
- Windows icon resource is made only by square-padding that image; the artwork is not regenerated.
- `install_ableton_integration.ps1` now works as the main installer from either
  the source repository or generated portable release.
- The installer auto-elevates for the Program Files VST3 copy.
- Ableton User Library detection now only accepts existing library folders and
  handles common Documents/OneDrive locations.
- A custom `-UserLibrary` may point to User Library, Remote Scripts, or
  StemLabRemote.
- VST3, embedded runtime, and Remote Script are verified after installation.
- Portable build now includes the Ableton integration installer.


## r6 JUCE bootstrap fix

Fresh Windows builds no longer rely on CMake's Git-based FetchContent JUCE
population step.

`plugin\build_windows.ps1` now:

- downloads the pinned JUCE 9.0.0 ZIP directly,
- retries transient download failures,
- caches the archive/source under `.portable-cache`,
- removes corrupt downloads when extraction fails,
- clears stale/incomplete CMake build state,
- passes the extracted JUCE tree to CMake with `STEMLAB_JUCE_SOURCE_DIR`.

CMake retains a direct archive FetchContent fallback for developers invoking it
without the PowerShell build helper.

## r7 Ableton runtime installer reliability

The elevated Ableton installer no longer uses PowerShell `Copy-Item` for the
multi-gigabyte embedded Python/ML runtime. It now uses Windows Robocopy with
visible file activity, multithreaded copying, long/deep-tree handling, bounded
retries, and proper Robocopy exit-code checks.

Installer failures are also written to `%TEMP%\StemLab-Ableton-install.log`, and
the UAC-elevated PowerShell window stays open on failure so the actual error is
visible instead of disappearing immediately.

## r8 polished Windows setup wizard

- Added an Inno Setup 6.7+ GUI installer branded as **StemLab**.
- Added a dedicated **Choose what to install** page.
- The desktop app and bundled ML runtime are required.
- Ableton Live integration is optional and installs both `StemLab.vst3` and
  `StemLabRemote`.
- Common Ableton User Library locations are auto-detected, with a Browse button
  for custom locations.
- Start Menu and Desktop shortcuts are selectable.
- The installer uses the existing user-supplied low-contrast pink artwork for
  its icon/branding rather than generating replacement artwork.
- Added `build_installer_windows.ps1`, which builds the portable payload and
  compiles `dist\StemLab-Setup-0.9.9.exe`.
- Large payloads automatically switch to Inno Setup disk-spanning data files to
  avoid the Windows single-EXE size limit.

## r9 portable-first GitHub workflow + StemLab branding reset

- Restored the product name to **StemLab** in the Standalone UI and Windows title bar.
- Restored JUCE `COMPANY_NAME` / vendor metadata to **StemLab**.
- The primary end-user package is now a stable `StemLab-Windows.zip` GitHub Release asset.
- Added first-run onboarding with **Use Standalone** and **Set Up Ableton** choices.
- Added **Settings > Ableton Live > Install / Repair Ableton Integration...**.
- Ableton setup now installs only the VST3 and `StemLabRemote`; the VST3 reuses the portable `Engine\` runtime instead of copying it again.
- The Standalone app records the current portable engine path under LocalAppData so the VST3 can find it.
- Ableton User Library setup now falls back to a graphical folder picker when auto-detection fails.
- Ableton setup no longer force-closes Live; it asks the user to save and quit first.
- Added `START_HERE.txt` for the extracted release and a one-command `publish_github_release.ps1` helper for maintainers.
