# Configure + build calligrus in one go.
#   .\build.ps1                      -> Release build in build\
#   .\build.ps1 -Config Debug
#   .\build.ps1 -BuildDir build-test
# Output: <BuildDir>\<Config>\calligrus.exe, a single file: every library and the
# C/C++ runtime are linked in statically (vcpkg triplet x64-windows-static),
# and the fonts, themes and demo are embedded, so it runs without DLLs or an
# assets folder.
param([string]$Config = "Release", [string]$BuildDir = "build", [string]$Triplet = "x64-windows-static")
$ErrorActionPreference = "Stop"

# Find vcpkg: $env:VCPKG_ROOT first, then the usual install locations.
$vcpkg = $env:VCPKG_ROOT
if (-not $vcpkg) {
    $vcpkg = @("C:\tools\vcpkg", "C:\vcpkg", "C:\src\vcpkg", "$env:USERPROFILE\vcpkg") |
        Where-Object { Test-Path "$_\scripts\buildsystems\vcpkg.cmake" } | Select-Object -First 1
}
if (-not $vcpkg) { throw "vcpkg not found; set `$env:VCPKG_ROOT" }

$src = $PSScriptRoot
$build = Join-Path $src $BuildDir

# A build folder configured for another triplet (e.g. the old dynamic
# x64-windows) would keep stale library paths in its cache: start it over.
$cache = Join-Path $build "CMakeCache.txt"
if ((Test-Path $cache) -and -not (Select-String -Path $cache -Pattern "^VCPKG_TARGET_TRIPLET:STRING=$Triplet$" -Quiet)) {
    Write-Host "Triplet changed: clearing $cache"
    Remove-Item $cache
    Remove-Item -Recurse -Force (Join-Path $build "CMakeFiles") -ErrorAction SilentlyContinue
}

# The Visual Studio generator finds MSVC by itself (no vcvars needed).
cmake -S $src -B $build -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=$Triplet"
if ($LASTEXITCODE) { exit $LASTEXITCODE }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "Built: $build\$Config\calligrus.exe"
