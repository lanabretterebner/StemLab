#ifndef SourceDir
  #error SourceDir must be supplied by scripts/win/build_installer_windows.ps1
#endif
#ifndef AppVersion
  #define AppVersion "0.9.9"
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
DefaultDirName={autopf}\StemLab
UsePreviousAppDir=yes
DisableDirPage=no
UninstallLogMode=append
UsePreviousGroup=no
DefaultGroupName=StemLab
DisableProgramGroupPage=yes
PrivilegesRequired=admin
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
; Keep the portable payload intact at build time, but install its VST3 bundle only
; to the system VST3 directory. All other application/runtime files follow {app}.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "StemLab.vst3\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\StemLab.vst3\*"; DestDir: "{commoncf64}\VST3\StemLab.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\StemLab"; Filename: "{app}\StemLab.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\StemLab"; Filename: "{app}\StemLab.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\install_ableton.ps1"""; Description: "Install/repair Ableton Live integration (close Ableton first)"; Flags: postinstall skipifsilent unchecked waituntilterminated
Filename: "{app}\StemLab.exe"; Description: "Launch StemLab"; Flags: nowait postinstall skipifsilent

[Code]
function EnginePointerFile: String;
begin
  Result := ExpandConstant('{localappdata}\StemLab\portable_engine_path.txt');
end;

function ValidateInstallDirectory(InstallDir: String; var ErrorText: String): Boolean;
var
  ProbeIndex: Integer;
  ProbeFile: String;
begin
  Result := False;
  InstallDir := RemoveBackslashUnlessRoot(Trim(InstallDir));

  if (Length(InstallDir) < 3) or (InstallDir[2] <> ':') or
     ((InstallDir[3] <> '\') and (InstallDir[3] <> '/')) then
  begin
    ErrorText :=
      'Choose a complete local Windows path, for example:' + #13#10 +
      'D:\Audio Tools\StemLab';
    Exit;
  end;

  if Length(InstallDir) = 3 then
  begin
    ErrorText :=
      'StemLab cannot be installed directly into a drive root.' + #13#10 +
      'Choose a folder such as D:\Audio Tools\StemLab.';
    Exit;
  end;

  if CompareText(InstallDir, RemoveBackslashUnlessRoot(ExpandConstant('{win}'))) = 0 then
  begin
    ErrorText :=
      'StemLab cannot be installed directly into the Windows system folder.' + #13#10 +
      'Choose a dedicated application folder.';
    Exit;
  end;

  try
    if not ForceDirectories(InstallDir) then
    begin
      ErrorText :=
        'Setup could not create the selected StemLab folder:' + #13#10 +
        InstallDir + #13#10 + #13#10 + 'Choose another folder.';
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
        'Setup cannot write to the selected StemLab folder:' + #13#10 +
        InstallDir + #13#10 + #13#10 +
       'Check the folder permissions or choose another location.';
      Exit;
    end;

    if not DeleteFile(ProbeFile) then
    begin
      ErrorText :=
        'Setup can create files in the selected StemLab folder but cannot remove them:' + #13#10 +
        InstallDir + #13#10 + #13#10 +
        'Check the folder permissions or choose another location.';
      Exit;
    end;
  except
    ErrorText :=
      'Setup cannot use the selected StemLab folder:' + #13#10 +
      InstallDir + #13#10 + #13#10 +
      'Check that the drive is available and the folder is writable.';
    Exit;
  end;

  Result := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ErrorText: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and
     not ValidateInstallDirectory(WizardDirValue, ErrorText) then
  begin
    MsgBox(ErrorText, mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  ValidateInstallDirectory(ExpandConstant('{app}'), Result);
end;

procedure WriteEnginePointer;
var
  FiStemData: String;
  EnginePython: String;
begin
  FiStemData := ExpandConstant('{localappdata}\StemLab');
  EnginePython := ExpandConstant('{app}\Engine\python.exe');
  ForceDirectories(FiStemData);
  SaveStringToFile(EnginePointerFile(), EnginePython + #13#10, False);
end;

procedure RemoveEnginePointerIfOwnedByThisInstall;
var
  PointerLines: TArrayOfString;
begin
  if LoadStringsFromFile(EnginePointerFile(), PointerLines) and
     (GetArrayLength(PointerLines) > 0) and
     (CompareText(Trim(PointerLines[0]),
                  ExpandConstant('{app}\Engine\python.exe')) = 0) then
    DeleteFile(EnginePointerFile());
end;

procedure VerifySystemVst3;
var
  Vst3Module: String;
begin
  Vst3Module := ExpandConstant(
    '{commoncf64}\VST3\StemLab.vst3\Contents\x86_64-win\StemLab.vst3');
  if not FileExists(Vst3Module) then
    RaiseException(
      'StemLab VST3 installation failed.' + #13#10 +
      'The installed plug-in module is missing:' + #13#10 + Vst3Module + #13#10 +
      'Setup cannot complete successfully.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    VerifySystemVst3;
    WriteEnginePointer;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveEnginePointerIfOwnedByThisInstall;
end;
