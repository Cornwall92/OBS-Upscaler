# SPDX-License-Identifier: GPL-2.0-or-later
# Builds an x64 plugin and a ZIP for C:\ProgramData\obs-studio\plugins.
# Requires Visual Studio 2022 C++ build tools, a Windows 10/11 SDK, CMake 3.28+.
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $BuildDirectory = 'build-windows',
    [switch] $SkipPackage,
    [switch] $BuildInstaller
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') {
    throw 'This script builds Windows x64 binaries. Use CMake directly on Linux.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Run this script from 64-bit PowerShell.'
}

function Invoke-Checked {
    param([string] $Command, [string[]] $Arguments)
    $global:LASTEXITCODE = 0
    & $Command @Arguments
    $Succeeded = $?
    if (-not $Succeeded -or $LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE."
    }
}

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $ProjectRoot $BuildDirectory
}
if ($BuildInstaller -and $SkipPackage) {
    throw '-BuildInstaller cannot be combined with -SkipPackage.'
}
$InstallerCompiler = $null
if ($BuildInstaller) {
    $CompilerCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    $InstallerCompiler = if ($CompilerCommand) { $CompilerCommand.Source } else {
        Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
    }
    if (-not (Test-Path $InstallerCompiler)) {
        throw 'Install Inno Setup 6 (6.3 or newer), or add ISCC.exe to PATH, to use -BuildInstaller.'
    }
}
$DependenciesPath = Join-Path $ProjectRoot '.deps'
$SdkSpec = Get-Content (Join-Path $ProjectRoot 'cmake/obs-sdk.json') -Raw | ConvertFrom-Json
$ArchivePath = Join-Path $DependenciesPath "obs-studio-$($SdkSpec.version).zip"
$ObsSourcePath = Join-Path $DependenciesPath "obs-studio-$($SdkSpec.version)"
$ObsBuildPath = Join-Path $DependenciesPath "obs-build-$($SdkSpec.version)-x64"
$SdkPath = Join-Path $DependenciesPath "obs-sdk-$($SdkSpec.version)-x64"

Get-Command cmake -ErrorAction Stop | Out-Null
Get-Command ctest -ErrorAction Stop | Out-Null
if (-not $SkipPackage) {
    Get-Command cpack -ErrorAction Stop | Out-Null
}
$global:LASTEXITCODE = 0
# Capture the complete native output before selecting a line. Stopping a native
# pipeline early can leave LASTEXITCODE unset under PowerShell StrictMode.
$CMakeVersionOutput = & cmake --version
$CMakeVersionLine = $CMakeVersionOutput | Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or $CMakeVersionLine -notmatch '([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Unable to determine the CMake version.'
}
if ([version] $Matches[1] -lt [version] '3.28.0') {
    throw 'The pinned OBS SDK requires CMake 3.28 or newer.'
}

New-Item -ItemType Directory -Path $DependenciesPath -Force | Out-Null
if (-not (Test-Path $ArchivePath)) {
    Write-Host "Downloading the official OBS $($SdkSpec.version) source archive..."
    Invoke-WebRequest -Uri $SdkSpec.url -OutFile $ArchivePath
}
$ArchiveHash = (Get-FileHash -Algorithm SHA256 -Path $ArchivePath).Hash.ToLowerInvariant()
if ($ArchiveHash -ne $SdkSpec.sha256) {
    throw "OBS archive checksum mismatch. Remove '$ArchivePath' and retry. No unverified source will be built."
}
if (-not (Test-Path (Join-Path $ObsSourcePath 'CMakeLists.txt'))) {
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $DependenciesPath -Force
}

# OBS's own pinned buildspec downloads and verifies its dependency archives.
# Its SDK bootstrap also downloads Qt build files, but this plugin does not link
# Qt or obs-frontend-api and requires no additional vendor GPU runtime.
Invoke-Checked cmake @(
    '-S', $ObsSourcePath, '-B', $ObsBuildPath,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DENABLE_PLUGINS=OFF', '-DENABLE_FRONTEND=OFF', '-DENABLE_BROWSER=OFF',
    '-DENABLE_SCRIPTING=OFF', "-DOBS_VERSION_OVERRIDE=$($SdkSpec.version)"
)
# The SDK's Development install component includes this lightweight API library
# even with the UI disabled; building it also builds libobs and w32-pthreads.
Invoke-Checked cmake @('--build', $ObsBuildPath, '--target', 'obs-frontend-api', 'libobs-d3d11', '--config', $Configuration, '--parallel')
Invoke-Checked cmake @('--install', $ObsBuildPath, '--component', 'Development', '--config', $Configuration, '--prefix', $SdkPath)

