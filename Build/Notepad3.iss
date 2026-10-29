; Inno Setup script for Notepad3 with the optional Markdown preview add-on.
;
; Build one architecture at a time, because the payload comes from the matching
; output directory:
;   ISCC.exe /DAppArch=x64 /DAppVersion=7.26.1028.1 /O..\out Notepad3.iss
;
; Build\scripts\PackageNotepad3.ps1 does this for you and also writes the
; portable ZIP. Settings files (Notepad3.ini, minipath.ini) are intentionally
; left out: an installed Notepad3 keeps its settings under %APPDATA% instead of
; writing next to the executable in Program Files.

#ifndef AppArch
  #define AppArch "x64"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0.0"
#endif

#define AppName "Notepad3"
#define AppPublisher "Rizonesoft"
#define AppURL "https://rizonesoft.com"
#define SourceBin "..\Bin\Release_" + AppArch + "_v145"

; Inno Setup cannot wildcard directory names, so the packager hands over a clean
; lng tree (no intermediate objects, no built-in English) via /DSourceLng.
#ifdef SourceLng
  #define LngDir SourceLng
#else
  #define LngDir SourceBin + "\lng"
#endif

#if AppArch == "x64"
  #define ArchAllowed "x64compatible"
#else
  #define ArchAllowed "x86compatible"
#endif

[Setup]
; Keep this AppId stable: it is what upgrades and uninstall entries key on.
AppId={{6F1E2D84-9C3B-4B7A-8E5D-2A4F7C1B9E30}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Notepad3 Setup
DefaultDirName={autopf}\Notepad3
DefaultGroupName=Notepad3
DisableProgramGroupPage=yes
LicenseFile=Docs\License.txt
OutputDir=..\out
OutputBaseFilename=Notepad3_{#AppVersion}_{#AppArch}_Setup
SetupIconFile=..\res\Notepad3.ico
UninstallDisplayIcon={app}\Notepad3.exe
UninstallDisplayName={#AppName} {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed={#ArchAllowed}
#if AppArch == "x64"
ArchitecturesInstallIn64BitMode=x64compatible
#endif
PrivilegesRequired=admin
; Without administrator rights the wizard offers a per-user install instead.
PrivilegesRequiredOverridesAllowed=commandline dialog

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "{#SourceBin}\Notepad3.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceBin}\minipath.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceBin}\np3encrypt.exe"; DestDir: "{app}"; Flags: ignoreversion
; Markdown preview add-on: the editor loads this DLL only when it is present.
Source: "{#SourceBin}\MarkdownPreview.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceBin}\MarkdownPreview\*"; DestDir: "{app}\MarkdownPreview"; Flags: ignoreversion recursesubdirs createallsubdirs
; Interface translations; English is compiled into the executable.
Source: "{#LngDir}\*"; DestDir: "{app}\lng"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Themes\*.ini"; DestDir: "{app}\Themes"; Flags: ignoreversion
Source: "Docs\*"; DestDir: "{app}\Docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Changes.txt"; DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "..\src\MarkdownPreview\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}\Docs"; Flags: ignoreversion
Source: "..\grepWin\portables\grepWin-{#AppArch}_portable.exe"; DestDir: "{app}\grepWin"; Flags: ignoreversion
Source: "..\grepWin\portables\LICENSE.txt"; DestDir: "{app}\grepWin"; Flags: ignoreversion
Source: "..\grepWin\translations\*.lang"; DestDir: "{app}\grepWin"; Flags: ignoreversion

[Dirs]
; MiniPath's favorites folder stays empty until the user adds shortcuts to it.
Name: "{app}\Favorites"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\Notepad3.exe"
Name: "{group}\MiniPath"; Filename: "{app}\minipath.exe"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Notepad3.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Notepad3.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
