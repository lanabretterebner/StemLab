#ifndef SourceDir
  #define SourceDir "..\dist\StemLab-0.9.9-Windows"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef AssetDir
  #define AssetDir "..\assets"
#endif
#ifndef AppVersion
  #define AppVersion "0.9.9"
#endif

#define AppName "StemLab"
#define SafeAppName "StemLab"
#define ProductId "{{3E2CBF6D-36CE-48D2-B589-8DA99525529B}"

[Setup]
AppId={#ProductId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
DefaultDirName={localappdata}\Programs\StemLab
DefaultGroupName={#SafeAppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
AlwaysUsePersonalGroup=yes
UsedUserAreasWarning=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=StemLab-Setup-{#AppVersion}
SetupIconFile={#AssetDir}\StemLab.ico
WizardSmallImageFile={#AssetDir}\StemLabWizardSmall.bmp
WizardStyle=modern dynamic windows11
WizardResizable=yes
WizardSizePercent=120
Compression=lzma2
SolidCompression=yes
LZMAUseSeparateProcess=yes
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\StemLab.exe
VersionInfoVersion=0.9.9.0
VersionInfoDescription={#AppName} installer
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoCompany=StemLab
VersionInfoCopyright=StemLab contributors
#ifdef UseDiskSpanning
DiskSpanning=yes
DiskSliceSize=2000000000
#else
DiskSpanning=no
#endif

[Files]
; Core desktop application (required)
Source: "{#SourceDir}\StemLab.exe"; DestDir: "{app}"; DestName: "StemLab.exe"; Flags: ignoreversion
Source: "{#SourceDir}\Engine\*"; DestDir: "{localappdata}\StemLab\Engine"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\FFMPEG_BUILD_INFO.txt"; DestDir: "{app}"; Flags: ignoreversion

; Optional Ableton integration
Source: "{#SourceDir}\StemLab.vst3\*"; DestDir: "{commoncf}\VST3\StemLab.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: ShouldInstallAbleton
Source: "{#SourceDir}\StemLabRemote\*"; DestDir: "{code:GetRemoteScriptDir}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: ShouldInstallAbleton

[Icons]
Name: "{group}\StemLab"; Filename: "{app}\StemLab.exe"; Check: ShouldCreateStartMenuShortcut
Name: "{userdesktop}\StemLab"; Filename: "{app}\StemLab.exe"; Check: ShouldCreateDesktopShortcut

[Run]
Filename: "{app}\StemLab.exe"; Description: "Launch StemLab"; Flags: nowait postinstall skipifsilent

[Code]
var
  OptionsPage: TWizardPage;
  CoreCheck: TNewCheckBox;
  CoreDescription: TNewStaticText;
  AbletonCheck: TNewCheckBox;
  AbletonDescription: TNewStaticText;
  AbletonStatus: TNewStaticText;
  UserLibraryLabel: TNewStaticText;
  UserLibraryEdit: TNewEdit;
  BrowseButton: TNewButton;
  ShortcutHeading: TNewStaticText;
  DesktopCheck: TNewCheckBox;
  StartMenuCheck: TNewCheckBox;
  AbletonDetected: Boolean;

function CleanDir(const Value: String): String;
begin
  Result := Trim(Value);
  while (Length(Result) > 3) and
        ((Result[Length(Result)] = '\') or (Result[Length(Result)] = '/')) do
    Delete(Result, Length(Result), 1);
end;

function TryUserLibraryCandidate(const Candidate: String; var Found: String): Boolean;
var
  C: String;
begin
  C := CleanDir(Candidate);
  Result := (C <> '') and DirExists(C);
  if Result then
    Found := C;
end;

function DetectUserLibrary(): String;
var
  Candidate: String;
  OneDrivePath: String;
begin
  Result := '';

  Candidate := ExpandConstant('{userdocs}\Ableton\User Library');
  if TryUserLibraryCandidate(Candidate, Result) then
    Exit;

  OneDrivePath := GetEnv('OneDrive');
  if OneDrivePath <> '' then
  begin
    Candidate := AddBackslash(OneDrivePath) + 'Documents\Ableton\User Library';
    if TryUserLibraryCandidate(Candidate, Result) then
      Exit;
  end;

  OneDrivePath := GetEnv('OneDriveConsumer');
  if OneDrivePath <> '' then
  begin
    Candidate := AddBackslash(OneDrivePath) + 'Documents\Ableton\User Library';
    if TryUserLibraryCandidate(Candidate, Result) then
      Exit;
  end;

  OneDrivePath := GetEnv('OneDriveCommercial');
  if OneDrivePath <> '' then
  begin
    Candidate := AddBackslash(OneDrivePath) + 'Documents\Ableton\User Library';
    if TryUserLibraryCandidate(Candidate, Result) then
      Exit;
  end;
end;

function DetectAbleton(): Boolean;
begin
  Result := DirExists(ExpandConstant('{commonappdata}\Ableton')) or
            DirExists(ExpandConstant('{userappdata}\Ableton'));
end;

procedure UpdateAbletonControls();
var
  Enabled: Boolean;
begin
  Enabled := False;
  if Assigned(AbletonCheck) then
    Enabled := AbletonCheck.Checked;

  if Assigned(UserLibraryLabel) then
    UserLibraryLabel.Enabled := Enabled;
  if Assigned(UserLibraryEdit) then
    UserLibraryEdit.Enabled := Enabled;
  if Assigned(BrowseButton) then
    BrowseButton.Enabled := Enabled;
end;

procedure AbletonClick(Sender: TObject);
begin
  UpdateAbletonControls();
end;

procedure BrowseUserLibrary(Sender: TObject);
var
  Chosen: String;
begin
  Chosen := UserLibraryEdit.Text;
  if Chosen = '' then
    Chosen := ExpandConstant('{userdocs}');

  if BrowseForFolder('Choose your Ableton User Library folder', Chosen, False) then
  begin
    UserLibraryEdit.Text := CleanDir(Chosen);
    AbletonStatus.Caption := 'User Library selected';
  end;
end;

function ShouldInstallAbleton(): Boolean;
begin
  { [Files] Check expressions can be evaluated before InitializeWizard. }
  Result := False;
  if Assigned(AbletonCheck) then
    Result := AbletonCheck.Checked;
end;

function ShouldCreateDesktopShortcut(): Boolean;
begin
  { [Icons] Check expressions can be evaluated before InitializeWizard. }
  Result := False;
  if Assigned(DesktopCheck) then
    Result := DesktopCheck.Checked;
end;

function ShouldCreateStartMenuShortcut(): Boolean;
begin
  { [Icons] Check expressions can be evaluated before InitializeWizard. }
  Result := False;
  if Assigned(StartMenuCheck) then
    Result := StartMenuCheck.Checked;
end;

function GetRemoteScriptDir(Param: String): String;
var
  LibraryPath: String;
begin
  { Code constants may also be expanded before InitializeWizard. }
  LibraryPath := '';

  if Assigned(UserLibraryEdit) then
    LibraryPath := CleanDir(UserLibraryEdit.Text);

  if LibraryPath = '' then
    LibraryPath := DetectUserLibrary();

  if LibraryPath = '' then
    LibraryPath := ExpandConstant('{userdocs}\Ableton\User Library');

  Result := AddBackslash(LibraryPath) + 'Remote Scripts\StemLabRemote';
end;

procedure InitializeWizard();
var
  DetectedLibrary: String;
  TopY: Integer;
begin
  OptionsPage := CreateCustomPage(
    wpWelcome,
    'Choose what to install',
    'StemLab is required. Add the Ableton integration and shortcuts you want.'
  );

  TopY := ScaleY(6);

  CoreCheck := TNewCheckBox.Create(OptionsPage);
  CoreCheck.Parent := OptionsPage.Surface;
  CoreCheck.Left := ScaleX(4);
  CoreCheck.Top := TopY;
  CoreCheck.Width := OptionsPage.SurfaceWidth - ScaleX(8);
  CoreCheck.Caption := 'StemLab Desktop App   •   Required';
  CoreCheck.Checked := True;
  CoreCheck.Enabled := False;
  CoreCheck.Font.Style := [fsBold];
  CoreCheck.Font.Size := 11;

  CoreDescription := TNewStaticText.Create(OptionsPage);
  CoreDescription.Parent := OptionsPage.Surface;
  CoreDescription.Left := ScaleX(28);
  CoreDescription.Top := CoreCheck.Top + ScaleY(28);
  CoreDescription.Width := OptionsPage.SurfaceWidth - ScaleX(40);
  CoreDescription.Height := ScaleY(34);
  CoreDescription.AutoSize := False;
  CoreDescription.WordWrap := True;
  CoreDescription.Caption := 'Six-stem desktop separator with the bundled CUDA/ML runtime. No separate Python install is required.';

  AbletonCheck := TNewCheckBox.Create(OptionsPage);
  AbletonCheck.Parent := OptionsPage.Surface;
  AbletonCheck.Left := ScaleX(4);
  AbletonCheck.Top := CoreDescription.Top + ScaleY(48);
  AbletonCheck.Width := OptionsPage.SurfaceWidth - ScaleX(8);
  AbletonCheck.Caption := 'Ableton Live Integration';
  AbletonCheck.Font.Style := [fsBold];
  AbletonCheck.Font.Size := 11;
  AbletonCheck.OnClick := @AbletonClick;

  AbletonDescription := TNewStaticText.Create(OptionsPage);
  AbletonDescription.Parent := OptionsPage.Surface;
  AbletonDescription.Left := ScaleX(28);
  AbletonDescription.Top := AbletonCheck.Top + ScaleY(27);
  AbletonDescription.Width := OptionsPage.SurfaceWidth - ScaleX(40);
  AbletonDescription.Height := ScaleY(32);
  AbletonDescription.AutoSize := False;
  AbletonDescription.WordWrap := True;
  AbletonDescription.Caption := 'Installs StemLab.vst3 plus StemLabRemote so separated stems can be sent directly into Ableton.';

  AbletonStatus := TNewStaticText.Create(OptionsPage);
  AbletonStatus.Parent := OptionsPage.Surface;
  AbletonStatus.Left := ScaleX(28);
  AbletonStatus.Top := AbletonDescription.Top + ScaleY(33);
  AbletonStatus.Width := OptionsPage.SurfaceWidth - ScaleX(40);
  AbletonStatus.Font.Style := [fsBold];

  DetectedLibrary := DetectUserLibrary();
  AbletonDetected := DetectAbleton();

  if DetectedLibrary <> '' then
  begin
    AbletonCheck.Checked := True;
    if AbletonDetected then
      AbletonStatus.Caption := 'Ableton Live detected • User Library found'
    else
      AbletonStatus.Caption := 'Ableton User Library found';
  end
  else
  begin
    AbletonCheck.Checked := False;
    if AbletonDetected then
      AbletonStatus.Caption := 'Ableton Live detected • choose your User Library below'
    else
      AbletonStatus.Caption := 'Ableton was not detected automatically';
  end;

  UserLibraryLabel := TNewStaticText.Create(OptionsPage);
  UserLibraryLabel.Parent := OptionsPage.Surface;
  UserLibraryLabel.Left := ScaleX(28);
  UserLibraryLabel.Top := AbletonStatus.Top + ScaleY(24);
  UserLibraryLabel.Caption := 'Ableton User Library:';

  UserLibraryEdit := TNewEdit.Create(OptionsPage);
  UserLibraryEdit.Parent := OptionsPage.Surface;
  UserLibraryEdit.Left := ScaleX(28);
  UserLibraryEdit.Top := UserLibraryLabel.Top + ScaleY(20);
  UserLibraryEdit.Width := OptionsPage.SurfaceWidth - ScaleX(118);
  UserLibraryEdit.Text := DetectedLibrary;

  BrowseButton := TNewButton.Create(OptionsPage);
  BrowseButton.Parent := OptionsPage.Surface;
  BrowseButton.Left := UserLibraryEdit.Left + UserLibraryEdit.Width + ScaleX(8);
  BrowseButton.Top := UserLibraryEdit.Top - ScaleY(1);
  BrowseButton.Width := ScaleX(78);
  BrowseButton.Height := UserLibraryEdit.Height + ScaleY(2);
  BrowseButton.Caption := 'Browse...';
  BrowseButton.OnClick := @BrowseUserLibrary;

  ShortcutHeading := TNewStaticText.Create(OptionsPage);
  ShortcutHeading.Parent := OptionsPage.Surface;
  ShortcutHeading.Left := ScaleX(4);
  ShortcutHeading.Top := UserLibraryEdit.Top + ScaleY(48);
  ShortcutHeading.Caption := 'Shortcuts';
  ShortcutHeading.Font.Style := [fsBold];
  ShortcutHeading.Font.Size := 10;

  StartMenuCheck := TNewCheckBox.Create(OptionsPage);
  StartMenuCheck.Parent := OptionsPage.Surface;
  StartMenuCheck.Left := ScaleX(28);
  StartMenuCheck.Top := ShortcutHeading.Top + ScaleY(25);
  StartMenuCheck.Width := ScaleX(190);
  StartMenuCheck.Caption := 'Start Menu shortcut';
  StartMenuCheck.Checked := True;

  DesktopCheck := TNewCheckBox.Create(OptionsPage);
  DesktopCheck.Parent := OptionsPage.Surface;
  DesktopCheck.Left := ScaleX(240);
  DesktopCheck.Top := StartMenuCheck.Top;
  DesktopCheck.Width := ScaleX(170);
  DesktopCheck.Caption := 'Desktop shortcut';
  DesktopCheck.Checked := False;

  UpdateAbletonControls();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  LibraryPath: String;
begin
  Result := True;

  if (CurPageID = OptionsPage.ID) and AbletonCheck.Checked then
  begin
    LibraryPath := CleanDir(UserLibraryEdit.Text);
    if (LibraryPath = '') or not DirExists(LibraryPath) then
    begin
      MsgBox(
        'Ableton integration is selected, but the User Library folder could not be found.' + #13#10 + #13#10 +
        'In Ableton, right-click User Library in the Browser and choose Show in Explorer, then select that folder here.',
        mbError,
        MB_OK
      );
      Result := False;
      Exit;
    end;

    UserLibraryEdit.Text := LibraryPath;
  end;
end;

function UpdateReadyMemo(
  Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Install:' + NewLine +
            '  • StemLab Desktop App' + NewLine;

  if ShouldInstallAbleton() then
    Result := Result + '  • Ableton Live Integration' + NewLine +
              '      ' + UserLibraryEdit.Text + NewLine;

  if ShouldCreateStartMenuShortcut() or ShouldCreateDesktopShortcut() then
  begin
    Result := Result + NewLine + 'Shortcuts:' + NewLine;
    if ShouldCreateStartMenuShortcut() then
      Result := Result + '  • Start Menu' + NewLine;
    if ShouldCreateDesktopShortcut() then
      Result := Result + '  • Desktop' + NewLine;
  end;

  Result := Result + NewLine +
            'StemLab will install its private ML runtime under your Local AppData folder.';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
  begin
    if ShouldInstallAbleton() then
      WizardForm.FinishedLabel.Caption :=
        'StemLab is installed.' + #13#10 + #13#10 +
        'Ableton integration is installed too. Fully restart Ableton Live, then set:' + #13#10 +
        'Settings > Link, Tempo & MIDI > Control Surface = StemLabRemote' + #13#10 +
        'Input = None, Output = None.'
    else
      WizardForm.FinishedLabel.Caption := 'StemLab is installed and ready to use.';
  end;
end;
