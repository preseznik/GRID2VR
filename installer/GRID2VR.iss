#ifndef Stage
  #error Stage is required
#endif
#ifndef PackageVersion
  #error PackageVersion is required
#endif
[Setup]
AppId={{9A37CD22-87F4-4A0C-B4EE-E8DA57F09D21}
AppName=GRID2VR
AppVersion={#PackageVersion}
AppPublisher=Bohloney
AppPublisherURL=https://github.com/preseznik/GRID2VR
DefaultDirName={code:DefaultGameDir}
AppendDefaultDirName=no
DisableProgramGroupPage=yes
DirExistsWarning=no
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallFilesDir={app}\GRID2VR\uninstall
UninstallDisplayIcon={app}\GRID2VR.exe
OutputDir={#Stage}\..
OutputBaseFilename=GRID2VR-{#PackageVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
AppMutex=Global\GRID2VR.Session
SetupLogging=yes
SetupIconFile=..\launcher\assets\GRID2VR.ico
InfoBeforeFile={#Stage}\GRID2VR\ReleaseNotes.txt

[Files]
Source: "{#Stage}\GRID2VR.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\Start-GRID2VR.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\GRID2VR\*"; DestDir: "{app}\GRID2VR"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\GRID2VR\payload\prerequisites\vc_redist.x86.exe"; Flags: dontcopy
Source: "{#Stage}\GRID2VR\payload\prerequisites\vc_redist.x64.exe"; Flags: dontcopy

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked
[Icons]
Name: "{autoprograms}\GRID2VR"; Filename: "{app}\GRID2VR.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\GRID2VR"; Filename: "{app}\GRID2VR.exe"; WorkingDir: "{app}"; Tasks: desktopicon
[Run]
Filename: "{app}\GRID2VR.exe"; Description: "Open GRID2VR"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var SuggestedGame: String;
function DefaultGameDir(Param: String): String;
begin
  if SuggestedGame <> '' then Result := SuggestedGame
  else Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\grid 2');
end;
procedure InitializeWizard();
var ResultCode, I: Integer; Discovered: AnsiString; ExplicitDir: Boolean;
begin
  WizardForm.SelectDirLabel.Caption := 'Select the existing GRID 2 folder containing grid2.exe and grid2_avx.exe. No game files or saves are included.';
  ExtractTemporaryFile('GRID2VR.exe');
  ExplicitDir := False;
  for I := 1 to ParamCount do if CompareText(Copy(ParamStr(I),1,5),'/DIR=')=0 then ExplicitDir:=True;
  if Exec(ExpandConstant('{tmp}\GRID2VR.exe'), '--discover "'+ExpandConstant('{tmp}\grid2-path.txt')+'"','',SW_HIDE,ewWaitUntilTerminated,ResultCode) and (ResultCode=0) then
    if LoadStringFromFile(ExpandConstant('{tmp}\grid2-path.txt'),Discovered) then begin
      SuggestedGame:=UTF8Decode(Discovered);
      if not ExplicitDir and (SuggestedGame<>'') then WizardForm.DirEdit.Text:=SuggestedGame;
    end;
end;
function InstallRuntime(Arch: String; var NeedsRestart: Boolean): Boolean;
var Installed, Minor: Cardinal; ResultCode: Integer;
begin
  Result:=True;
  if RegQueryDWordValue(HKLM32,'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\'+Arch,'Installed',Installed) and (Installed=1) and
     RegQueryDWordValue(HKLM32,'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\'+Arch,'Minor',Minor) and (Minor>=44) then exit;
  ExtractTemporaryFile('vc_redist.'+Arch+'.exe');
  Result:=Exec(ExpandConstant('{tmp}\vc_redist.'+Arch+'.exe'),'/install /quiet /norestart','',SW_HIDE,ewWaitUntilTerminated,ResultCode) and ((ResultCode=0) or (ResultCode=3010) or (ResultCode=1638));
  if Result and (ResultCode=3010) then NeedsRestart:=True;
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
var ResultCode: Integer; ErrorText: AnsiString;
begin
  Result:='';
  if CheckForMutexes('Global\GRID2VR.Session') then begin Result:='Finish the GRID2VR session before installing.';exit;end;
  if not Exec(ExpandConstant('{tmp}\GRID2VR.exe'),'--check-install --game "'+WizardDirValue()+'" --error-log "'+ExpandConstant('{tmp}\grid2-install-error.txt')+'"','',SW_HIDE,ewWaitUntilTerminated,ResultCode) or (ResultCode<>0) then begin
    Result:='Game-folder validation failed. No package files were installed.';
    if LoadStringFromFile(ExpandConstant('{tmp}\grid2-install-error.txt'),ErrorText) then Result:=Result+#13#10+UTF8Decode(ErrorText);
    exit;
  end;
  if not InstallRuntime('x86',NeedsRestart) or not InstallRuntime('x64',NeedsRestart) then Result:='Microsoft Visual C++ runtime installation failed. Restart Windows if requested, then retry.';
end;
function InitializeUninstall(): Boolean;
var ResultCode: Integer;
begin
  Result:=False;
  if CheckForMutexes('Global\GRID2VR.Session') then begin MsgBox('Close GRID 2 and let GRID2VR restore its files before uninstalling.',mbError,MB_OK);exit;end;
  if not Exec(ExpandConstant('{app}\GRID2VR.exe'),'--check-uninstall --game "'+ExpandConstant('{app}')+'"','',SW_HIDE,ewWaitUntilTerminated,ResultCode) or (ResultCode<>0) then begin
    MsgBox('Restore the session and close GRID2VR before uninstalling. Changed package files and recovery data were preserved.',mbError,MB_OK);exit;
  end;
  Result:=True;
end;
