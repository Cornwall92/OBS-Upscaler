# SPDX-License-Identifier: GPL-2.0-or-later
# Intended for the disposable Windows CI runner, not a user's OBS installation.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Setup = @(Get-ChildItem (Join-Path $ProjectRoot 'build-windows/packages/*-setup.exe'))
if ($Setup.Count -ne 1) { throw 'Expected exactly one installer.' }
$Stage = Join-Path $ProjectRoot 'build-windows/installer-stage/obs-upscaler'
$Install = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'obs-studio/plugins/obs-upscaler'
if (Test-Path $Install) { throw "Refusing to test over an existing installation: $Install" }
$Payload = @('bin/64bit/obs-upscaler.dll', 'data/upscale.effect', 'data/locale/en-US.ini', 'README.md', 'LICENSE')
$Sentinel = Join-Path $Install 'keep-user-file.txt'
for ($Pass = 1; $Pass -le 2; $Pass++) {
    $Process = Start-Process $Setup[0].FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS' -PassThru -Wait
    if ($Process.ExitCode -ne 0) { throw "Installer pass $Pass failed: $($Process.ExitCode)" }
    foreach ($File in $Payload) {
        $InstalledHash = (Get-FileHash (Join-Path $Install $File) -Algorithm SHA256).Hash
        $StagedHash = (Get-FileHash (Join-Path $Stage $File) -Algorithm SHA256).Hash
        if ($InstalledHash -ne $StagedHash) { throw "Installed payload differs: $File" }
    }
    if ($Pass -eq 1) {
        Set-Content $Sentinel 'Must survive update and uninstall.'
        # Simulate an old payload to prove an in-place reinstall replaces files.
        Set-Content (Join-Path $Install 'bin/64bit/obs-upscaler.dll') 'old test payload'
    }
}
if (-not (Test-Path $Sentinel)) { throw 'Update removed an unrelated file.' }
$Uninstallers = @(Get-ChildItem (Join-Path $Install 'unins*.exe'))
if ($Uninstallers.Count -ne 1) { throw 'Expected one uninstall entry after reinstall.' }
$Process = Start-Process $Uninstallers[0].FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru -Wait
if ($Process.ExitCode -ne 0) { throw "Uninstaller failed: $($Process.ExitCode)" }
foreach ($File in $Payload) {
    if (Test-Path (Join-Path $Install $File)) { throw "Uninstaller left payload: $File" }
}
if (-not (Test-Path $Sentinel)) { throw 'Uninstaller removed an unrelated file.' }
Remove-Item $Sentinel
Write-Host 'PASS: default install location, payload hashes, in-place reinstall, uninstall and unrelated-file preservation.'
