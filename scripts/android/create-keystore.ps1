param(
    [string]$ProjectRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$OutputPath = '.\secrets\upload-key.jks',
    [string]$Alias = 'upload',
    [string]$JavaHome = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$CommonName = 'Android Upload Key',
    [string]$OrgUnit = 'Personal',
    [string]$Organization = 'Personal',
    [string]$City = 'Unknown',
    [string]$State = 'Unknown',
    [string]$CountryCode = 'US',
    [int]$ValidityDays = 9125,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function ConvertTo-PlainText {
    param(
        [Parameter(Mandatory = $true)]
        [Security.SecureString]$SecureString
    )

    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($SecureString)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    }
    finally {
        if ($bstr -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        }
    }
}

function Resolve-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

$resolvedProjectRoot = Resolve-AbsolutePath -Path $ProjectRoot -BasePath (Get-Location).Path
$keytoolExe = Join-Path $JavaHome 'bin\keytool.exe'
if (-not (Test-Path $keytoolExe)) {
    throw "keytool.exe not found under: $JavaHome"
}

$resolvedOutputPath = Resolve-AbsolutePath -Path $OutputPath -BasePath $resolvedProjectRoot
$outputDir = Split-Path -Parent $resolvedOutputPath
if ($outputDir -and -not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir | Out-Null
}

if ((Test-Path $resolvedOutputPath) -and -not $Force) {
    throw "Keystore already exists: $resolvedOutputPath. Use -Force to overwrite it."
}

$storePasswordSecure = Read-Host 'Enter keystore password' -AsSecureString
$confirmStorePasswordSecure = Read-Host 'Confirm keystore password' -AsSecureString
$storePassword = ConvertTo-PlainText -SecureString $storePasswordSecure
$confirmStorePassword = ConvertTo-PlainText -SecureString $confirmStorePasswordSecure

if ($storePassword -ne $confirmStorePassword) {
    throw 'Keystore passwords did not match.'
}

$keyPasswordSecure = Read-Host 'Enter key password (press Enter to reuse keystore password)' -AsSecureString
$keyPassword = ConvertTo-PlainText -SecureString $keyPasswordSecure
if ([string]::IsNullOrWhiteSpace($keyPassword)) {
    $keyPassword = $storePassword
}

$dname = "CN=$CommonName, OU=$OrgUnit, O=$Organization, L=$City, S=$State, C=$CountryCode"

$arguments = @(
    '-genkeypair'
    '-v'
    '-storetype', 'PKCS12'
    '-keystore', $resolvedOutputPath
    '-alias', $Alias
    '-keyalg', 'RSA'
    '-keysize', '4096'
    '-validity', $ValidityDays
    '-storepass', $storePassword
    '-keypass', $keyPassword
    '-dname', $dname
)

try {
    & $keytoolExe @arguments
}
finally {
    $storePassword = $null
    $confirmStorePassword = $null
    $keyPassword = $null
}

Write-Host "[create-keystore] Keystore created at: $resolvedOutputPath"
Write-Host "[create-keystore] Alias: $Alias"
Write-Host '[create-keystore] Keep this file and both passwords safe'