<#
    build-rust.ps1 — build the alternative Rust workspace against the C++ catalogue.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\build\rust\build-rust.ps1
      powershell -ExecutionPolicy Bypass -File scripts\build\rust\build-rust.ps1 -Clean
      powershell -ExecutionPolicy Bypass -File scripts\build\rust\build-rust.ps1 -Tests
      powershell -ExecutionPolicy Bypass -File scripts\build\rust\build-rust.ps1 -Package streamfind-rust-mass-spec

    Flags:
      -Clean    wipe the cargo target dir first
      -Tests    also run cargo test (all targets)
      -Package  build/test a single workspace package (default: all)
      -Release  use the release profile (default: debug)
      -Features comma-separated Cargo features to enable
#>
param(
    [switch]$Clean,
    [switch]$Tests,
    [string]$Package = '',
    [switch]$Release,
    [string]$Features = '',
    [string]$CppCatalogue = ''
)

. "$PSScriptRoot\..\build-common.ps1"
Start-ScriptLog 'build-rust'
Invoke-VcvarsAll -Arch 'x64'

if (-not $CppCatalogue) {
    $CppCatalogue = Join-Path $Script:TMP_BUILD 'core-default\semantic_catalogue\catalogue.duckdb'
}
if (-not (Test-Path $CppCatalogue -PathType Leaf)) {
    throw "C++ catalogue not found at $CppCatalogue - build the C++ backend first with scripts\build\cpp\build-cpp.cmd."
}
$env:STREAMFIND_CATALOGUE = (Resolve-Path $CppCatalogue).Path
Write-Log "Conformance catalogue: $env:STREAMFIND_CATALOGUE"

$cargo     = Get-Cargo
$targetDir = Join-Path $Script:TMP_BUILD 'rust-target'
$workspace = Join-Path $Script:REPO_ROOT 'rust'

if (-not (Test-Path (Join-Path $workspace 'Cargo.toml'))) {
    throw "Rust workspace not found at $workspace"
}

# Centralize cargo artifacts under tmp/ (AGENTS.md) regardless of .cargo config.
$env:CARGO_TARGET_DIR = $targetDir

# On Windows, use the repository's C++ DuckDB package instead of compiling a
# second engine through libduckdb-sys. The Windows dependency intentionally
# omits DuckDB features that force the bundled build (for example `json`).
if ($env:OS -eq 'Windows_NT') {
    $duckdbRoot = Join-Path $Script:REPO_ROOT 'cpp\vendor\duckdb'
    $env:DUCKDB_INCLUDE_DIR = Join-Path $duckdbRoot 'include'
    $env:DUCKDB_LIB_DIR = Join-Path $duckdbRoot 'lib\windows-x64'
    $env:DUCKDB_STATIC = '0'
}

if ($Clean -and (Test-Path $targetDir)) {
    Write-Log "Cleaning cargo target dir: $targetDir"
    Remove-Item -Recurse -Force $targetDir
}

$manifest = Join-Path $workspace 'Cargo.toml'

if ($Tests) {
    $testArgs = @('test', '--manifest-path', $manifest)
    if ($Package)  { $testArgs += @('-p', $Package) }
    if ($Release)  { $testArgs += '--release' }
    if ($Features) { $testArgs += @('--features', $Features) }
    Write-Log "cargo $($testArgs -join ' ')"
    & $cargo @testArgs
    if ($LASTEXITCODE -ne 0) { throw "cargo test failed ($LASTEXITCODE)" }
} else {
    $buildArgs = @('build', '--manifest-path', $manifest)
    if ($Package)  { $buildArgs += @('-p', $Package) }
    if ($Release)  { $buildArgs += '--release' }
    if ($Features) { $buildArgs += @('--features', $Features) }
    Write-Log "cargo $($buildArgs -join ' ')"
    & $cargo @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed ($LASTEXITCODE)" }
}

Write-Log "Done. Target dir: $targetDir"