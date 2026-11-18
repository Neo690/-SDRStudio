; SDR Studio - installateur Windows (Inno Setup 6.6+).
;
; Compile par build-installer.bat, qui construit d'abord l'application et
; regenere les visuels (icone, images de l'assistant).
;
; Installation par utilisateur : aucun droit administrateur demande, fichier
; dans %LOCALAPPDATA%\Programs\SDR Studio, raccourci Bureau et menu Demarrer.

#define AppName "SDR Studio"
#define AppVersion "1.0.0"
#define AppExe "SDRStudio.exe"
#define AppUrl "https://github.com/Neo690/-SDRStudio"

[Setup]
; Identite stable : c'est elle qui relie les mises a jour et la desinstallation.
AppId={{7F3A1C62-5B4E-4E2B-9A7D-2C9F0E51A8B4}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Neo690
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
AppUpdatesURL={#AppUrl}
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Neo690
VersionInfoDescription={#AppName} {#AppVersion} - console RTL-SDR native

DefaultDirName={autopf}\SDR Studio
; Page d'accueil toujours affichee : c'est elle qui presente l'application et
; le visuel genere par make-assets.ps1.
DisableWelcomePage=no
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Les choix de l'assistant repartent des valeurs du script, pas de ceux d'une
; installation precedente.
UsePreviousTasks=no
AllowNoIcons=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\icon.ico

OutputDir=dist
OutputBaseFilename=SDRStudio-{#AppVersion}-setup
SetupIconFile=assets\icon.ico
Compression=lzma2/max
SolidCompression=yes

; Apparence : assistant moderne en sombre, aux couleurs de l'application.
WizardStyle=modern dark includetitlebar
WizardImageFile=assets\setup-big.bmp
WizardSmallImageFile=assets\setup-small.bmp
WizardImageStretch=yes
WizardSizePercent=110

; Dossier utilisateur : pas d'UAC, pas de questions de privileges.
PrivilegesRequired=lowest

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Une seule langue : l'assistant demarre directement, sans boite de choix.
[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
; Cocher par defaut : le raccourci Bureau fait partie de l'installation.
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\sdr_selftest.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; librtlsdr.dll / libusb-1.0.dll : chargees par l'exe depuis son propre dossier.
Source: "..\vendor\rtlsdr\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "assets\icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\vendor\tools\zadig-2.9.exe"; DestDir: "{app}\tools"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\vendor\tools\zadig.ini"; DestDir: "{app}\tools"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\install-driver.bat"; DestDir: "{app}\tools"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\icon.ico"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon
; Le pilote WinUSB s'installe a la demande : rien ne se lance tout seul
; apres l'installation (Zadig demande des droits administrateur).
Name: "{group}\Pilote WinUSB (Zadig)"; Filename: "{app}\tools\zadig-2.9.exe"; IconFilename: "{app}\icon.ico"; Comment: "Associer la cle RTL-SDR au pilote WinUSB"

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
procedure InitializeWizard;
begin
  WizardForm.WelcomeLabel2.Caption :=
    'SDR Studio installe une console de reception RTL-SDR native : le spectre, ' +
    'le waterfall, six demodulations et l''enregistrement sont calcules en C++ ' +
    'sur le CPU, sans Python ni Electron.' + #13#10 + #13#10 +
    'Installation pour l''utilisateur courant : aucun droit administrateur ' +
    'n''est demande. Un raccourci peut etre place sur le Bureau et dans le ' +
    'menu Demarrer.';
end;