$ObsBuildSpec = Get-Content (Join-Path $ObsSourcePath 'buildspec.json') -Raw | ConvertFrom-Json
$PrebuiltPath = Join-Path $ObsSourcePath ".deps/obs-deps-$($ObsBuildSpec.dependencies.prebuilt.version)-x64"
$PrefixPath = "$SdkPath;$PrebuiltPath"
Invoke-Checked cmake @(
    '-S', $ProjectRoot, '-B', $BuildPath,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$PrefixPath", '-DBUILD_TESTING=ON', '-DBUILD_OBS_SMOKE_TEST=ON', '-DENABLE_STRICT_WARNINGS=ON'
)
Invoke-Checked cmake @('--build', $BuildPath, '--config', $Configuration, '--parallel')
Invoke-Checked ctest @('--test-dir', $BuildPath, '-C', $Configuration, '-R', '^upscale-policy$', '--output-on-failure')

# OBS's Windows resource lookup is relative to the current directory. Use the
# runtime tree produced by the SDK build, which includes ../../data/libobs.
# Dependency DLLs stay outside the plugin ZIP; the installed OBS supplies them.
$RuntimePath = Join-Path $ObsBuildPath "rundir/$Configuration/bin/64bit"
$SdkBinaryPath = Join-Path $SdkPath 'bin/64bit'
$PrebuiltBinaryPath = Join-Path $PrebuiltPath 'bin'
$SmokeExecutable = Join-Path $BuildPath "$Configuration/smoke-obs.exe"
$PluginBinary = Join-Path $BuildPath "$Configuration/obs-upscaler.dll"
$GraphicsModule = Join-Path $RuntimePath 'libobs-d3d11.dll'
$PluginData = Join-Path $ProjectRoot 'data'
@(
    $SmokeExecutable, $PluginBinary, $GraphicsModule, $PrebuiltBinaryPath,
    (Join-Path $SdkBinaryPath 'obs.dll'),
    (Join-Path $SdkBinaryPath 'w32-pthreads.dll'),
    (Join-Path $ObsBuildPath "rundir/$Configuration/data/libobs/default.effect")
) | ForEach-Object {
    if (-not (Test-Path -LiteralPath $_)) {
        throw "The D3D11 smoke test needs a missing runtime file or directory: $_"
    }
}
$OriginalPath = $env:PATH
try {
    $env:PATH = "$RuntimePath;$SdkBinaryPath;$PrebuiltBinaryPath;$OriginalPath"
    Push-Location -LiteralPath $RuntimePath
    try {
        Invoke-Checked $SmokeExecutable @($PluginBinary, $PluginData, $GraphicsModule)
    } finally {
        Pop-Location
    }
} finally {
    $env:PATH = $OriginalPath
}

if (-not $SkipPackage) {
    Invoke-Checked cpack @('--config', (Join-Path $BuildPath 'CPackConfig.cmake'), '-C', $Configuration, '-B', (Join-Path $BuildPath 'packages'))
    Write-Host "Package ready in $BuildPath\packages."
}

if ($BuildInstaller) {
    $StagePath = Join-Path $BuildPath 'installer-stage'
    Invoke-Checked cmake @('--install', $BuildPath, '--config', $Configuration, '--prefix', $StagePath)
    $CMakeCache = Get-Content (Join-Path $BuildPath 'CMakeCache.txt') -Raw
    if ($CMakeCache -notmatch '(?m)^CMAKE_PROJECT_VERSION:STATIC=([0-9]+\.[0-9]+\.[0-9]+)\r?$') {
        throw 'Cannot determine the plugin version from CMakeCache.txt.'
    }
    $PluginVersion = $Matches[1]
    Invoke-Checked $InstallerCompiler @(
        "/DPluginVersion=$PluginVersion", "/DStageDirectory=$StagePath",
        "/DPackageDirectory=$(Join-Path $BuildPath 'packages')",
        (Join-Path $ProjectRoot 'installer/obs-upscaler.iss')
    )
}
