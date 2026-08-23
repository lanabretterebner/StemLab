# StemLab Windows Installer

The polished Windows installer is built with Inno Setup 6.7+.

From the repository root:

```powershell
.\build_installer_windows.ps1
```

The script first builds the complete portable payload, then compiles:

```text
dist\StemLab-Setup-0.9.9.exe
```

The installer presents a dedicated **Choose what to install** screen:

- **StemLab Desktop App** — required
- **Ableton Live Integration** — optional; auto-detects common User Library locations and allows browsing
- **Start Menu shortcut** — optional
- **Desktop shortcut** — optional

The Ableton option installs both `StemLab.vst3` and `StemLabRemote` while preserving the internal StemLab product identifiers required by existing sessions and the Remote Script protocol.

If the complete payload is too large for one Windows setup executable, the build script automatically enables Inno Setup disk spanning. In that case, distribute the generated `.exe` and `.bin` files together.
