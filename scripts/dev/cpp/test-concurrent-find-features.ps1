[CmdletBinding()]
param(
    [switch]$Child,
    [int]$WorkerIndex = 0,
    [ValidateSet('Completed', 'Cancelled', 'Failed')]
    [string]$Scenario = 'Completed',

    [string]$DatabasePath = '',
    [string]$LogPath = '',
    [string]$FixturePath = '',
    [string]$ServiceEndpoint = 'http://127.0.0.1:8790'
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

$catalogue = Join-Path $repoRoot 'tmp\build\mingw-ucrt64\semantic_catalogue\catalogue.duckdb'
$executable = Get-BackendMcpExecutable -RepositoryRoot $repoRoot
if (-not (Test-Path $executable -PathType Leaf)) { throw "MCP executable not found: $executable" }
$env:STREAMFIND_CATALOGUE = $catalogue
$env:STREAMFIND_SERVICE_ENDPOINT = $ServiceEndpoint
$env:PATH = (Join-Path $repoRoot 'tmp\build\mingw-ucrt64\tests') + ';' + $env:PATH
$projectsRoot = Join-Path $repoRoot 'tmp\projects'
$logsRoot = Join-Path $repoRoot 'tmp\logs\concurrent-find-features'
New-Item -ItemType Directory -Force -Path $projectsRoot, $logsRoot | Out-Null

function Invoke-ServiceJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    return Invoke-RestMethod -Uri ($ServiceEndpoint.TrimEnd('/') + $Path) -TimeoutSec 30
}

