param(
    [Alias('Task')]
    [string[]]$Tasks = @('assembleDebug'),
    [string]$ProjectRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$GradleWrapperPath,
    [string]$JavaHome = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$AndroidSdkRoot = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$LocalPropertiesPath,
    [string]$StoreFile,
    [string]$StorePassword,
    [string]$KeyAlias,
    [string]$KeyPassword,
    [string]$GradleUserHome,
    [string[]]$AdditionalGradleArgs = @()
)

$ErrorActionPreference = 'Stop'

function Resolve-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    }

    if ([string]::IsNullOrWhiteSpace($BasePath)) {
        return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

$resolvedProjectRoot = Resolve-AbsolutePath -Path $ProjectRoot
if (-not (Test-Path $resolvedProjectRoot)) {
    throw "Project root does not exist: $resolvedProjectRoot"
}

if ([string]::IsNullOrWhiteSpace($GradleWrapperPath)) {
    $GradleWrapperPath = Join-Path $resolvedProjectRoot 'gradlew.bat'
}

$resolvedGradleWrapperPath = Resolve-AbsolutePath -Path $GradleWrapperPath -BasePath $resolvedProjectRoot
if (-not (Test-Path $resolvedGradleWrapperPath)) {
    throw "Gradle wrapper was not found: $resolvedGradleWrapperPath"
}

$javaExe = Join-Path $JavaHome 'bin\java.exe'
if (-not (Test-Path $JavaHome)) {
    throw "JAVA_HOME path does not exist: $JavaHome"
}

if (-not (Test-Path $javaExe)) {
    throw "java.exe not found under: $JavaHome"
}

if (-not (Test-Path $AndroidSdkRoot)) {
    throw "Android SDK path does not exist: $AndroidSdkRoot"
}

if ([string]::IsNullOrWhiteSpace($LocalPropertiesPath)) {
    $LocalPropertiesPath = Join-Path $resolvedProjectRoot 'local.properties'
}

$resolvedLocalPropertiesPath = Resolve-AbsolutePath -Path $LocalPropertiesPath -BasePath $resolvedProjectRoot
$escapedSdk = $AndroidSdkRoot.Replace('\', '\\').Replace(':', '\:')
Set-Content -Path $resolvedLocalPropertiesPath -Value "sdk.dir=$escapedSdk" -Encoding ASCII

$signingValues = @($StoreFile, $StorePassword, $KeyAlias, $KeyPassword) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
if ($signingValues.Count -gt 0 -and $signingValues.Count -lt 4) {
    throw 'Signing requires StoreFile, StorePassword, KeyAlias, and KeyPassword.'
}

$env:JAVA_HOME = $JavaHome
if (-not [string]::IsNullOrWhiteSpace($GradleUserHome)) {
    $env:GRADLE_USER_HOME = (Resolve-AbsolutePath -Path $GradleUserHome -BasePath $resolvedProjectRoot)
}

$gradleArgs = @()
$gradleArgs += $Tasks

if ($signingValues.Count -eq 4) {
    $resolvedStoreFile = Resolve-AbsolutePath -Path $StoreFile -BasePath $resolvedProjectRoot
    if (-not (Test-Path $resolvedStoreFile)) {
        throw "Signing keystore was not found: $resolvedStoreFile"
    }

    $gradleArgs += @(
        "-Pandroid.injected.signing.store.file=$resolvedStoreFile"
        "-Pandroid.injected.signing.store.password=$StorePassword"
        "-Pandroid.injected.signing.key.alias=$KeyAlias"
        "-Pandroid.injected.signing.key.password=$KeyPassword"
    )
}

if ($AdditionalGradleArgs.Count -gt 0) {
    $gradleArgs += $AdditionalGradleArgs
}

$gradleArgs += '--stacktrace'

Write-Host "[build-gradle] Project root: $resolvedProjectRoot"
Write-Host "[build-gradle] Tasks: $($Tasks -join ', ')"
Write-Host "[build-gradle] Gradle wrapper: $resolvedGradleWrapperPath"
Write-Host "[build-gradle] Java: $javaExe"
Write-Host "[build-gradle] Android SDK: $AndroidSdkRoot"
if (-not [string]::IsNullOrWhiteSpace($env:GRADLE_USER_HOME)) {
    Write-Host "[build-gradle] GRADLE_USER_HOME: $env:GRADLE_USER_HOME"
}

Push-Location $resolvedProjectRoot
try {
    & $resolvedGradleWrapperPath @gradleArgs
}
finally {
    Pop-Location
}