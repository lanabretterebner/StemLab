#ifndef SourceDir
  #error SourceDir must be supplied by scripts/win/build_installer_windows.ps1
#endif
#ifndef AppVersion
  ; No fallback: a manual compile would silently stamp a stale number.
  ; build_installer_windows.ps1 reads the real version from pyproject.toml.
  #error AppVersion must be supplied by scripts/win/build_installer_windows.ps1
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
; Which torch build the bundled Engine carries. Read from the Engine itself
; by build_installer_windows.ps1 rather than passed down from a build label,
; so the filename cannot claim hardware support the payload does not have.
#ifndef Flavor
  #define Flavor "cpu"
#endif
; Whether to slice the installer into .bin files. build_installer_windows.ps1
; tries a single-file build first and only re-runs with spanning when the
; compiler refuses the size, so small flavors ship as one double-clickable
; .exe. Defaults to spanning for manual compiles, which always succeed.
#ifndef Spanning
  #define Spanning "yes"
#endif

#define AppName "StemLab"
#define AppPublisher "StemLab"

[Setup]
; NVIDIA and CPU are variants of the same installed product. Keep this AppId
; backend-independent so upgrades share one uninstall entry and uninstall log.
AppId={{6A4B97E7-8939-4BB8-B92C-86AF51922114}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
; One location, and the app knows it without being told.
;
; StemLab does not search for its Engine any more - the plug-in resolves
; exactly %LOCALAPPDATA%\StemLab\Engine\python.exe (StemLabPaths.cpp,
; engineExecutable) and nothing else, with $STEMLAB_ENGINE as the override.
; So the install directory is fixed rather than chosen: a directory page here
; would let someone put the Engine somewhere the plug-in will never look.
;
; That fixed directory is per-user, which also makes it correct. Inno's own
; documentation warns that the "user" constants "refer to the profile of the
; user running Setup" - under over-the-shoulder elevation that is the
; administrator, not the person at the keyboard, so an elevated Setup writing
; to {localappdata} can write to the wrong profile entirely. Not elevating at
; all is what makes {localappdata} mean what it says.
;
; UsePreviousAppDir=no for the same reason: an upgrade from a machine-wide
; 0.1.x install must land in the new fixed location, not reuse C:\Program
; Files\StemLab. InitializeSetup refuses that upgrade outright.
DefaultDirName={localappdata}\StemLab
UsePreviousAppDir=no
DisableDirPage=yes
UninstallLogMode=append
UsePreviousGroup=no
DefaultGroupName=StemLab
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=StemLab-Setup-{#AppVersion}-{#Flavor}
SetupIconFile={#SourceDir}\StemLabIcon.ico
Compression=lzma2/max
SolidCompression=yes
DiskSpanning={#Spanning}
DiskSliceSize=2100000000
WizardStyle=modern
UninstallDisplayIcon={app}\StemLab.exe
ChangesEnvironment=no

[Files]
; Keep the payload intact at build time, but install its VST3 bundle only to
; the VST3 directory. All other application/runtime files follow {app}, which
; includes Engine\ - the one place the plug-in looks for it.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "StemLab.vst3\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; The per-user VST3 location from Steinberg's own plug-in-locations page,
; where it is listed first. Nothing here needs administrator rights, which is
; the point: see the note on {localappdata} in [Setup].
Source: "{#SourceDir}\StemLab.vst3\*"; DestDir: "{localappdata}\Programs\Common\VST3\StemLab.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; The Engine is a Python installation: it writes __pycache__ beside its own
; modules the first time it runs, and the uninstall log only knows about files
; Setup put there. Scoped to Engine on purpose - {app} is also where the
; analysis cache and the downloaded model weights live (see stemlab/paths.py),
; and an uninstall that took the whole folder would take gigabytes of weights
; with it.
Type: filesandordirs; Name: "{app}\Engine"

[Icons]
Name: "{group}\StemLab"; Filename: "{app}\StemLab.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\StemLab"; Filename: "{app}\StemLab.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\install_ableton.ps1"""; Description: "Install/repair Ableton Live integration (close Ableton first)"; Flags: postinstall skipifsilent unchecked waituntilterminated
Filename: "{app}\StemLab.exe"; Description: "Launch StemLab"; Flags: nowait postinstall skipifsilent

[Code]
function MachineWideVst3: String;
begin
  Result := ExpandConstant('{commoncf64}\VST3\StemLab.vst3');
end;

function StaleEnginePointer: String;
begin
  Result := ExpandConstant('{localappdata}\StemLab\portable_engine_path.txt');
end;

{ The install directory is fixed, so there is nothing here about drive roots
  or the Windows folder any more - nobody can choose those. What is left is
  the question that can still go wrong: can Setup actually create and write
  the one directory it is going to use. A roaming profile on a full disk is a
  real answer of "no", and finding that out after unpacking gigabytes is not. }
function EnsureInstallDirectoryUsable(InstallDir: String; var ErrorText: String): Boolean;
var
  ProbeIndex: Integer;
  ProbeFile: String;
begin
  Result := False;
  InstallDir := RemoveBackslashUnlessRoot(Trim(InstallDir));

  try
    if not ForceDirectories(InstallDir) then
    begin
      ErrorText :=
        'Setup could not create the StemLab folder:' + #13#10 +
        InstallDir + #13#10 + #13#10 +
        'Check that there is free space and that your profile is available.';
      Exit;
    end;

    ProbeIndex := 0;
    repeat
      ProbeFile := AddBackslash(InstallDir) + '.stemlab-install-write-test-' +
                   IntToStr(ProbeIndex) + '.tmp';
      ProbeIndex := ProbeIndex + 1;
    until (not FileExists(ProbeFile) and not DirExists(ProbeFile)) or
          (ProbeIndex >= 100);

    if FileExists(ProbeFile) or DirExists(ProbeFile) or
       not SaveStringToFile(ProbeFile, 'StemLab installer write test', False) then
    begin
      ErrorText :=
        'Setup cannot write to the StemLab folder:' + #13#10 +
        InstallDir + #13#10 + #13#10 +
        'Check the folder permissions and that the disk is not full.';
      Exit;
    end;

    if not DeleteFile(ProbeFile) then
    begin
      ErrorText :=
        'Setup can create files in the StemLab folder but cannot remove them:' + #13#10 +
        InstallDir + #13#10 + #13#10 +
        'Check the folder permissions, and whether a security tool is holding'
        + #13#10 + 'files open there.';
      Exit;
    end;
  except
    ErrorText :=
      'Setup cannot use the StemLab folder:' + #13#10 +
      InstallDir + #13#10 + #13#10 +
      'Check that your profile is available and the folder is writable.';
    Exit;
  end;

  Result := True;
end;

{ A machine-wide 0.1.x install and this one would leave two copies of the
  plug-in in two folders a host scans, and the host would list both. Setup
  cannot remove the old one - it is under Program Files and this Setup never
  elevates - so it refuses and says exactly what to do instead. }
function InitializeSetup: Boolean;
begin
  Result := True;

  if DirExists(MachineWideVst3()) then
  begin
    MsgBox(
      'An older StemLab is installed for all users on this computer.' + #13#10 +
      #13#10 +
      'StemLab now installs for your account only. Installing over the old one'
      + #13#10 +
      'would leave two copies of the plug-in, and your DAW would list both.'
      + #13#10 + #13#10 +
      'Uninstall the old StemLab first - Settings, Apps, Installed apps - then'
      + #13#10 +
      'run this again. If it is no longer listed there, delete this folder as'
      + #13#10 +
      'an administrator:' + #13#10 + #13#10 + MachineWideVst3(),
      mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  EnsureInstallDirectoryUsable(ExpandConstant('{app}'), Result);
end;

procedure VerifyVst3;
var
  Vst3Module: String;
begin
  Vst3Module := ExpandConstant(
    '{localappdata}\Programs\Common\VST3\StemLab.vst3\Contents\x86_64-win\StemLab.vst3');
  if not FileExists(Vst3Module) then
    RaiseException(
      'StemLab VST3 installation failed.' + #13#10 +
      'The installed plug-in module is missing:' + #13#10 + Vst3Module + #13#10 +
      'Setup cannot complete successfully.');
end;

{ The one invariant this installer exists to hold. The plug-in resolves this
  exact path and does not look anywhere else, so an install that finished with
  the Engine somewhere else is an install that cannot separate anything - and
  it would only be discovered on the user's first attempt. }
procedure VerifyEngine;
var
  EnginePython: String;
begin
  EnginePython := ExpandConstant('{app}\Engine\python.exe');
  if not FileExists(EnginePython) then
    RaiseException(
      'StemLab Engine installation failed.' + #13#10 +
      'The plug-in looks for its Engine here and nowhere else:' + #13#10 +
      EnginePython + #13#10 +
      'Setup cannot complete successfully.');
end;

{ Written by 0.1.x, read by nothing since engine discovery was removed. It
  sits inside {app}, so it is ours to clear. }
procedure RemoveStaleEnginePointer;
begin
  if FileExists(StaleEnginePointer()) then
    DeleteFile(StaleEnginePointer());
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    VerifyVst3;
    VerifyEngine;
    RemoveStaleEnginePointer;
  end;
end;
