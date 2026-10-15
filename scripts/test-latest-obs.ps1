# SPDX-License-Identifier: GPL-2.0-or-later
# Test the minimum-SDK binary against the latest official stable Windows runtime.
[CmdletBinding()]
param([string] $Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Release = Invoke-RestMethod 'https://api.github.com/repos/obsproject/obs-studio/releases/latest'
$Version = $Release.tag_name
$Asset = @($Release.assets | Where-Object { $_.name -eq "OBS-Studio-$Version-Windows-x64.zip" })
if ($Asset.Count -ne 1) { throw "No unique Windows x64 ZIP for OBS $Version." }
$Destination = Join-Path $ProjectRoot ".deps/runtime-$Version"
$Archive = Join-Path $ProjectRoot ".deps/runtime-$Version.zip"
Invoke-WebRequest $Asset[0].browser_download_url -OutFile $Archive
if ($Asset[0].PSObject.Properties.Name -contains 'digest' -and $Asset[0].digest) {
    $Digest = "sha256:$((Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant())"
    if ($Digest -ne $Asset[0].digest) { throw 'Official runtime checksum mismatch.' }
}
Expand-Archive $Archive -DestinationPath $Destination -Force
$Runtime = Join-Path $Destination 'bin/64bit'
$Executable = Join-Path $ProjectRoot "build-windows/$Configuration/smoke-obs.exe"
$Plugin = Join-Path $ProjectRoot "build-windows/$Configuration/obs-upscaler.dll"
$OriginalPath = $env:PATH
try {
    $env:PATH = "$Runtime;$OriginalPath"
    Push-Location $Runtime
    try {
        & $Executable $Plugin (Join-Path $ProjectRoot 'data') (Join-Path $Runtime 'libobs-d3d11.dll') $Version
        if ($LASTEXITCODE -ne 0) { throw "Latest OBS $Version smoke test failed: $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { $env:PATH = $OriginalPath }
