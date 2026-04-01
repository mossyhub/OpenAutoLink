param(
    [string]$OutputPath = '.\secrets\upload-key.jks',
    [string]$Alias = 'upload',
    [string]$JavaHome = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$CommonName = 'BlazeLink Upload Key',
    [string]$OrgUnit = 'Personal',
    [string]$Organization = 'Personal',
    [string]$City = 'Unknown',
    [string]$State = 'Unknown',
    [string]$CountryCode = 'US',
    [int]$ValidityDays = 9125,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$sharedScript = Join-Path $PSScriptRoot 'android\create-keystore.ps1'
$repoRoot = Split-Path -Parent $PSScriptRoot

& $sharedScript `
    -ProjectRoot $repoRoot `
    -OutputPath $OutputPath `
    -Alias $Alias `
    -JavaHome $JavaHome `
    -CommonName $CommonName `
    -OrgUnit $OrgUnit `
    -Organization $Organization `
    -City $City `
    -State $State `
    -CountryCode $CountryCode `
    -ValidityDays $ValidityDays `
    -Force:$Force
