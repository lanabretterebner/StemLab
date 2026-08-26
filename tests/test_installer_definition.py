import re
from pathlib import Path, PureWindowsPath


ROOT = Path(__file__).resolve().parents[1]
ISS = (ROOT / "packaging" / "StemLab.iss").read_text(encoding="utf-8")
BUILDER = (ROOT / "scripts" / "build_installer_windows.ps1").read_text(encoding="utf-8")
ABLETON_INSTALLER = (ROOT / "scripts" / "install_ableton.ps1").read_text(encoding="utf-8")


def test_flavor_variants_share_one_uninstall_identity():
    # cpu/cuda/xpu installers are variants of one installed product: the
    # flavor may name the setup file, never the uninstall identity.
    app_id_line = next(line for line in ISS.splitlines() if line.startswith("AppId="))
    assert "Flavor" not in app_id_line
    assert "UsePreviousAppDir=yes" in ISS
    assert "UninstallLogMode=append" in ISS


def test_application_directory_is_selectable_and_reused():
    assert r"DefaultDirName={autopf}\StemLab" in ISS
    assert "DisableDirPage=no" in ISS
    assert "UsePreviousAppDir=yes" in ISS
    assert "PrivilegesRequired=admin" in ISS
    assert "ValidateInstallDirectory(WizardDirValue" in ISS
    assert "SaveStringToFile(ProbeFile, 'StemLab installer write test'" in ISS


def test_application_payload_follows_app_but_vst3_does_not():
    application_line = next(
        line for line in ISS.splitlines() if 'Source: "{#SourceDir}\\*"' in line
    )
    assert 'DestDir: "{app}"' in application_line
    assert 'Excludes: "StemLab.vst3\\*"' in application_line
    assert "[UninstallDelete]" not in ISS


def test_vst3_destination_and_installed_module_are_verified():
    bundle = r"{commoncf64}\VST3\StemLab.vst3"
    module = bundle + r"\Contents\x86_64-win\StemLab.vst3"
    assert f'DestDir: "{bundle}"' in ISS
    assert module in ISS
    assert "FileExists(Vst3Module)" in ISS
    assert "RaiseException(" in ISS


def test_portable_vst3_module_is_checked_before_compilation():
    module = r"StemLab.vst3\Contents\x86_64-win\StemLab.vst3"
    assert module in BUILDER
    assert "Portable VST3 module is missing" in BUILDER


def test_custom_install_path_generates_exact_engine_pointer():
    match = re.search(
        r"EnginePython := ExpandConstant\('(?P<template>\{app\}[^']+)'\);", ISS
    )
    assert match is not None

    install_dir = PureWindowsPath(r"D:\Audio Tools\StemLab")
    generated_pointer = match.group("template").replace("{app}", str(install_dir))
    assert generated_pointer == r"D:\Audio Tools\StemLab\Engine\python.exe"


def test_ableton_setup_uses_selected_app_root_and_system_vst3_fallback():
    assert '$RepoRoot = Split-Path $PSScriptRoot -Parent' in ABLETON_INSTALLER
    assert '$PortableEnginePython = Join-Path $RepoRoot "Engine\\python.exe"' in ABLETON_INSTALLER
    assert "StemLab.vst3 is already installed in the system VST3 directory." in ABLETON_INSTALLER
    assert r'Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\install_ableton.ps1"""' in ISS


def test_uninstall_only_removes_engine_pointer_owned_by_selected_app():
    assert "RemoveEnginePointerIfOwnedByThisInstall" in ISS
    assert "LoadStringsFromFile(EnginePointerFile(), PointerLines)" in ISS
    assert "ExpandConstant('{app}\\Engine\\python.exe')" in ISS
