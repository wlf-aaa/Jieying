; Inno Setup script for 截影 (Jieying). Built by tools/package.ps1, which passes:
;   /DAppVersion=x.y.z  /DStageDir=<folder with the deployed app>
; Per-user install by default (no admin rights); the user can choose "all users" instead.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir must point to the deployed application folder
#endif

[Setup]
AppId={{98CAFD72-1599-4CC3-ACD8-C780D7B68A43}
AppName=截影
AppVersion={#AppVersion}
AppVerName=截影 {#AppVersion}
AppPublisherURL=https://gitee.com/Wang_yuan_com/jieying
AppSupportURL=https://gitee.com/Wang_yuan_com/jieying/issues
DefaultDirName={autopf}\Jieying
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=Jieying-{#AppVersion}-setup
SetupIconFile=..\assets\jieying.ico
UninstallDisplayIcon={app}\jieying.exe
UninstallDisplayName=截影
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; The app holds this mutex while running; setup and uninstall ask to close it first.
AppMutex=JieyingRunning
CloseApplications=yes
VersionInfoVersion={#AppVersion}
VersionInfoDescription=截影 安装程序

[Languages]
Name: "chs"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "开机时自动启动截影"; GroupDescription: "其他："

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\截影"; Filename: "{app}\jieying.exe"
Name: "{autodesktop}\截影"; Filename: "{app}\jieying.exe"; Tasks: desktopicon

[Registry]
; Same entry the app's own "start with Windows" switch writes (Autostart::command()).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Jieying"; \
    ValueData: """{app}\jieying.exe"" --autostart"; Tasks: autostart
; Uninstall removes the autostart entry however it was created, Task Manager's record of it,
; and the app's settings.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Jieying"; \
    Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: none; \
    ValueName: "Jieying"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Jieying"; ValueType: none; Flags: uninsdeletekey

[UninstallDelete]
; Capture history and other per-user data (QStandardPaths::AppLocalDataLocation).
Type: filesandordirs; Name: "{localappdata}\Jieying"

[Run]
Filename: "{app}\jieying.exe"; Description: "{cm:LaunchProgram,截影}"; Flags: nowait postinstall skipifsilent
