<#
.SYNOPSIS
  Aligns and signs an unsigned Calligrus release APK with your release keystore (Windows).

.DESCRIPTION
  Runs zipalign, then apksigner sign, then apksigner verify. The password comes from -PasswordFile if
  that file exists (apksigner reads it directly), else apksigner asks for it interactively. This script
  itself never reads or passes a password.

.EXAMPLE
  .\sign-release.ps1 -Apk calligrus-0.1.0-unsigned.apk
  .\sign-release.ps1 -Apk in.apk -Out out.apk -Keystore D:\keys\my.jks -Alias mykey
#>
param(
    [Parameter(Mandatory = $true)][string]$Apk,
    [string]$Out,
    [string]$Keystore = (Join-Path $env:USERPROFILE ".kaoss\kaoss-release.jks"),
    [string]$Alias = "calligrus",
    # A file holding only the keystore password (kept next to the keystore, never in a project). If it
    # exists, signing needs no input; otherwise apksigner asks.
    [string]$PasswordFile = (Join-Path $env:USERPROFILE ".kaoss\kaoss-release.pass")
)

$ErrorActionPreference = "Stop"

function Fail([string]$msg) {
    Write-Host "error: $msg" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $Apk -PathType Leaf)) { Fail "APK not found: $Apk" }
if (-not (Test-Path -LiteralPath $Keystore -PathType Leaf)) {
    Fail "keystore not found: $Keystore (pass -Keystore <path>)"
}
$Apk = (Resolve-Path -LiteralPath $Apk).Path
if (-not $Out) {
    $Out = [System.IO.Path]::ChangeExtension($Apk, $null).TrimEnd('.')
    $Out = ($Out -replace '-unsigned$', '') + "-signed.apk"
}
if ($Out -eq $Apk) { Fail "the output must differ from the input" }

# Newest build-tools folder that has both tools.
$roots = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "C:\tools\android-sdk") |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ "build-tools")) }
if (-not $roots) { Fail "no Android SDK found: set ANDROID_HOME (or ANDROID_SDK_ROOT), or install it at C:\tools\android-sdk" }

$tools = $null
foreach ($root in $roots) {
    $tools = Get-ChildItem -LiteralPath (Join-Path $root "build-tools") -Directory |
        Where-Object {
            (Test-Path (Join-Path $_.FullName "zipalign.exe")) -and
            ((Test-Path (Join-Path $_.FullName "apksigner.bat")) -or (Test-Path (Join-Path $_.FullName "apksigner.exe")))
        } |
        Sort-Object { try { [version]($_.Name -replace '[^0-9.].*$', '') } catch { [version]"0.0" } } -Descending |
        Select-Object -First 1
    if ($tools) { break }
}
if (-not $tools) { Fail "no build-tools with zipalign and apksigner under: $($roots -join ', ') (install them with sdkmanager `"build-tools;36.0.0`")" }

$zipalign = Join-Path $tools.FullName "zipalign.exe"
$apksigner = Join-Path $tools.FullName "apksigner.bat"
if (-not (Test-Path $apksigner)) { $apksigner = Join-Path $tools.FullName "apksigner.exe" }
Write-Host "build-tools: $($tools.Name)"

$aligned = Join-Path ([System.IO.Path]::GetTempPath()) ("calligrus-aligned-" + [guid]::NewGuid().ToString("N") + ".apk")
try {
    Write-Host "aligning..."
    & $zipalign -p -f 4 $Apk $aligned
    if ($LASTEXITCODE -ne 0) { Fail "zipalign failed (exit $LASTEXITCODE)" }

    if (Test-Path -LiteralPath $PasswordFile -PathType Leaf) {
        # apksigner reads the password from the file itself; it never passes through this script.
        Write-Host "signing with $Keystore (alias $Alias), password from $PasswordFile..."
        & $apksigner sign --ks $Keystore --ks-key-alias $Alias --ks-pass "file:$PasswordFile" --out $Out $aligned
    } else {
        Write-Host "signing with $Keystore (alias $Alias); apksigner will ask for the password..."
        & $apksigner sign --ks $Keystore --ks-key-alias $Alias --out $Out $aligned
    }
    if ($LASTEXITCODE -ne 0) { Fail "apksigner sign failed (exit $LASTEXITCODE)" }
}
finally {
    Remove-Item -LiteralPath $aligned -Force -ErrorAction SilentlyContinue
}

Write-Host "verifying..."
$certs = & $apksigner verify --print-certs $Out
if ($LASTEXITCODE -ne 0) { Fail "apksigner verify failed (exit $LASTEXITCODE)" }
$certs | Where-Object { $_ -match 'certificate (DN|SHA-256)' } | ForEach-Object { Write-Host $_ }
Write-Host "signed APK: $Out" -ForegroundColor Green
