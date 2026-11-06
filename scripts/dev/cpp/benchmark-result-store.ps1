[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot 'tmp\build\windows\core'
}
$BuildDirectory = (Resolve-Path $BuildDirectory).Path
$benchmark = Join-Path $BuildDirectory 'streamfind_dev_result_store_benchmark.exe'

if (-not $SkipBuild) {
    cmake --build $BuildDirectory --target streamfind_dev_result_store_benchmark -j 8
    if ($LASTEXITCODE -ne 0) { throw "Result-store benchmark build failed ($LASTEXITCODE)." }
}
if (-not (Test-Path $benchmark -PathType Leaf)) {
    throw "Benchmark executable not found: $benchmark. Build it first or omit -SkipBuild."
}

$logRoot = Join-Path $repoRoot 'tmp\logs'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$logPath = Join-Path $logRoot ("result-store-benchmark-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-Host "Benchmark log: $logPath"
& $benchmark 2>&1 | Tee-Object -FilePath $logPath
if ($LASTEXITCODE -ne 0) { throw "Result-store benchmark failed ($LASTEXITCODE). See $logPath" }
