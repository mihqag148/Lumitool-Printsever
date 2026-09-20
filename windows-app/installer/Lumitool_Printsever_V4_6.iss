#define MyAppName "Lumitool Printsever"
#define MyAppVersion "4.6"
#define MyAppPublisher "Lumi3D"
#define MyAppExeName "Lumitool_Printsever.exe"

[Setup]
AppId={{5D9F5B7A-4A77-4C67-85F8-77CA91019103}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Lumitool Printsever
DefaultGroupName=Lumitool Printsever
DisableProgramGroupPage=yes
DirExistsWarning=no
OutputDir=..\..\dist
OutputBaseFilename=Lumitool_Printsever_Setup_V4_6
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Lumitool Printsever
UninstallDisplayIcon={app}\Lumitool_Printsever.exe
SetupIconFile=..\build\Lumi3D_logo.ico
SetupLogging=yes
CloseApplications=force
RestartApplications=no
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "..\current\Lumitool_Preinstall_Cleanup_V1.ps1"; Flags: dontcopy
Source: "..\current\Lumitool_Printsever_Setup_V4_6.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Scan_Helper_V1.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Job_Watcher_V2.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_OTA_Helper_V1.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Watcher_Install_V2.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Lumitool_Printsever.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\Lumi3D_logo.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Lumi3D_logo.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: filesandordirs; Name: "{app}\*"

[Icons]
Name: "{autoprograms}\Lumitool Printsever"; Filename: "{app}\Lumitool_Printsever.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Lumi3D_logo.ico"
Name: "{autodesktop}\Lumitool Printsever"; Filename: "{app}\Lumitool_Printsever.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Lumi3D_logo.ico"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Tạo biểu tượng Lumitool Printsever ngoài Desktop"; GroupDescription: "Tùy chọn:"; Flags: unchecked

[Run]
Filename: "powershell.exe"; Parameters: "-NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\Lumitool_Watcher_Install_V2.ps1"""; WorkingDir: "{app}"; Flags: runhidden waituntilterminated
Filename: "{app}\Lumitool_Printsever.exe"; Description: "Mở Lumitool Printsever"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""Lumitool Printsever Job Watcher"" /F"; Flags: runhidden; RunOnceId: "RemoveWatcherTask"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\Lumitool\Printsever"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  PowerShellExe: String;
  ScriptPath: String;
  Params: String;
begin
  Result := '';

  ExtractTemporaryFile('Lumitool_Preinstall_Cleanup_V1.ps1');

  PowerShellExe := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  ScriptPath := ExpandConstant('{tmp}\Lumitool_Preinstall_Cleanup_V1.ps1');
  Params := '-NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + ScriptPath + '"';

  if not Exec(PowerShellExe, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Result := 'Không chạy được bước dọn bản Lumitool cũ. Hãy đóng ứng dụng rồi chạy lại bộ cài bằng Administrator.';
    exit;
  end;

  if ResultCode <> 0 then
  begin
    Result := 'Dọn bản Lumitool cũ thất bại (exit code ' + IntToStr(ResultCode) + '). Xem %TEMP%\Lumitool_Preinstall_Cleanup.log.';
    exit;
  end;
end;
