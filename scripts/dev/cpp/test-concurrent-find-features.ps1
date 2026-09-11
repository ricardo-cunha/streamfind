[CmdletBinding()]
param(
    [switch]$Child,
    [ValidateSet('Cpp')]
    [string]$Backend = 'Cpp',
    [int]$WorkerIndex = 0,
    [ValidateSet('Completed', 'Cancelled', 'Failed')]
    [string]$Scenario = 'Completed',

    [string]$DatabasePath = '',
    [string]$LogPath = '',
    [string]$FixturePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\mcp-common.ps1')

foreach ($name in @('tmp', 'temp', 'tmpdir')) {
    if (Test-Path "Env:$name") { Remove-Item "Env:$name" }
}

$scriptPath = $PSCommandPath
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dataRoot = Get-StreamfindDataRoot $repoRoot
$fixtureRoot = Join-Path $dataRoot 'mass_spec\wastewater'
$fixtures = @(
    Join-Path $fixtureRoot '01_tof_ww_is_pos_blank-r001.mzML'
    Join-Path $fixtureRoot '01_tof_ww_is_pos_blank-r002.mzML'
)
foreach ($fixture in $fixtures) {
    if (-not (Test-Path $fixture -PathType Leaf)) { throw "Missing mzML fixture: $fixture" }
}

$catalogue = Join-Path $repoRoot 'tmp\build\core-default\semantic_catalogue\catalogue.duckdb'
$executable = Get-BackendMcpExecutable -RepositoryRoot $repoRoot -Backend $Backend
if ($Backend -eq 'Rust' -and -not (Test-Path $executable -PathType Leaf)) {
    $executable = Join-Path $repoRoot 'tmp\build\rust-target\debug\streamfind-rust-mcp.exe'
}
if (-not (Test-Path $executable -PathType Leaf)) { throw "MCP executable not found: $executable" }
$env:STREAMFIND_CATALOGUE = $catalogue
if ($Backend -eq 'Cpp') {
    $env:PATH = (Join-Path $repoRoot 'tmp\build\core-default\tests') + ';' + $env:PATH
}
$projectsRoot = Join-Path $repoRoot 'tmp\projects'
$logsRoot = Join-Path $repoRoot 'tmp\logs\concurrent-find-features'
New-Item -ItemType Directory -Force -Path $projectsRoot, $logsRoot | Out-Null

function Invoke-Child {
    param([Parameter(Mandatory = $true)][int]$Index)
    $database = Join-Path $projectsRoot "concurrent-find-features-$($Backend.ToLowerInvariant())-$Index.duckdb"
    $log = Join-Path $logsRoot "worker-$Index.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $database, $log
    $script = $scriptPath
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script,
        '-Child', '-Backend', $Backend,
        '-Scenario', $Scenario,
        '-WorkerIndex', $Index,
        '-DatabasePath', $database,
        '-LogPath', $log,
        '-FixturePath', $fixtures[$Index - 1]
    )
    return Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WorkingDirectory $repoRoot -RedirectStandardOutput $log -RedirectStandardError ($log + '.err') -PassThru
}

