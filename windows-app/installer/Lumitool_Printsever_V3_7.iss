#define MyAppName "Lumitool Printsever"
#define MyAppVersion "3.7"
#define MyAppPublisher "Lumitool"
#define MyAppExeName "Lumitool_Printsever_Launcher.cmd"

[Setup]
AppId={{5D9F5B7A-4A77-4C67-85F8-77CA91019103}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Lumitool Printsever
DefaultGroupName=Lumitool Printsever
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=Lumitool_Printsever_Setup_V3_7
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Lumitool Printsever
SetupLogging=yes
CloseApplications=no
RestartApplications=no

[Files]
Source: "..\current\Lumitool_Printsever_Setup_V3_7.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Scan_Helper_V1.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Job_Watcher_V1.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Fix_Printing_Status_V2.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Printsever_Launcher.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\current\Lumitool_Printsever_Repair.cmd"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Lumitool Printsever"; Filename: "{app}\Lumitool_Printsever_Launcher.cmd"; WorkingDir: "{app}"
Name: "{autoprograms}\Lumitool Printsever\Repair / Sync Print Jobs"; Filename: "{app}\Lumitool_Printsever_Repair.cmd"; WorkingDir: "{app}"
Name: "{autodesktop}\Lumitool Printsever"; Filename: "{app}\Lumitool_Printsever_Launcher.cmd"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Tạo biểu tượng Lumitool Printsever ngoài Desktop"; GroupDescription: "Tùy chọn:"; Flags: unchecked

[Run]
Filename: "{app}\Lumitool_Printsever_Launcher.cmd"; Description: "Mở Lumitool Printsever để cài máy in"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""Lumitool Printsever Job Watcher"" /F"; Flags: runhidden; RunOnceId: "RemoveWatcherTask"
