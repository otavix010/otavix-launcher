; Otavix Launcher's installer (Inno Setup 6). Built by scripts\release.cmd from out\pacote.
; Installed for the user alone, under %LOCALAPPDATA%\Programs: the launcher writes its ports, settings and saves
; beside itself, so it needs a folder of the user's own and no administrator.

#ifndef Versao
  #define Versao "1.0.0"
#endif

[Setup]
AppId={{6F2C1D7E-4A1B-4C59-9E3A-0D5B7C2F8A41}
AppName=Otavix Launcher
AppVersion={#Versao}
AppPublisher=Otavix
DefaultDirName={localappdata}\Programs\Otavix Launcher
DefaultGroupName=Otavix Launcher
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputDir=..\out
OutputBaseFilename=Instalar-Otavix-Launcher-{#Versao}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Otavix Launcher.exe
LicenseFile=..\LICENSE
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "pt"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\out\pacote\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Otavix Launcher"; Filename: "{app}\Otavix Launcher.exe"
Name: "{autodesktop}\Otavix Launcher"; Filename: "{app}\Otavix Launcher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Otavix Launcher.exe"; Description: "{cm:LaunchProgram,Otavix Launcher}"; Flags: nowait postinstall skipifsilent

; the ports, the games, the saves and the settings stay unless the player removes the folder
[UninstallDelete]
Type: filesandordirs; Name: "{app}\cache"
