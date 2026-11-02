# Builds W-Rally's MSI from a CMake build (installer/W-Rally.wxs). Needs WiX 5 with its UI extension:
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.UI.wixext/5.0.2
#
#   installer/build-msi.ps1 [-Build build] [-Config Release] [-Out build/W-Rally-<version>-<commit>.msi]
#
# The version and commit are the build's own (<build>/generated/build_info.h, cmake/BuildInfo.cmake): the MSI shows
# "W-Rally <version> · <commit>" as the game does. Under GitHub Actions the MSI's file name goes to the step's output
# "name".
param(
    [string]$Build = "build",
    [string]$Config = "Release",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Build)) { $Build = Join-Path $root $Build }

$info = Get-Content -Raw (Join-Path $Build "generateduild_info.h")
if ($info -notmatch '#define WRALLY_VERSION "([^"]*)"') { throw "no WRALLY_VERSION in build_info.h - build W-Rally first" }
$Version = $Matches[1]
$Commit = if ($info -match '#define WRALLY_COMMIT "([^"]*)"') { $Matches[1] } else { "" }
$name = if ($Commit) { "W-Rally-$Version-$Commit" } else { "W-Rally-$Version" }
if (-not $Out) { $Out = Join-Path $Build "$name.msi" }
$stage = Join-Path $Build "msi-stage"

# What ships, laid out as installed.
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install $Build --config $Config --prefix $stage
if ($LASTEXITCODE) { throw "cmake --install failed" }

$ca = Join-Path $Build "installer-ca\RbrFolder.dll"
if (-not (Test-Path $ca)) { throw "no $ca - build the wrally-installer-ca target" }

wix build (Join-Path $PSScriptRoot "W-Rally.wxs") -arch x64 -ext WixToolset.UI.wixext `
    -d "Version=$Version" -d "Commit=$Commit" -d "StageDir=$stage" -d "CaDll=$ca" -o $Out
if ($LASTEXITCODE) { throw "wix build failed" }
Write-Host "built $Out"
if ($env:GITHUB_OUTPUT) { "name=$name" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT }
