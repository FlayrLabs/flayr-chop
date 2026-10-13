; Inno Setup script for the Windows VST3 installer.
; iscc /DAppVersion=1.2.0 /DSourceDir=<folder containing "Flayr Chop.vst3"> FlayrChop.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\FlayrChop_artefacts\Release\VST3"
#endif

[Setup]
AppId={{1F97BE8F-2E2A-4DE8-8609-796383EF1612}
AppName=Flayr Chop
AppVersion={#AppVersion}
AppPublisher=Flayr Labs
AppPublisherURL=https://github.com/FlayrLabs/flayr-chop
AppSupportURL=https://github.com/FlayrLabs/flayr-chop/issues
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=FlayrChop-{#AppVersion}-Windows-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Flayr Chop (VST3)

[Files]
Source: "{#SourceDir}\Flayr Chop.vst3\*"; DestDir: "{commoncf64}\VST3\Flayr Chop.vst3"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Flayr Chop.vst3"

[Messages]
FinishedLabel=Flayr Chop is installed in Common Files\VST3. Restart your DAW or rescan plug-ins to use it.
