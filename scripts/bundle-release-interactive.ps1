param(
    [string]$KeystorePath = '.\secrets\upload-key.jks',
    [string]$Alias = 'upload'
)

$ErrorActionPreference = 'Stop'

$sharedScript = Join-Path $PSScriptRoot 'android\build-bundle-interactive.ps1'
$repoRoot = Split-Path -Parent $PSScriptRoot

& $sharedScript `
    -ProjectRoot $repoRoot `
    -KeystorePath $KeystorePath `
    -Alias $Alias
