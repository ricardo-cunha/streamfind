param(
    [Parameter(Mandatory = $true)][string]$BrukerPath,
    [switch]$AllSpectra
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$buildRoot = Join-Path $repoRoot "tmp\build\windows\core"
$scratch = Join-Path $repoRoot "tmp\scratch"
$exe = Join-Path $scratch "validate_bruker_calibration.exe"
$source = Join-Path $PSScriptRoot "validate_bruker_calibration.cpp"
$duckdbImportLibrary = Join-Path $repoRoot "cpp\vendor\duckdb\lib\windows-x64\duckdb.lib"
$duckdbInclude = Join-Path $repoRoot "cpp\vendor\duckdb\include"
$zstdLibrary = Join-Path $buildRoot "streamfind-zstd\lib\libzstd.a"
$coreInclude = Join-Path $repoRoot "cpp\core\include"
$massSpecSource = Join-Path $repoRoot "cpp\plugins\mass_spec\src"

if (-not (Test-Path $duckdbImportLibrary)) {
    throw "The Windows DuckDB import library is missing. Run sfdev setup and configure the Windows backend."
}
if (-not (Test-Path $zstdLibrary)) {
    throw "The Windows zstd library is missing. Build the Windows backend first."
}
New-Item -ItemType Directory -Path $scratch -Force | Out-Null

$sfdev = Join-Path $repoRoot "scripts\sfdev.ps1"
$environment = (& (Join-Path $PSHOME "pwsh.exe") -NoProfile -File $sfdev env show --json | ConvertFrom-Json)
$compiler = ($environment.tools | Where-Object { $_.name -eq "gxx" -and $_.available }).path
if (-not $compiler -or -not (Test-Path $compiler)) {
    throw "sfdev could not resolve the configured MinGW-w64 UCRT64 C++ compiler."
}

$compileArguments = @(
    "-std=c++20", "-O2",
    "-I$massSpecSource", "-I$coreInclude", "-I$duckdbInclude",
    $source,
    (Join-Path $massSpecSource "readers\reader_bruker.cpp"),
    (Join-Path $massSpecSource "readers\reader_bruker_calibration.cpp"),
    $duckdbImportLibrary, $zstdLibrary,
    "-o", $exe
)
& $compiler @compileArguments
if ($LASTEXITCODE -ne 0) {
    throw "Could not build the Bruker TSF calibration validation helper."
}

$env:PATH = (Join-Path $buildRoot "bin") + ";" + $env:PATH
$validationArguments = @($BrukerPath)
if ($AllSpectra) {
    $validationArguments += "--all-spectra"
}
& $exe @validationArguments
if ($LASTEXITCODE -ne 0) {
    throw "Bruker calibration data check failed."
}
