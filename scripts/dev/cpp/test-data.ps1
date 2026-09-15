[CmdletBinding()]
param(
    [ValidateSet('Cpp')]
    [string]$Backend = 'Cpp',
    [string]$RelativePath = 'mass_spec\basic_tof\00_tof_s_is_pos_cent-r001.mzML',
    [switch]$SkipBuild
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dataRoot = Get-StreamfindDataRoot $repoRoot
$dataFile = Join-Path $dataRoot $RelativePath
if (-not (Test-Path $dataFile -PathType Leaf)) {
    throw "Required test data file not found: $dataFile"
}

if (-not $SkipBuild) {
    if ($Backend -eq 'Cpp') {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'scripts\build\cpp\build-cpp.ps1') -Clean -Config Release
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
    } else {
        $buildExitCode = Invoke-StreamfindRustBuild $repoRoot
        if ($buildExitCode -ne 0) { throw "Rust build failed ($buildExitCode)" }
    }
}

$executable = Get-BackendMcpExecutable -RepositoryRoot $repoRoot -Backend $Backend
$catalogue = Join-Path $repoRoot 'tmp\build\core-default\semantic_catalogue\catalogue.duckdb'
$database = Join-Path $repoRoot "tmp\projects\streamfind-$($Backend.ToLowerInvariant())-data-script.duckdb"
$projectId = "data-$($Backend.ToLowerInvariant())"
$analysisName = [System.IO.Path]::GetFileNameWithoutExtension($dataFile)
New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
$env:STREAMFIND_CATALOGUE = $catalogue
if ($Backend -eq 'Cpp') {
    $env:PATH = (Join-Path $repoRoot 'tmp\build\core-default\tests') + ';' + $env:PATH
}

$process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
try {
    Initialize-Mcp $process | Out-Null
    $tools = Send-McpRequest $process @{
        jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{}
    }
    if ($tools.PSObject.Properties.Name -contains 'error' -and $null -ne $tools.error) {
        throw "tools/list failed: $($tools.error.message)"
    }
    $toolNames = @($tools.result.tools | ForEach-Object { $_.name })
    if ($toolNames -notcontains 'mass_spec.add_analyses') {
        throw 'mass_spec.add_analyses is not advertised by the built MCP server'
    }

    Invoke-McpTool $process 3 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $added = Invoke-McpTool $process 4 'mass_spec.add_analyses' @{
        database_path = $database
        analyses = @(@{ path = $dataFile })
    }
    if ([int]$added.row_count -ne 1) {
        throw "Expected one parsed analysis, received $($added.row_count)"
    }
    $info = Invoke-McpTool $process 5 'mass_spec.get_analyses_info' @{
        database_path = $database
    }
    if ([int]$info.row_count -ne 1) {
        throw "Expected one persisted analysis, received $($info.row_count)"
    }
    $headers = Invoke-McpTool $process 6 'mass_spec.get_spectra_headers' @{
        database_path = $database
        analysis_names = @($analysisName)
    }
    if ([int]$headers.row_count -le 0) {
        throw 'Parsed source produced no spectrum headers'
    }
    $firstHeader = [ordered]@{}
    foreach ($column in $headers.columns.PSObject.Properties) {
        $values = @($column.Value)
        $firstHeader[$column.Name] = if ($values.Count -gt 0) { $values[0] } else { $null }
    }
    Write-Host ("First spectra header row: " + ($firstHeader | ConvertTo-Json -Compress -Depth 20))
    Write-Host "$Backend data test passed: $RelativePath ($($headers.row_count) spectrum headers)."
} finally {
    Stop-StreamfindMcp $process
    Remove-Item -Force -ErrorAction SilentlyContinue $database
}
