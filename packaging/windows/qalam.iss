; Inno Setup script for the qalam Windows installer.
;
; Built by the release workflow; to build it yourself (Inno Setup 6.3+):
;
;   tools/deps.sh && make VERSION=0.1.0 LDFLAGS=-static     (in MSYS2 UCRT64)
;   packaging/package.sh windows-x86_64
;   iscc /DAppVersion=0.1.0 packaging\windows\qalam.iss
;
; Output: dist\qalam-windows-setup.exe
;
; The installer puts qalam.exe in <install dir>\bin (and the fonts in
; <install dir>\fonts, where qalam finds them) and adds bin to PATH, so `qalam` works in any NEW terminal. By default it installs for the
; current user only (no admin prompt, PATH is the user's PATH); the first
; page offers "install for all users" instead (needs admin, system PATH).
; Uninstalling removes the folder from PATH again.

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\qalam-windows-x86_64"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#ifndef Repo
  #define Repo "Husseinabdulameer11/qalam"
#endif
#define AppPublisher "Husseinabdulameer11"

[Setup]
; AppId identifies qalam to Windows across versions: never change it, or an
; update installs side by side with the old version instead of replacing it.
AppId={{D1B7069B-CB8B-46FB-8F0F-AF47C0345E39}
AppName=qalam
AppVersion={#AppVersion}
AppVerName=qalam {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL=https://github.com/{#Repo}
AppSupportURL=https://github.com/{#Repo}/issues
AppUpdatesURL=https://github.com/{#Repo}/releases
DefaultDirName={autopf}\qalam
DefaultGroupName=qalam
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
OutputDir={#OutputDir}
OutputBaseFilename=qalam-windows-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\qalam.exe
UninstallDisplayName=qalam {#AppVersion}
VersionInfoProductTextVersion={#AppVersion}

[Tasks]
Name: "addtopath"; Description: "Add qalam to PATH, so the qalam command works in any terminal (recommended)"

[Files]
; qalam.exe finds its fonts at ..\fonts, so keep bin and fonts side by side.
Source: "{#SourceDir}\bin\qalam.exe"; DestDir: "{app}\bin";   Flags: ignoreversion
Source: "{#SourceDir}\fonts\*";       DestDir: "{app}\fonts"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\README.md";  DestDir: "{app}";     Flags: ignoreversion
Source: "{#SourceDir}\LICENSE";    DestDir: "{app}";     Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs

[Icons]
; A terminal with qalam ready, starting in Documents (writable, unlike the
; install folder, so `qalam -o doc.pdf ...` can save there).
Name: "{group}\qalam command prompt"; Filename: "{cmd}"; \
    Parameters: "/k set ""PATH={app}\bin;%PATH%"" && qalam --help"; WorkingDir: "{userdocs}"
Name: "{group}\qalam examples"; Filename: "{app}\examples"
Name: "{group}\Uninstall qalam"; Filename: "{uninstallexe}"

[Messages]
; (the finished page's text is set in CurPageChanged, which can name the install folder)
FinishedLabel=qalam is installed.

[Code]
const
  SystemEnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  UserEnvKey   = 'Environment';

{ All-users install -> system PATH; current-user install -> user PATH. }
function EnvRoot: Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE
  else Result := HKEY_CURRENT_USER;
end;

function EnvKey: String;
begin
  if IsAdminInstallMode then Result := SystemEnvKey
  else Result := UserEnvKey;
end;

{ Position of Dir in ';' + Paths + ';' (case-insensitive), 0 if absent.
  A match at P means Dir starts at index P of Paths itself. }
function FindInPath(Paths, Dir: String): Integer;
begin
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';');
end;

procedure AddToPath(Dir: String);
var
  Paths: String;
begin
  if not RegQueryStringValue(EnvRoot, EnvKey, 'Path', Paths) then
    Paths := '';
  if FindInPath(Paths, Dir) > 0 then
    exit;
  if (Paths <> '') and (Copy(Paths, Length(Paths), 1) <> ';') then
    Paths := Paths + ';';
  Paths := Paths + Dir;
  if not RegWriteExpandStringValue(EnvRoot, EnvKey, 'Path', Paths) then
    SuppressibleMsgBox('Could not add ' + Dir + ' to PATH. You can add it by ' +
                       'hand in "Edit environment variables".', mbError, MB_OK, IDOK);
end;

procedure RemoveFromPath(Dir: String);
var
  Paths: String;
  P: Integer;
begin
  if not RegQueryStringValue(EnvRoot, EnvKey, 'Path', Paths) then
    exit;
  P := FindInPath(Paths, Dir);
  if P = 0 then
    exit;
  if P > 1 then
    Delete(Paths, P - 1, Length(Dir) + 1)   { the ';' before it, and Dir }
  else
    Delete(Paths, 1, Length(Dir) + 1);      { Dir, and the ';' after it }
  RegWriteExpandStringValue(EnvRoot, EnvKey, 'Path', Paths);
end;

{ The last page: how to run qalam, with the real path of the examples. }
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      'qalam is installed.' + #13#10 + #13#10 +
      'Open a NEW terminal (PowerShell or cmd; ones that were already open ' +
      'do not see the new PATH) and try an example:' + #13#10 + #13#10 +
      '    qalam -o coffee.pdf "' + ExpandConstant('{app}') +
      '\examples\coffee\coffee.qlm"' + #13#10 + #13#10 +
      'then open coffee.pdf. The Start menu has a qalam ' +
      'command prompt and the examples folder (each example has its PDF next to it).';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddToPath(ExpandConstant('{app}\bin'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}\bin'));
end;
