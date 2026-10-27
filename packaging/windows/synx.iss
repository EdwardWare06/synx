; Inno Setup script for the graphical Windows installer.
; Built by scripts\package-win.ps1 from the staged package:
;   ISCC /DAppVersion=<version> /DStageDir=<stage> /DOutputDir=<dir> synx.iss
; Output: synx-<version>-windows-x86_64-setup.exe
;
; The installer is per user and needs no administrator rights. It installs to
; the same folder as install.cmd, so it upgrades a zip installation in place.
; The optional "addtopath" task adds bin to the user PATH and sets SYNX_HOME;
; the uninstaller removes both.

#ifndef AppVersion
  #error Define AppVersion, for example /DAppVersion=0.1.0
#endif
#ifndef StageDir
  #error Define StageDir, the staged package folder
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; The AppId identifies the installation for upgrades and uninstall. Keep it.
AppId={{30D4DB7D-B673-42A5-AFBA-782AA6D5AEA4}
AppName=Synx
AppVersion={#AppVersion}
AppVerName=Synx {#AppVersion}
AppPublisher=Edward Ware
AppPublisherURL=https://github.com/EdwardWare06/synx
AppSupportURL=https://github.com/EdwardWare06/synx/issues
AppUpdatesURL=https://github.com/EdwardWare06/synx/releases
DefaultDirName={localappdata}\Programs\Synx
DefaultGroupName=Synx
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
UninstallDisplayName=Synx {#AppVersion}
UninstallDisplayIcon={app}\synx.ico
SetupIconFile=synx.ico
OutputDir={#OutputDir}
OutputBaseFilename=synx-{#AppVersion}-windows-x86_64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "addtopath"; Description: "Add Synx to my PATH (recommended)"

[InstallDelete]
; An upgrade replaces these folders, so removed files do not stay behind.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\libexec"
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\std"
Type: filesandordirs; Name: "{app}\examples"
; Left by a zip installation; the uninstaller replaces them.
Type: files; Name: "{app}\install.cmd"
Type: files; Name: "{app}\install.ps1"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\install.cmd,\install.ps1"; Flags: recursesubdirs ignoreversion
; The Synx mark for Installed apps and the Start menu shortcuts.
Source: "synx.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Synx Command Prompt"; Filename: "{cmd}"; Parameters: "/k ""set ""PATH={app}\bin;%PATH%"" && synx version"""; WorkingDir: "{userdocs}"; IconFilename: "{app}\synx.ico"
Name: "{group}\Synx Documentation"; Filename: "https://github.com/EdwardWare06/synx/blob/main/docs/index.md"; IconFilename: "{app}\synx.ico"
Name: "{group}\Uninstall Synx"; Filename: "{uninstallexe}"

[Run]
Filename: "{cmd}"; Parameters: "/k ""set ""PATH={app}\bin;%PATH%"" && synx version"""; WorkingDir: "{userdocs}"; Description: "Open a Synx command prompt"; Flags: postinstall nowait skipifsilent

[Messages]
FinishedLabel=Setup has installed Synx on your computer.%n%nTo use Synx, open a new terminal and run: synx version

[Code]
const
  EnvironmentKey = 'Environment';

function SameDirectory(Left, Right: string): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(Trim(Left)), RemoveBackslashUnlessRoot(Trim(Right))) = 0;
end;

{ Returns the user PATH without entries equal to Dir. The value is read
  without expanding %VARIABLES%, so other entries keep their form. }
function UserPathWithout(Dir: string): string;
var
  Raw, Entry: string;
  Split: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', Raw) then
    Raw := '';
  while Raw <> '' do
  begin
    Split := Pos(';', Raw);
    if Split = 0 then
    begin
      Entry := Raw;
      Raw := '';
    end else begin
      Entry := Copy(Raw, 1, Split - 1);
      Delete(Raw, 1, Split);
    end;
    if (Entry <> '') and not SameDirectory(Entry, Dir) then
    begin
      if Result <> '' then
        Result := Result + ';';
      Result := Result + Entry;
    end;
  end;
end;

procedure WriteUserPath(Value: string);
begin
  if Value = '' then
    RegDeleteValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path')
  else
    RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', Value);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Bin, Rest: string;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
  begin
    Bin := ExpandConstant('{app}\bin');
    Rest := UserPathWithout(Bin);
    if Rest = '' then
      WriteUserPath(Bin)
    else
      WriteUserPath(Bin + ';' + Rest);
    RegWriteStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'SYNX_HOME', ExpandConstant('{app}'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Home: string;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    WriteUserPath(UserPathWithout(ExpandConstant('{app}\bin')));
    if RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'SYNX_HOME', Home) and
       SameDirectory(Home, ExpandConstant('{app}')) then
      RegDeleteValue(HKEY_CURRENT_USER, EnvironmentKey, 'SYNX_HOME');
  end;
end;
