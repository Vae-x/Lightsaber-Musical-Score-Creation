; Build with scripts/package-installer.ps1. All payload entries come from its
; verified manifest and explicit generated file list; never recurse a live app.
; Inno Setup documentation: https://jrsoftware.org/ishelp/
#if Ver < EncodeVer(6, 7, 3)
  #error Inno Setup 6.7.3 or newer is required
#endif
#ifndef AppVersion
  #error AppVersion must be supplied by package-installer.ps1
#endif
#ifndef StagingDir
  #error StagingDir must be supplied by package-installer.ps1
#endif
#ifndef FilesInclude
  #error FilesInclude must be supplied by package-installer.ps1
#endif
#ifndef OutputDirectory
  #error OutputDirectory must be supplied by package-installer.ps1
#endif
#ifndef IconFile
  #error IconFile must be supplied by package-installer.ps1
#endif
#ifndef LanguageFile
  #error LanguageFile must be supplied by package-installer.ps1
#endif
#ifndef CompilerVersionFile
  #error CompilerVersionFile must be supplied by package-installer.ps1
#endif
#expr SaveStringToFile(CompilerVersionFile, DecodeVer(Ver), False)

#define AppName "光剑曲谱制作"
#define AppExeName "LightsaberMusicalScoreCreation.exe"

[Setup]
; Keep this ID and install mode unchanged in later releases to recognize upgrades.
AppId={{DA9165EF-8F06-45D3-8EA0-BE0D952CA6CA}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Vae-x
AppPublisherURL=https://github.com/Vae-x/Lightsaber-Musical-Score-Creation
AppSupportURL=https://github.com/Vae-x/Lightsaber-Musical-Score-Creation/issues
AppUpdatesURL=https://github.com/Vae-x/Lightsaber-Musical-Score-Creation/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
LicenseFile={#StagingDir}\LICENSE
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
UninstallLogMode=append
AppMutex=Local\LmscLightsaberScoreRunning
SetupMutex=Local\LmscLightsaberScoreInstaller
; The application holds AppMutex. Ask the user to save and close every instance;
; silent upgrades must fail rather than automatically close an unsaved project.
CloseApplications=no
RestartApplications=no
WizardStyle=modern
OutputDir={#OutputDirectory}
OutputBaseFilename=光剑曲谱制作-Windows-v{#AppVersion}-Setup
VersionInfoVersion={#AppVersion}
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription={#AppName} 安装程序
Compression=lzma2
SolidCompression=yes
SetupLogging=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "{#LanguageFile}"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："; Flags: unchecked

[Files]
#include FilesInclude

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{group}\卸载{#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

; Deliberately no [Run], [InstallDelete], [UninstallDelete], file associations,
; or projects directory entries. Inno removes only logged installation files
; and empty created directories; user songs, projects and settings are retained.