if (-not $Child) {
    $processes = @(Invoke-Child 1; Invoke-Child 2)
    foreach ($process in $processes) {
        $process.WaitForExit()
        if (-not [string]::IsNullOrWhiteSpace([string]$process.ExitCode) -and $process.ExitCode -ne 0) {
            throw "Concurrent worker process $($process.Id) failed with exit code $($process.ExitCode)"
        }
    }
    $summaries = @()
    foreach ($index in 1, 2) {
        $log = Join-Path $logsRoot "worker-$index.log"
        $summaryLine = @(Get-Content -LiteralPath $log | Where-Object { $_ -like 'SUMMARY=*' } | Select-Object -Last 1)
        if ($summaryLine.Count -ne 1) { throw "Worker $index did not emit a summary; inspect $log" }
        $summaries += ($summaryLine[0].Substring(8) | ConvertFrom-Json)
    }
    $start = ($summaries | Measure-Object -Property start_utc -Minimum).Minimum
    $end = ($summaries | Measure-Object -Property end_utc -Maximum).Maximum
    $latestStart = ($summaries | Measure-Object -Property start_utc -Maximum).Maximum
    $earliestEnd = ($summaries | Measure-Object -Property end_utc -Minimum).Minimum
    if ($Scenario -eq 'Completed' -and [DateTime]::Parse($latestStart) -ge [DateTime]::Parse($earliestEnd)) {
        throw "The two real find_features executions did not overlap: $($summaries | ConvertTo-Json -Compress)"
    }
    foreach ($summary in $summaries) {
        $expectedStatus = if ($Scenario -eq 'Completed') { 'completed' } elseif ($Scenario -eq 'Cancelled') { 'cancelled' } else { 'failed' }
        if ($summary.execution_status -ne $expectedStatus) { throw "Worker $($summary.worker) execution status was $($summary.execution_status), expected $expectedStatus" }
        if ($Scenario -eq 'Completed' -and [int]$summary.feature_rows -le 0) { throw "Worker $($summary.worker) produced no feature rows" }
    }
    Write-Output ($summaries | ConvertTo-Json -Depth 8)
    Write-Output "overlap_start=$latestStart overlap_end=$earliestEnd"
    Write-Output "verified $Scenario find_features workflow executions for two project databases"
    exit 0
}

