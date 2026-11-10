[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$KeepProject,
    [switch]$PackagedBuild,
    [string]$ProjectName = 'pipeline-mass-spec'
)

. (Join-Path $PSScriptRoot '..\_lib\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dataRoot = Get-StreamfindDataRoot $repoRoot
$catalogue = if ($PackagedBuild) {
    Join-Path $repoRoot 'tmp\packaged_build\core\catalogue.duckdb'
} else {
    Join-Path $repoRoot 'tmp\build\windows\core\semantic_catalogue\catalogue.duckdb'
}
$buildRoot = if ($PackagedBuild) {
    Join-Path $repoRoot 'tmp\packaged_build\bin'
} else {
    Join-Path $repoRoot 'tmp\build\windows\core'
}
$executable = if ($PackagedBuild) {
    Join-Path $buildRoot 'streamfind-mcp.exe'
} else {
    Get-BackendMcpExecutable $repoRoot
}
$serviceExecutable = Join-Path $buildRoot 'streamfind-server.exe'
$database = Join-Path $repoRoot "tmp\projects\$ProjectName.duckdb"
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
    if ($PackagedBuild) { throw 'Packaged-build mode uses the existing package; pass -SkipBuild.' }
    cmake --build $buildRoot --target streamfind_mcp streamfind_service streamfind_worker -j 1
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
if ($PackagedBuild -and (Test-Path $database)) {
    throw "Refusing to replace an existing packaged test project: $database"
}
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
    if ($KeepProject) {
        $workflowExport = Join-Path (Split-Path $database -Parent) "$ProjectName.workflow.json"
        @{ workflow = $workflow } | ConvertTo-Json -Depth 30 |
            Set-Content -LiteralPath $workflowExport -Encoding utf8
        Write-Host "Saved editable workflow JSON: $workflowExport"
    }
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
    Write-Host ("Published result inventory: " + (($results | ForEach-Object {
        "$($_.producer_instance):$($_.contract_id):$($_.result_id):$($_.status)"
    }) -join '; '))
    $terminal = @($results | Where-Object { $_.producer_instance -eq '4' -and $_.contract_id -eq 'featuresTable' })
    $intermediate = @($results | Where-Object { $_.producer_instance -in @('1', '2', '3') })
    if ($terminal.Count -ne 1) { throw "Expected one published featuresTable from operation 4, found $($terminal.Count)." }
    if ($intermediate.Count -eq 0) { throw 'Pipeline mode did not retain any intermediate results for inspection.' }
    $publishedNodes = @($results | Select-Object -ExpandProperty producer_instance -Unique)
    if (@($publishedNodes | Where-Object { $_ -in @('1', '2', '3', '4') }).Count -ne 4) {
        throw "Pipeline mode did not retain results from every completed node: $($publishedNodes -join ', ')"
    }
    $manifestFiles = @(Get-ChildItem (Join-Path $dataStore 'results') -Filter 'manifest.json' -Recurse -File)
    if ($manifestFiles.Count -ne $results.Count) {
        throw "Pipeline result inventory has $($results.Count) entries but the result store has $($manifestFiles.Count) manifests."
    }
    $queryBody = @{
        result_id = $terminal[0].result_id
        mode = 'page'
        offset = 0
        limit = 25
        columns = @('analysis', 'feature', 'rt', 'mz')
    } | ConvertTo-Json -Depth 10
    $queryTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $page = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/query" `
        -Method Post -ContentType 'application/json' -Body $queryBody
    $queryTimer.Stop()
    if (@($page.rows).Count -ne 25 -or $page.total_rows -ne $terminal[0].row_count) {
        throw "Result page query returned an invalid slice: $($page | ConvertTo-Json -Depth 10 -Compress)"
    }
    Write-Host ("Terminal result page returned 25/$($page.total_rows) rows in $($queryTimer.ElapsedMilliseconds) ms.")

    $filteredQuery = @{
        result_id = $terminal[0].result_id
        mode = 'page'
        offset = 0
        limit = 25
        columns = @('analysis', 'feature', 'rt', 'mz')
        filters = @(
            @{ column = 'analysis'; operator = 'eq'; value = $page.rows[0].analysis }
            @{ column = 'rt'; operator = 'gt'; value = 0 }
        )
    } | ConvertTo-Json -Depth 10
    $filterTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $filteredPage = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/query" `
        -Method Post -ContentType 'application/json' -Body $filteredQuery
    $filterTimer.Stop()
    $filteredRows = @($filteredPage.rows)
    if ($filteredRows.Count -ne 25 -or $filteredPage.total_rows -lt $filteredRows.Count -or
        @($filteredRows | Where-Object { $_.analysis -ne $page.rows[0].analysis }).Count -ne 0 -or
        @($filteredRows | Where-Object { [double]$_.rt -le 0 }).Count -ne 0) {
        throw "Filtered result query returned an invalid page: $($filteredPage | ConvertTo-Json -Depth 10 -Compress)"
    }
    Write-Host ("Native string/numeric Arrow filters returned 25/$($filteredPage.total_rows) rows in $($filterTimer.ElapsedMilliseconds) ms.")

    $sampleQuery = @{
        result_id = $terminal[0].result_id
        mode = 'sample'
        limit = 1
        columns = @('analysis', 'feature')
    } | ConvertTo-Json -Depth 10
    $sample = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/query" `
        -Method Post -ContentType 'application/json' -Body $sampleQuery
    if (@($sample.rows).Count -ne 1) { throw 'Navigation-index seed query did not return one feature row.' }
    $sampleAnalysis = $sample.rows[0].analysis
    $featureTraceBody = @{
        result_id = $terminal[0].result_id
        trace_type = 'eic'
        limit = 1
        filters = @(
            @{ column = 'analysis'; operator = 'eq'; value = $sampleAnalysis }
            @{ column = 'feature'; operator = 'eq'; value = $sample.rows[0].feature }
        )
    } | ConvertTo-Json -Depth 10
    $traceTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $featureTrace = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/trace" `
        -Method Post -ContentType 'application/json' -Body $featureTraceBody
    $traceTimer.Stop()
    if (@($featureTrace.traces).Count -ne 1 -or
        -not $featureTrace.traces[0].eic_rt -or -not $featureTrace.traces[0].eic_intensity -or
        @($featureTrace.traces[0].eic_rt).Count -ne @($featureTrace.traces[0].eic_intensity).Count) {
        throw "Single-feature EIC query returned invalid paired axes: $($featureTrace | ConvertTo-Json -Depth 8 -Compress)"
    }
    $warmTraceTimer = [System.Diagnostics.Stopwatch]::StartNew()
    for ($traceAttempt = 0; $traceAttempt -lt 5; $traceAttempt++) {
        $warmTrace = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/trace" `
            -Method Post -ContentType 'application/json' -Body $featureTraceBody
        if (@($warmTrace.traces).Count -ne 1) { throw 'Warm single-feature EIC query returned an invalid result.' }
    }
    $warmTraceTimer.Stop()
    Write-Host ("Single-feature EIC: cold " + $traceTimer.ElapsedMilliseconds + " ms; warm average " +
        [math]::Round($warmTraceTimer.ElapsedMilliseconds / 5.0, 2) + " ms; points " +
        @($featureTrace.traces[0].eic_rt).Count + ".")

    # Restart the service to prove that the derived index is persisted and
    # validated independently of the previous process memory cache.
    if (-not $serviceProcess.HasExited) { $serviceProcess.Kill(); $serviceProcess.WaitForExit() }
    $restartStdout = Join-Path $logRoot 'pipeline-mass-spec-service-restart.stdout.log'
    $restartStderr = Join-Path $logRoot 'pipeline-mass-spec-service-restart.stderr.log'
    $serviceProcess = Start-Process -FilePath $serviceExecutable -ArgumentList '8791' -WorkingDirectory $buildRoot `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $restartStdout -RedirectStandardError $restartStderr
    Start-Sleep -Milliseconds 500
    $reopenBody = @{ mode = 'open'; session_id = $session; database_path = $database } | ConvertTo-Json -Depth 10
    Invoke-RestMethod -Uri "$endpoint/projects" -Method Post -ContentType 'application/json' -Body $reopenBody | Out-Null
    $reopenedTrace = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/trace" `
        -Method Post -ContentType 'application/json' -Body $featureTraceBody
    if (@($reopenedTrace.traces).Count -ne 1 -or
        @($reopenedTrace.traces[0].eic_rt).Count -ne @($reopenedTrace.traces[0].eic_intensity).Count) {
        throw 'Selected EIC could not be reconstructed with paired axes after service restart.'
    }
    Write-Host ("Selected EIC was reconstructed after service restart with " +
        @($reopenedTrace.traces[0].eic_rt).Count + " paired points.")
    $indexedQuery = @{
        result_id = $terminal[0].result_id
        mode = 'page'
        limit = 10
        columns = @('analysis', 'feature', 'rt')
        filters = @(@{ column = 'analysis'; operator = 'eq'; value = $sampleAnalysis })
    } | ConvertTo-Json -Depth 10
    $indexed = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/query" `
        -Method Post -ContentType 'application/json' -Body $indexedQuery
    if (@($indexed.rows).Count -eq 0 -or @($indexed.rows | Where-Object { $_.analysis -ne $sampleAnalysis }).Count -ne 0) {
        throw "Persisted navigation index query failed after service restart: $($indexed | ConvertTo-Json -Depth 10 -Compress)"
    }
    Write-Host ("Persisted equality index returned $(@($indexed.rows).Count)/$($indexed.total_rows) rows after service restart.")
    $rangeQuery = @{
        result_id = $terminal[0].result_id
        mode = 'page'
        limit = 25
        columns = @('analysis', 'feature', 'rt')
        filters = @(@{ column = 'rt'; operator = 'between'; min = 1000; max = 1100 })
    } | ConvertTo-Json -Depth 10
    $rangePage = Invoke-RestMethod -Uri "$endpoint/projects/$([uri]::EscapeDataString($session))/results/query" `
        -Method Post -ContentType 'application/json' -Body $rangeQuery
    if ($null -eq $rangePage.query_stats -or
        @($rangePage.rows | Where-Object { [double]$_.rt -lt 1000 -or [double]$_.rt -gt 1100 }).Count -ne 0) {
        throw "Persisted range statistics query returned invalid rows or omitted scan diagnostics: $($rangePage | ConvertTo-Json -Depth 10 -Compress)"
    }
    Write-Host ("Range query scanned " + $rangePage.query_stats.arrow_batches_scanned +
        " and skipped " + $rangePage.query_stats.arrow_batches_skipped + " Arrow batches after service restart.")
    Write-Host ("Pipeline mode retained results from all four nodes, including " +
        "$($intermediate.Count) intermediate outputs. Terminal result id: " + $terminal[0].result_id)
} finally {
    Stop-StreamfindMcp $process
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        $serviceProcess.Kill()
        $serviceProcess.WaitForExit()
    }
    if (-not $KeepProject) {
        Remove-Item -Force -ErrorAction SilentlyContinue $database
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dataStore
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path (Split-Path $database -Parent) "$ProjectName.workflow.json")
    } else {
        Write-Host "Kept editable test project: $database"
        Write-Host "Result store: $dataStore"
    }
}
