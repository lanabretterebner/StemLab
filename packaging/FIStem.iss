#ifndef SourceDir
  #error SourceDir must be supplied by build_installer_windows.ps1
#endif
#ifndef AppVersion
  #define AppVersion "0.9.9"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppName "FI-STEM"
#define AppPublisher "FI-STEM"

[Setup]
AppId={{6A4B97E7-8939-4BB8-B92C-86AF51922114}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\FI-STEM
UsePreviousAppDir=no
UsePreviousGroup=no
DefaultGroupName=FI-STEM
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=FI-STEM-Setup-{#AppVersion}
SetupIconFile={#SourceDir}\FIStemIcon.ico
Compression=lzma2/max
SolidCompression=yes
DiskSpanning=yes
DiskSliceSize=2100000000
WizardStyle=modern
UninstallDisplayIcon={app}\FI-STEM.exe
ChangesEnvironment=no

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\FI-STEM.vst3\*"; DestDir: "{commoncf64}\VST3\FI-STEM.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\FI-STEM"; Filename: "{app}\FI-STEM.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\FI-STEM"; Filename: "{app}\FI-STEM.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\install_ableton.ps1"""; Description: "Install/repair Ableton Live integration (close Ableton first)"; Flags: postinstall skipifsilent unchecked waituntilterminated
Filename: "{app}\FI-STEM.exe"; Description: "Launch FI-STEM"; Flags: nowait postinstall skipifsilent

[Code]
procedure WriteEnginePointer;
var
  FiStemData: String;
  PointerFile: String;
  EnginePython: String;
begin
  FiStemData := ExpandConstant('{localappdata}\FI-STEM');
  PointerFile := FiStemData + '\portable_engine_path.txt';
  EnginePython := ExpandConstant('{app}\Engine\python.exe');
  ForceDirectories(FiStemData);
  SaveStringToFile(PointerFile, EnginePython + #13#10, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    WriteEnginePointer;
end;
