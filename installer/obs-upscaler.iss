; SPDX-License-Identifier: GPL-2.0-or-later
; Inputs are supplied by scripts/build-windows.ps1 after CMake stages the package.
#ifndef PluginVersion
  #error PluginVersion is required
#endif
#ifndef StageDirectory
  #error StageDirectory is required
#endif
#ifndef PackageDirectory
  #error PackageDirectory is required
#endif

[Setup]
AppId={{923EBD57-A16A-4A8E-B175-2CF6B0D0D36F}
AppName=OBS Upscaler
AppVersion={#PluginVersion}
AppPublisher=James C
AppPublisherURL=https://github.com/Cornwall92/OBS-Upscaler
DefaultDirName={commonappdata}\obs-studio\plugins\obs-upscaler
DisableWelcomePage=no
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
OutputDir={#PackageDirectory}
OutputBaseFilename=obs-upscaler-{#PluginVersion}-windows-x64-setup
LicenseFile={#StageDirectory}\obs-upscaler\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS Upscaler
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Files]
Source: "{#StageDirectory}\obs-upscaler\bin\64bit\obs-upscaler.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StageDirectory}\obs-upscaler\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDirectory}\obs-upscaler\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDirectory}\obs-upscaler\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
WelcomeLabel2=This installs OBS Upscaler for all users in OBS Studio's standard plugin folder.%n%nClose OBS before continuing. OBS Studio must be installed separately. For portable OBS, use the ZIP package instead.
FinishedLabel=OBS Upscaler is installed. Restart OBS, open your source's Filters, and add OBS Upscaler under Effect Filters.
