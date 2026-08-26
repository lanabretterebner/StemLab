#ifndef SourceDir
  #error SourceDir must be supplied by scripts/build_installer_windows.ps1
#endif
#ifndef AppVersion
  #define AppVersion "0.9.9"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppName "StemLab"
#define AppPublisher "StemLab"

[Setup]
AppId={{6A4B97E7-8939-4BB8-B92C-86AF51922114}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\StemLab
UsePreviousAppDir=no
UsePreviousGroup=no
DefaultGroupName=StemLab
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=StemLab-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
DiskSpanning=yes
DiskSliceSize=2100000000
WizardStyle=modern
UninstallDisplayIcon={app}\StemLab.exe
ChangesEnvironment=no

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
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
procedure WriteEnginePointer;
var
  FiStemData: String;
  PointerFile: String;
  EnginePython: String;
begin
  FiStemData := ExpandConstant('{localappdata}\StemLab');
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
