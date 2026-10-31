# Builds W-Rally's MSI from a CMake build (installer/W-Rally.wxs). Needs WiX 5 with its UI extension:
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.UI.wixext/5.0.2
#
#   installer/build-msi.ps1 [-Build build] [-Config Release] [-Version 0.1.0] [-Out build/W-Rally-<version>.msi]
param(
    [string]$Build = "build",
    [string]$Config = "Release",
    [string]$Version = "0.1.0",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Build)) { $Build = Join-Path $root $Build }
if (-not $Out) { $Out = Join-Path $Build "W-Rally-$Version.msi" }
$stage = Join-Path $Build "msi-stage"

# What ships, laid out as installed.
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install $Build --config $Config --prefix $stage
if ($LASTEXITCODE) { throw "cmake --install failed" }

$ca = Join-Path $Build "installer-ca\RbrFolder.dll"
if (-not (Test-Path $ca)) { throw "no $ca - build the wrally-installer-ca target" }

wix build (Join-Path $PSScriptRoot "W-Rally.wxs") -arch x64 -ext WixToolset.UI.wixext `
    -d "Version=$Version" -d "StageDir=$stage" -d "CaDll=$ca" -o $Out
if ($LASTEXITCODE) { throw "wix build failed" }
Write-Host "built $Out"
