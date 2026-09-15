[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CppExecutable,
    [Parameter(Mandatory = $true)][string]$RustExecutable,
    [switch]$Thermo,
    [switch]$Sciex
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$python = Join-Path $repoRoot '.venv\Scripts\python.exe'
if (-not (Test-Path $python -PathType Leaf)) { throw "Repository Python environment not found: $python" }

$cpp = (Resolve-Path $CppExecutable).Path
$rust = (Resolve-Path $RustExecutable).Path
foreach ($path in @($cpp, $rust)) {
    if (-not (Test-Path $path -PathType Leaf)) { throw "Backend executable not found: $path" }
}

$env:STREAMFIND_CPP_MCP = $cpp
$env:STREAMFIND_RUST_MCP = $rust
$ran = 0

if ($Thermo) {
    & $python (Join-Path $PSScriptRoot 'test-thermo-readers.py')
    if ($LASTEXITCODE -ne 0) { throw "Thermo conformance failed ($LASTEXITCODE)" }
    $ran++
}
if ($Sciex) {
    & $python (Join-Path $PSScriptRoot 'test-sciex-mcp-differential.py')
    if ($LASTEXITCODE -ne 0) { throw "SCIEX conformance failed ($LASTEXITCODE)" }
    $ran++
}

if ($ran -eq 0) {
    throw 'Select at least one conformance corpus with -Thermo or -Sciex.'
}
Write-Host "Conformance completed: $ran corpus runner(s)."
