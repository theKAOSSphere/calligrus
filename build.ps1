# Configure + build calligrus in one go.
#   .\build.ps1                      -> Release build in build\
#   .\build.ps1 -Config Debug
#   .\build.ps1 -BuildDir build-test
# Output: <BuildDir>\<Config>\calligrus.exe, a single file: the libraries are downloaded and
# built by CMake (FetchContent, cmake/deps.cmake) and linked in statically together with the
# C/C++ runtime, and the fonts, themes and demo are embedded, so it runs without DLLs or an
# assets folder. Downloads are cached in $env:CALLIGRUS_DOWNLOAD_DIR (default <BuildDir>\_downloads).
param([string]$Config = "Release", [string]$BuildDir = "build", [string]$Generator = "")
$ErrorActionPreference = "Stop"

$src = $PSScriptRoot
$build = Join-Path $src $BuildDir

# The Visual Studio generator finds MSVC by itself (no vcvars needed). Use the newest Visual Studio
# that has the C++ tools (2022 = 17, 2026 = 18), unless -Generator names one.
if (-not $Generator) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsVersion = if (Test-Path $vswhere) {
        & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    }
    $Generator = switch -Wildcard ("$vsVersion") {
        "18.*"  { "Visual Studio 18 2026" }
        default { "Visual Studio 17 2022" }
    }
}
Write-Host "Generator: $Generator"
cmake -S $src -B $build -G $Generator -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "Built: $build\$Config\calligrus.exe"
