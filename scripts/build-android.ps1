param(
    [ValidateSet('assembleDebug', 'assembleRelease', 'bundleRelease')]
    [string]$Task = 'assembleDebug',
    [string]$JavaHome = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$AndroidSdkRoot = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$GradleUserHome
)

$ErrorActionPreference = 'Stop'

$sharedScript = Join-Path $PSScriptRoot 'android\build-gradle.ps1'
$repoRoot = Split-Path -Parent $PSScriptRoot

& $sharedScript `
    -ProjectRoot $repoRoot `
    -Tasks $Task `
    -JavaHome $JavaHome `
    -AndroidSdkRoot $AndroidSdkRoot `
    -GradleUserHome $GradleUserHome
