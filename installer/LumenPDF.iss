; LumenPDF 安装脚本（Inno Setup 6）。由 scripts\package.ps1 调用：
;   ISCC.exe /DAppVersion=0.4.0 /DSourceDir=<staging> /DOutputDir=<dist> installer\LumenPDF.iss
; 按当前用户安装（无需管理员），安装到 %LOCALAPPDATA%\Programs\LumenPDF。
#ifndef AppVersion
  #define AppVersion "0.4.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\package\LumenPDF"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\dist"
#endif

[Setup]
AppId={{6F1C2A7E-5B8D-4E1A-9C3F-4C756D656E50}
AppName=LumenPDF
AppVersion={#AppVersion}
AppVerName=LumenPDF {#AppVersion}
AppPublisher=LumenPDF
AppPublisherURL=https://github.com/jimmgreen/LumenPDF
AppSupportURL=https://github.com/jimmgreen/LumenPDF/issues
AppUpdatesURL=https://github.com/jimmgreen/LumenPDF/releases
DefaultDirName={localappdata}\Programs\LumenPDF
DefaultGroupName=LumenPDF
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=LumenPDF-{#AppVersion}-setup
SetupIconFile=..\app\app.ico
UninstallDisplayIcon={app}\LumenPDF.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
#ifexist AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl"
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "登记为 PDF 打开程序（首次启动时可一键设为默认）"; GroupDescription: "系统集成:"
Name: "mergemenu"; Description: "在资源管理器右键菜单中添加“使用 LumenPDF 合并”"; GroupDescription: "系统集成:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\LumenPDF"; Filename: "{app}\LumenPDF.exe"
Name: "{autodesktop}\LumenPDF"; Filename: "{app}\LumenPDF.exe"; Tasks: desktopicon

[Registry]
; 与程序内“设为默认 PDF 阅读器”写入的键一致（当前用户）。Windows 不允许静默改默认应用，最终需用户在系统设置确认。
Root: HKCU; Subkey: "Software\Classes\LumenPDF.Document"; ValueType: string; ValueData: "PDF 文档 (LumenPDF)"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\LumenPDF.Document"; ValueType: string; ValueName: "FriendlyTypeName"; ValueData: "PDF 文档"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\LumenPDF.Document\DefaultIcon"; ValueType: string; ValueData: """{app}\LumenPDF.exe"",-3"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\LumenPDF.Document\shell\open\command"; ValueType: string; ValueData: """{app}\LumenPDF.exe"" ""%1"""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "LumenPDF.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LumenPDF.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "LumenPDF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LumenPDF.exe\shell\open\command"; ValueType: string; ValueData: """{app}\LumenPDF.exe"" ""%1"""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LumenPDF.exe\SupportedTypes"; ValueType: string; ValueName: ".pdf"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\LumenPDF\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "LumenPDF"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\LumenPDF\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "本地 PDF 阅读、批注与页面整理"; Tasks: assoc
Root: HKCU; Subkey: "Software\LumenPDF\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pdf"; ValueData: "LumenPDF.Document"; Tasks: assoc
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "LumenPDF"; ValueData: "Software\LumenPDF\Capabilities"; Flags: uninsdeletevalue; Tasks: assoc

[Run]
; 右键菜单由程序自身写入（与“更多 → 系统”中的开关使用同一份注册代码）。
Filename: "{app}\LumenPDF.exe"; Parameters: "--register-shell"; Flags: runhidden waituntilterminated; Tasks: assoc and mergemenu
Filename: "{app}\LumenPDF.exe"; Parameters: "--register-shell --no-merge-menu"; Flags: runhidden waituntilterminated; Tasks: assoc and not mergemenu
Filename: "{app}\LumenPDF.exe"; Parameters: "--register-merge-menu"; Flags: runhidden waituntilterminated; Tasks: mergemenu and not assoc
Filename: "{app}\LumenPDF.exe"; Description: "{cm:LaunchProgram,LumenPDF}"; Flags: nowait postinstall skipifsilent
; 程序内在线升级以 /SILENT /LPDFRELAUNCH=1 运行安装程序：安装完成后重新打开 LumenPDF。
Filename: "{app}\LumenPDF.exe"; Flags: nowait; Check: RelaunchAfterUpdate

[UninstallRun]
; 删除程序写入的全部外壳注册（右键菜单、打开方式、默认应用候选），包括安装后在程序内开启的项目。
Filename: "{app}\LumenPDF.exe"; Parameters: "--unregister-shell"; Flags: runhidden waituntilterminated; RunOnceId: "LumenPDFShell"

[UninstallDelete]
; 只删除程序目录；%LOCALAPPDATA%\LumenPDF 下的最近记录、阅读位置、设置和日志属于用户数据，保留。
Type: filesandordirs; Name: "{app}"

[Code]
function RelaunchAfterUpdate(): Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:LPDFRELAUNCH|0}') = '1');
end;
