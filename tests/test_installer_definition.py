"""The Windows installer definition, read as text.

There is no Windows machine here to run it on, so what can be checked is what
the definition says - and the one thing worth checking hardest is that the
installer and the plug-in name the same Engine location. They disagreed once:
the plug-in stopped searching and resolved %LOCALAPPDATA%\\StemLab\\Engine
while the installer was still putting the Engine under Program Files and
writing a pointer file at it. Every Windows install in that window could not
find its Engine, and nothing failed until the user's first separation.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISS = (ROOT / "scripts" / "win" / "StemLab.iss").read_text(encoding="utf-8")
BUILDER = (ROOT / "scripts" / "win" / "build_installer_windows.ps1").read_text(encoding="utf-8")
ABLETON_INSTALLER = (ROOT / "scripts" / "win" / "install_ableton.ps1").read_text(encoding="utf-8")
SETUP = (ROOT / "scripts" / "win" / "StemLab-Windows-setup.ps1").read_text(encoding="utf-8")
PATHS_CPP = (ROOT / "src" / "plugin" / "Source" / "StemLabPaths.cpp").read_text(encoding="utf-8")

PER_USER_VST3 = r"{localappdata}\Programs\Common\VST3\StemLab.vst3"


class TestTheCompilerRulesThatAreInvisibleUntilItRuns:
    """Two ways to write a StemLab.iss that reads fine and will not compile.

    There is no Windows machine in CI, so ISCC only ever runs during a release
    - which makes a compile error cost a whole release cycle to find. Both of
    these cost one.
    """

    def test_no_line_outside_the_preamble_starts_with_a_hash(self):
        # The preprocessor reads any line whose first non-whitespace character
        # is "#" as a directive, so a Pascal string continued as
        #
        #     #13#10 +
        #
        # aborts the compile with "Unknown preprocessor directive" before the
        # Pascal is ever parsed. Continuation lines start with "+" instead.
        lines = ISS.splitlines()
        first_section = next(
            index for index, line in enumerate(lines) if line.startswith("[")
        )

        offenders = [
            f"{index + 1}: {line.strip()}"
            for index, line in enumerate(lines[first_section:], start=first_section)
            if line.lstrip().startswith("#")
        ]

        assert offenders == []

    def test_the_code_section_has_no_brace_comments(self):
        # An Inno constant inside one closes it at its own brace: a comment
        # saying "it sits inside {app}" ends at "{app}" and leaves the rest of
        # the sentence as code. Line comments cannot do that.
        code = ISS.partition("[Code]")[2]

        offenders = [
            line.strip()
            for line in code.splitlines()
            if line.lstrip().startswith("{")
        ]

        assert offenders == []


def test_flavor_variants_share_one_uninstall_identity():
    # cpu/cuda/xpu installers are variants of one installed product: the
    # flavor may name the setup file, never the uninstall identity.
    app_id_line = next(line for line in ISS.splitlines() if line.startswith("AppId="))
    assert "Flavor" not in app_id_line
    assert "UninstallLogMode=append" in ISS


class TestOneEngineLocation:
    """The invariant the whole installer exists to hold."""

    def test_the_installer_and_the_plugin_agree_on_where_the_engine_is(self):
        # Two halves of one path. The installer decides {app}; the plug-in
        # composes the same thing from its own constants and never looks
        # anywhere else, so if these drift apart Windows silently loses its
        # Engine.
        assert r"DefaultDirName={localappdata}\StemLab" in ISS

        windows_data = re.search(
            r"juce::File userDataDirectory\(\)\s*\{(.*?)\n\}", PATHS_CPP, re.S
        )
        assert windows_data is not None
        assert 'windowsLocalAppData().getChildFile ("StemLab")' in windows_data.group(1)

        engine = re.search(
            r"juce::File engineExecutable\(\)\s*\{(.*?)\n\}", PATHS_CPP, re.S
        )
        assert engine is not None

        windows_branch = engine.group(1).split("#if JUCE_WINDOWS")[1].split("#else")[0]
        assert "userDataDirectory()" in windows_branch
        assert '"Engine"' in windows_branch
        assert '"python.exe"' in windows_branch

    def test_the_engine_follows_the_application_directory(self):
        application_line = next(
            line for line in ISS.splitlines() if 'Source: "{#SourceDir}\\*"' in line
        )
        assert 'DestDir: "{app}"' in application_line
        # Only the VST3 is carved out. Excluding Engine here would put it
        # somewhere the plug-in does not look.
        assert 'Excludes: "StemLab.vst3\\*"' in application_line
        assert "Engine" not in application_line.split("Excludes:")[1]

    def test_setup_refuses_to_finish_without_the_engine_in_place(self):
        assert "procedure VerifyEngine" in ISS
        assert r"ExpandConstant('{app}\Engine\python.exe')" in ISS
        assert "VerifyEngine;" in ISS

    def test_nothing_writes_or_reads_an_engine_pointer(self):
        # The pointer is what "the app just knows" replaced. The only mention
        # left is the one that deletes the file 0.1.x wrote.
        assert "WriteEnginePointer" not in ISS
        assert "RemoveStaleEnginePointer" in ISS
        assert "portable_engine_path" not in ABLETON_INSTALLER


class TestItInstallsForOneUserAndNeverElevates:
    def test_setup_asks_for_no_administrator_rights(self):
        assert "PrivilegesRequired=lowest" in ISS
        assert "PrivilegesRequired=admin" not in ISS

    def test_the_directory_is_fixed_rather_than_chosen(self):
        # A directory page would let someone put the Engine where the plug-in
        # will never look, and UsePreviousAppDir would reuse a machine-wide
        # 0.1.x directory on upgrade.
        assert "DisableDirPage=yes" in ISS
        assert "UsePreviousAppDir=no" in ISS

    def test_the_vst3_goes_to_the_per_user_location(self):
        vst3_line = next(
            line
            for line in ISS.splitlines()
            if 'Source: "{#SourceDir}\\StemLab.vst3\\*"' in line
        )
        assert f'DestDir: "{PER_USER_VST3}"' in vst3_line

        module = PER_USER_VST3 + r"\Contents\x86_64-win\StemLab.vst3"
        assert module in ISS
        assert "FileExists(Vst3Module)" in ISS
        assert "RaiseException(" in ISS

    def test_an_older_machine_wide_install_is_refused_with_instructions(self):
        # Both would be scanned, and the DAW would list StemLab twice. This
        # Setup cannot remove the old one - it never elevates - so it says
        # what to do instead of installing on top.
        assert "function InitializeSetup" in ISS
        assert "DirExists(MachineWideVst3())" in ISS
        assert r"ExpandConstant('{commoncf64}\VST3\StemLab.vst3')" in ISS
        assert "Installed apps" in ISS

    def test_the_download_helper_no_longer_promises_a_uac_prompt(self):
        assert "administrator prompt" not in SETUP
        assert "for your account only" in SETUP

    def test_the_download_stages_beside_the_install_it_feeds(self):
        # Same volume as %LOCALAPPDATA%\StemLab, so nothing crosses a disk,
        # and it survives a reboot - which %TEMP% under Storage Sense does
        # not, and this is the folder a half-finished multi-gigabyte download
        # has to survive in.
        assert 'Join-Path $env:LOCALAPPDATA "StemLab\\Setup"' in SETUP
        assert "$Stage = $env:STEMLAB_SETUP_STAGE" in SETUP


class TestUninstallTakesTheAppAndNothingElse:
    def test_it_removes_the_engine_it_cannot_track(self):
        # A Python installation writes __pycache__ beside its own modules, and
        # the uninstall log only knows the files Setup wrote.
        assert "[UninstallDelete]" in ISS
        assert r'Type: filesandordirs; Name: "{app}\Engine"' in ISS

    def test_it_never_removes_the_application_directory_wholesale(self):
        # {app} also holds the analysis cache and the downloaded model
        # weights - gigabytes of them - so a blanket entry would be the
        # Windows version of the bug uninstall.sh is shaped around.
        deletes = ISS.partition("[UninstallDelete]")[2].partition("\n[")[0]
        for line in deletes.splitlines():
            if not line.startswith("Type:"):
                continue
            assert '"{app}"' not in line
            assert r'"{app}\Models"' not in line
            assert r'"{app}\Analysis"' not in line


class TestTheBuildChecksThePayload:
    def test_vst3_module_is_checked_before_compilation(self):
        module = r"StemLab.vst3\Contents\x86_64-win\StemLab.vst3"
        assert module in BUILDER
        assert "Portable VST3 module is missing" in BUILDER


class TestAbletonSetup:
    def test_it_installs_the_vst3_where_the_installer_does(self):
        assert '$RepoRoot = Split-Path $PSScriptRoot -Parent' in ABLETON_INSTALLER
        assert (
            '$Vst3Root = Join-Path $env:LOCALAPPDATA "Programs\\Common\\VST3"'
            in ABLETON_INSTALLER
        )
        assert '$VstDestination = Join-Path $Vst3Root "StemLab.vst3"' in ABLETON_INSTALLER

    def test_the_legacy_path_is_not_an_alias_for_the_current_one(self):
        # They were the same string, so "remove the legacy VST3" deleted the
        # one just installed - and with the installer excluding the bundle
        # from {app} there was nothing left to reinstall it from.
        legacy = next(
            line for line in ABLETON_INSTALLER.splitlines()
            if line.startswith("$LegacyVstDestination")
        )
        current = next(
            line for line in ABLETON_INSTALLER.splitlines()
            if line.startswith("$VstDestination")
        )
        assert "CommonProgramFiles" in legacy
        assert "CommonProgramFiles" not in current

    def test_only_the_legacy_removal_elevates(self):
        # Everything else writes inside this user's profile. An elevated
        # process under over-the-shoulder elevation belongs to the
        # administrator, so $env:LOCALAPPDATA and the Ableton User Library
        # would be their profile rather than the one at the keyboard.
        assert "Test-Administrator" not in ABLETON_INSTALLER
        assert ABLETON_INSTALLER.count("-Verb RunAs") == 1

        elevated = ABLETON_INSTALLER.partition("-Verb RunAs")[0].rsplit("if (", 1)[1]
        assert "$LegacyVstDestination" in elevated

    def test_the_installer_still_offers_it_from_the_app_directory(self):
        assert (
            r'Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy '
            r'Bypass -File ""{app}\scripts\install_ableton.ps1"""' in ISS
        )
