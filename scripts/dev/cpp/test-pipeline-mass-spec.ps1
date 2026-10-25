[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$KeepProject
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dataRoot = Get-StreamfindDataRoot $repoRoot
$catalogue = Join-Path $repoRoot 'tmp\build\mingw-ucrt64\semantic_catalogue\catalogue.duckdb'
$executable = Get-BackendMcpExecutable $repoRoot
$buildRoot = Join-Path $repoRoot 'tmp\build\mingw-ucrt64'
$serviceExecutable = Join-Path $buildRoot 'streamfind-server.exe'
$database = Join-Path $repoRoot 'tmp\projects\pipeline-mass-spec.duckdb'
$dataStore = [System.IO.Path]::ChangeExtension($database, '.data')
$dataFiles = @(
    (Join-Path $dataRoot 'mass_spec\basic_tof\00_tof_s_is_pos_cent-r001.mzML'),
    (Join-Path $dataRoot 'mass_spec\basic_tof\00_tof_s_is_pos_cent-r002.mzML'),
    (Join-Path $dataRoot 'mass_spec\basic_tof\00_tof_s_is_pos_cent-r003.mzML')
)
foreach ($path in $dataFiles) {
    if (-not (Test-Path $path -PathType Leaf)) { throw "Required fixture not found: $path" }
}

if (-not $SkipBuild) {
    cmake --build $buildRoot --target streamfind_mcp streamfind_service streamfind_worker -j 1
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dataStore
$logRoot = Join-Path $repoRoot 'tmp\logs'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$serviceLog = Join-Path $logRoot 'pipeline-mass-spec-service.stdout.log'
$serviceErrorLog = Join-Path $logRoot 'pipeline-mass-spec-service.stderr.log'
$serviceProcess = Start-Process -FilePath $serviceExecutable -ArgumentList '8791' -WorkingDirectory $buildRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $serviceLog -RedirectStandardError $serviceErrorLog
Start-Sleep -Milliseconds 500
$process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
try {
    Initialize-Mcp $process | Out-Null
    $endpoint = 'http://127.0.0.1:8791'
    $session = 'pipeline-mass-spec'
    $createBody = @{ mode = 'create'; session_id = $session; database_path = $database } |
        ConvertTo-Json -Depth 20
    Invoke-RestMethod -Uri "$endpoint/projects" -Method Post -ContentType 'application/json' -Body $createBody | Out-Null

    $workflow = @{
        name = 'Non-target analysis short pipeline mode check'
        schema_version = 2
        workflow_id = 'non-target-analysis-short-pipeline'
        version = 1
        operations = @(
            @{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = $dataFiles }; inputs = @{} }
            @{ id = 2; operation = 'mass_spec.set_replicate_names'; parameters = @{ replicate_names = @('sample', 'sample', 'sample') }; inputs = @{} }
            @{ id = 3; operation = 'mass_spec.set_blank_names'; parameters = @{ blank_names = @('no', 'no', 'no') }; inputs = @{} }
            @{ id = 4; operation = 'mass_spec.find_features'; parameters = @{
                    ppm_threshold = 10; noise_threshold = 500; min_snr = 3; min_traces = 3
                    baseline_window = 200; max_feature_width = 100; base_quantile = 0.99
                    debug_analysis = ''; debug_mz = 0; debug_spec_idx = -1
                    rt_windows = @(@{ rtmin = 0; rtmax = 3400 })
                }; inputs = @{} }
        )
        connections = @(
            @{ source_operation = 1; source_port = 'analysesTable'; target_operation = 2; target_port = 'analysesTable' }
            @{ source_operation = 2; source_port = 'analysesTable'; target_operation = 3; target_port = 'analysesTable' }
            @{ source_operation = 3; source_port = 'analysesTable'; target_operation = 4; target_port = 'analysesTable' }
            @{ source_operation = 1; source_port = 'spectraHeadersTable'; target_operation = 4; target_port = 'spectraHeadersTable' }
        )
    }
    $workflowBody = $workflow | ConvertTo-Json -Depth 30
    Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow" -Method Post -ContentType 'application/json' -Body $workflowBody | Out-Null
    $validationBody = @{ workflow = $workflow } | ConvertTo-Json -Depth 30
    $validation = Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow/validate" -Method Post -ContentType 'application/json' -Body $validationBody
    if (-not $validation.valid) { throw "Workflow validation failed: $($validation | ConvertTo-Json -Depth 20 -Compress)" }

    $run = Invoke-McpTool $process 7 'run_workflow' @{ database_path = $database; execution_mode = 'pipeline' }
    Write-Host ("Pipeline run accepted: " + ($run | ConvertTo-Json -Depth 20 -Compress))

    $stateUri = "$endpoint/projects/$([uri]::EscapeDataString($session))/workflow/state"
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    do {
        Start-Sleep -Seconds 2
        $state = Invoke-RestMethod -Uri $stateUri -Method Get
        Write-Host ("Workflow state: " + $state.state + " (" + $state.progress.completed + "/" + $state.progress.total + ")")
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Pipeline integration run timed out.' }
    } while ($state.state -in @('queued', 'running', 'cancelling'))
    if ($state.state -ne 'completed') {
        $events = Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow/events?limit=2000" -Method Get
        throw "Pipeline integration run ended in state '$($state.state)': $($state | ConvertTo-Json -Depth 20 -Compress); events=$($events | ConvertTo-Json -Depth 20 -Compress)"
    }

    $registryUri = "$endpoint/projects/$([uri]::EscapeDataString($session))/results"
    $registry = Invoke-RestMethod -Uri $registryUri -Method Get
    $results = @($registry.results)
    $terminal = @($results | Where-Object { $_.producer_instance -eq '4' -and $_.contract_id -eq 'featuresTable' })
    $intermediate = @($results | Where-Object { $_.producer_instance -in @('1', '2', '3') })
    if ($terminal.Count -ne 1) { throw "Expected one published featuresTable from operation 4, found $($terminal.Count)." }
    if ($intermediate.Count -ne 0) { throw "Pipeline published intermediate results: $($intermediate | ConvertTo-Json -Depth 10 -Compress)" }
    Write-Host ("Pipeline mode passed: one terminal featuresTable result, no intermediate published results. Result id: " + $terminal[0].result_id)
} finally {
    Stop-StreamfindMcp $process
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        $serviceProcess.Kill()
        $serviceProcess.WaitForExit()
    }
    if (-not $KeepProject) {
        Remove-Item -Force -ErrorAction SilentlyContinue $database
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dataStore
    }
}