if ($WorkerIndex -lt 1 -or [string]::IsNullOrWhiteSpace($DatabasePath) -or [string]::IsNullOrWhiteSpace($FixturePath)) {
    throw 'Child mode requires worker index, database path, and fixture path'
}
$projectId = "concurrent-$($Backend.ToLowerInvariant())-$WorkerIndex"
$process = $null
try {
    $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
    Initialize-Mcp $process | Out-Null
    Invoke-McpTool $process 2 'create' @{ database_path = $DatabasePath; domain = 'mass_spec' } | Out-Null
    $added = Invoke-McpTool $process 3 'mass_spec.add_analyses' @{ database_path = $DatabasePath; analyses = @(@{ path = $FixturePath }) }
    if ([int]$added.row_count -ne 1) { throw "Expected one imported analysis, received $($added.row_count)" }
    $analysis = [System.IO.Path]::GetFileNameWithoutExtension($FixturePath)
    Invoke-McpTool $process 3 'mass_spec.get_features' @{ database_path = $DatabasePath; analysis_names = @($analysis) } | Out-Null
    $parameters = @{
        analysis_names = @($analysis)
        rt_windows_min = @()
        rt_windows_max = @()
        ppm_threshold = 10.0
        noise_threshold = 250.0
        min_snr = 3.0
        min_traces = 3
        baseline_window = 200.0
        max_feature_width = 250.0
        base_quantile = 0.99
    }
    $workflow = [ordered]@{
        name = 'concurrent-find-features-components'
        version = 1
        domain = 'mass_spec'
        steps = @(
            [ordered]@{ method = 'mass_spec.find_features'; parameters = $parameters }
            [ordered]@{ method = 'mass_spec.create_components'; parameters = [ordered]@{ analysis_names = @($analysis); rt_window = @(-2.5, 2.5); min_correlation = 0.85 } }
        )
    }
    if ($Scenario -eq 'Failed') {
        Stop-StreamfindMcp $process
        $process = $null
        $python = Join-Path $repoRoot '.venv\Scripts\python.exe'
        & $python (Join-Path $repoRoot 'tmp\scratch\corrupt-analysis-path.py') $DatabasePath
        if ($LASTEXITCODE -ne 0) { throw "Failed to corrupt disposable spectra fixture" }
        $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
        Initialize-Mcp $process | Out-Null
    }
    Invoke-McpTool $process 4 'set_workflow' @{ database_path = $DatabasePath; workflow = $workflow } | Out-Null
    Invoke-McpTool $process 5 'create_workflow_execution' @{ database_path = $DatabasePath } | Out-Null
    $controllerProcess = $null
    if ($Scenario -eq 'Cancelled') { Invoke-McpTool $process 6 'cancel_execution' @{ database_path = $DatabasePath } | Out-Null }
    $start = [DateTime]::UtcNow
    Write-Output "START worker=$WorkerIndex utc=$($start.ToString('o')) fixture=$FixturePath"
    $runError = $null
    try {
        $result = Invoke-McpTool $process 6 'run_workflow' @{ database_path = $DatabasePath; worker_id = "worker-$WorkerIndex" }
    } catch {
        $runError = $_.Exception.Message
        $result = @{ error = $runError }
    }
    if ($null -ne $controllerProcess) { $controllerProcess.WaitForExit() }
    $end = [DateTime]::UtcNow
    $execution = Invoke-McpTool $process 7 'get_execution' @{ database_path = $DatabasePath }
    $features = @{ row_count = 0 }
    if ($Scenario -ne 'Cancelled' -and $Scenario -ne 'Failed') {
        $features = Invoke-McpTool $process 8 'mass_spec.get_features' @{ database_path = $DatabasePath; analysis_names = @($analysis); filtered = $false }
    } elseif ($Scenario -eq 'Cancelled') {
        try { $features = Invoke-McpTool $process 8 'mass_spec.get_features' @{ database_path = $DatabasePath; analysis_names = @($analysis); filtered = $false } } catch { $features = @{ row_count = 0 } }
    }
    $executionRows = @(if (($execution.PSObject.Properties.Name -contains 'rows') -and $execution.rows) { @($execution.rows) } else { @($execution) })
    $status = if (($execution.PSObject.Properties.Name -contains 'columns') -and ($execution.columns.PSObject.Properties.Name -contains 'status')) { @($execution.columns.status)[0] } elseif ($executionRows.Count -gt 0 -and $executionRows[0].PSObject.Properties.Name -contains 'status') { $executionRows[0].status } elseif ($execution.PSObject.Properties.Name -contains 'status') { $execution.status } else { 'unknown' }
    $featureRows = if (($features.PSObject.Properties.Name -contains 'row_count') -and $features.row_count) { [int]$features.row_count } elseif (($features.PSObject.Properties.Name -contains 'rows') -and $features.rows) { @($features.rows).Count } else { 0 }

    $progressValue = if ($execution.PSObject.Properties.Name -contains 'progress') { $execution.progress } else { '{}' }
    $progressObject = if ($progressValue -is [string]) { $progressValue | ConvertFrom-Json } else { $progressValue }
    $progress = $progressObject | ConvertTo-Json -Compress
    if ($Scenario -eq 'Completed' -and ($status -ne 'completed' -or $null -eq $progressObject -or [int]$progressObject.completed -ne 2 -or [int]$progressObject.total -ne 2)) { throw "Expected durable two-step progress, received status=$status progress=$progress" }
    if ($Scenario -eq 'Cancelled' -and ($status -ne 'cancelled' -or ((@($progressObject.PSObject.Properties | Where-Object { $_.Name -eq 'completed' }).Count -gt 0) -and [int]$progressObject.completed -gt 1))) { throw "Expected queued cancellation, received status=$status progress=$progress" }
    if ($Scenario -eq 'Failed' -and ($status -ne 'failed' -or [string]::IsNullOrWhiteSpace([string]$execution.error))) { throw "Expected failed execution with error, received status=$status error=$($execution.error)" }
    Write-Output "END worker=$WorkerIndex utc=$($end.ToString('o')) status=$status feature_rows=$featureRows progress=$progress"
    Write-Output ('SUMMARY=' + (@{ worker = $WorkerIndex; scenario = $Scenario; database = $DatabasePath; fixture = $FixturePath; start_utc = $start.ToString('o'); end_utc = $end.ToString('o'); execution_status = $status; feature_rows = $featureRows; progress = $progressObject; error = $execution.error; run_error = $runError; result = $result } | ConvertTo-Json -Compress -Depth 8))
} finally {
    if ($null -ne $process) { Stop-StreamfindMcp $process }
}