function Invoke-Child {
    param([Parameter(Mandatory = $true)][int]$Index)
    $database = Join-Path $projectsRoot "concurrent-find-features-cpp-$Index.duckdb"
    $resultStore = [System.IO.Path]::ChangeExtension($database, '.data')
    $log = Join-Path $logsRoot "worker-$Index.log"
    Remove-Item -Force -Recurse -ErrorAction SilentlyContinue $database, $resultStore, $log, ($log + '.err')
    $script = $scriptPath
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script,
        '-Child',
        '-Scenario', $Scenario,
        '-WorkerIndex', $Index,
        '-DatabasePath', $database,
        '-LogPath', $log,
        '-FixturePath', $fixtures[$Index - 1],
        '-ServiceEndpoint', $ServiceEndpoint
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
$projectId = "concurrent-cpp-$WorkerIndex"
$process = $null
try {
    $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
    Initialize-Mcp $process | Out-Null
    Invoke-McpTool $process 2 'create' @{ database_path = $DatabasePath; domain = 'mass_spec' } | Out-Null
    $parameters = @{
        ppm_threshold = 10.0
        noise_threshold = 250.0
        min_snr = 3.0
        min_traces = 3
        baseline_window = 200.0
        max_feature_width = 250.0
        base_quantile = 0.99
        smoothing_enabled = $true
        smoothing_window = 7
        smoothing_sharp_window = 3
        smoothing_strength = 1.0
        debug_analysis = ''
        debug_mz = 0
        debug_spec_idx = -1
        rt_windows = @(@{ rtmin = 0; rtmax = 3400 })
    }
    $workflow = [ordered]@{
        name = 'concurrent-find-features-components'
        schema_version = 2
        workflow_id = "concurrent-find-features-components-$WorkerIndex"
        version = 1
        operations = @(
            [ordered]@{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = [ordered]@{ source_paths = @($FixturePath) }; inputs = @{} }
            [ordered]@{ id = 2; operation = 'mass_spec.find_features'; parameters = $parameters; inputs = @{} }
            [ordered]@{ id = 3; operation = 'mass_spec.create_components'; parameters = [ordered]@{ rt_window = @(-2.5, 2.5); min_correlation = 0.85 }; inputs = @{} }
        )
        connections = @(
            [ordered]@{ source_operation = 1; source_port = 'analysesTable'; target_operation = 2; target_port = 'analysesTable' }
            [ordered]@{ source_operation = 1; source_port = 'spectraHeadersTable'; target_operation = 2; target_port = 'spectraHeadersTable' }
            [ordered]@{ source_operation = 2; source_port = 'featuresTable'; target_operation = 3; target_port = 'featuresTable' }
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
    $controllerProcess = $null
    $start = [DateTime]::UtcNow
    Write-Output "START worker=$WorkerIndex utc=$($start.ToString('o')) fixture=$FixturePath"
    # Keep the short development fixtures in the overlap window long enough
    # to exercise the service's concurrent-session and result-store paths.
    Start-Sleep -Milliseconds 250
    $runError = $null
    try {
        $result = Invoke-McpTool $process 6 'run_workflow' @{ database_path = $DatabasePath }
    } catch {
        $runError = $_.Exception.Message
        $result = @{ error = $runError }
    }
    if ($null -ne $controllerProcess) { $controllerProcess.WaitForExit() }
    $end = [DateTime]::UtcNow
    $project = @(Invoke-ServiceJson '/projects').projects |
        Where-Object { [System.IO.Path]::GetFullPath($_.database_path) -eq [System.IO.Path]::GetFullPath($DatabasePath) } |
        Select-Object -First 1
    if ($null -eq $project) { throw "Service session was not found for $DatabasePath" }
    $sessionId = [uri]::EscapeDataString([string]$project.session_id)
    $execution = $null
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    do {
        try {
            $execution = Invoke-ServiceJson ("/projects/$sessionId/workflow/state")
            if ($execution.PSObject.Properties.Name -contains 'state' -and
                [string]$execution.state -notin @('queued', 'running', 'cancelling')) { break }
        } catch {
            if ([DateTime]::UtcNow -gt $deadline) { throw }
            Start-Sleep -Seconds 1
        }
    } while ($null -eq $execution -or
             ($execution.PSObject.Properties.Name -contains 'state' -and [string]$execution.state -in @('queued', 'running', 'cancelling')))
    $inventory = Invoke-ServiceJson ("/projects/$sessionId/results")
    $status = [string]$execution.state
    $featureRows = @($inventory.results | Where-Object {
        (($null -ne $_.PSObject.Properties['contract_id']) -and $_.PSObject.Properties['contract_id'].Value -eq 'featuresTable') -or
        (($null -ne $_.PSObject.Properties['output_port']) -and $_.PSObject.Properties['output_port'].Value -eq 'featuresTable')
    }).Count

    $progressValue = if ($execution.PSObject.Properties.Name -contains 'progress') { $execution.progress } else { '{}' }
    $progressObject = if ($progressValue -is [string]) { $progressValue | ConvertFrom-Json } else { $progressValue }
    $progress = $progressObject | ConvertTo-Json -Compress
    if ($Scenario -eq 'Completed' -and ($status -ne 'completed' -or $null -eq $progressObject -or [int]$progressObject.completed -ne 3 -or [int]$progressObject.total -ne 3)) { throw "Expected durable three-step progress, received status=$status progress=$progress execution=$($execution | ConvertTo-Json -Compress -Depth 12)" }
    if ($Scenario -eq 'Cancelled' -and ($status -ne 'cancelled' -or ((@($progressObject.PSObject.Properties | Where-Object { $_.Name -eq 'completed' }).Count -gt 0) -and [int]$progressObject.completed -gt 1))) { throw "Expected queued cancellation, received status=$status progress=$progress" }
    if ($Scenario -eq 'Failed' -and $status -ne 'failed') { throw "Expected failed execution, received status=$status" }
    Write-Output "END worker=$WorkerIndex utc=$($end.ToString('o')) status=$status feature_rows=$featureRows progress=$progress"
    $executionError = if ($execution.PSObject.Properties.Name -contains 'error') { $execution.error } else { $null }
    Write-Output ('SUMMARY=' + (@{ worker = $WorkerIndex; scenario = $Scenario; database = $DatabasePath; fixture = $FixturePath; start_utc = $start.ToString('o'); end_utc = $end.ToString('o'); execution_status = $status; feature_rows = $featureRows; progress = $progressObject; error = $executionError; run_error = $runError; result = $result } | ConvertTo-Json -Compress -Depth 8))
} finally {
    if ($null -ne $process) { Stop-StreamfindMcp $process }
}
