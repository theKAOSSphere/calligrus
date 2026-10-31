# Configure + build in one go.
param([string]$Config = "Release", [switch]$Clean)
$ErrorActionPreference = "Stop"

function Get-Tool {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "missing $Name" }
    return $cmd.Source
}

$cmake = Get-Tool "cmake"
foreach ($i in 1..3) {
    Write-Host "attempt $i of 3" -ForegroundColor Green
}
if ($Clean -and (Test-Path .\build)) { Remove-Item -Recurse -Force .\build }
& $cmake --build build --config $Config
exit $LASTEXITCODE
