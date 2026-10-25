param(
    [switch]$KeepProject
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildRoot = Join-Path $repoRoot 'tmp\build\mingw-ucrt64'
$serviceExecutable = Join-Path $buildRoot 'streamfind-server.exe'
$database = 'E:\example_files\ms_merck\ACC1_27166_find_features_debug.duckdb'
$workflowPath = 'E:\example_files\ms_merck\streamfind_workflows\ACC1_27166_find_features_debug.json'
$summaryPath = 'E:\example_files\ms_merck\streamfind_workflows\ACC1_27166_find_features_debug.summary.json'
$session = 'acc1_27166_find_features_debug'
$endpoint = 'http://127.0.0.1:8793'
$sourcePaths = @(
    'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4_blank_P1-E-5_1_2022_11895.mzML',
    'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4_P1-F-1_1_2022_11896.mzML'
)
$expectedPath = 'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4.csv'

New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot 'tmp\logs') | Out-Null
$serviceProcess = Start-Process -FilePath $serviceExecutable -ArgumentList '8793' -WorkingDirectory $buildRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $repoRoot 'tmp\logs\find-features-debug.stdout.log') `
    -RedirectStandardError (Join-Path $repoRoot 'tmp\logs\find-features-debug.stderr.log')

function Invoke-JsonPost([string]$uri, [object]$body) {
    return Invoke-RestMethod -Uri $uri -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 50)
}

try {
    Start-Sleep -Milliseconds 700
    Remove-Item -Force -ErrorAction SilentlyContinue $database
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue ([IO.Path]::ChangeExtension($database, '.data'))
    Invoke-JsonPost "$endpoint/projects" @{mode='create';session_id=$session;database_path=$database} | Out-Null

    $baseParameters = @{ppm_threshold=15;noise_threshold=10;min_snr=2;min_traces=3;baseline_window=300;max_feature_width=200;base_quantile=0.1;rt_windows=@(@{rtmin=300;rtmax=3400});debug_analysis='';debug_spec_idx=-1;smoothing_enabled=$false;smoothing_window=7;smoothing_sharp_window=3;smoothing_strength=1.0}
    $expected = @(Import-Csv -LiteralPath $expectedPath)
    $ops = @(@{id=1;operation='mass_spec.read_mass_spec_files';parameters=@{source_paths=$sourcePaths};inputs=@{}})
    $connections = @()
    $debugOperationIds = @()
    $id = 2
    foreach ($target in $expected) {
        $parameters = $baseParameters.Clone()
        $parameters.debug_mz = [double]$target.mz
        $ops += @{id=$id;operation='mass_spec.find_features';parameters=$parameters;inputs=@{}}
        $connections += @{source_operation=1;source_port='analysesTable';target_operation=$id;target_port='analysesTable'}
        $connections += @{source_operation=1;source_port='spectraHeadersTable';target_operation=$id;target_port='spectraHeadersTable'}
        $debugOperationIds += $id
        $id++
    }
    $workflow = @{name='ACC1 27166 Find Features Debug Peaks';schema_version=2;workflow_id='acc1-27166-find-features-debug';version=1;operations=$ops;connections=$connections}
    $workflowJson = $workflow | ConvertTo-Json -Depth 50
    Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow" -Method Post -ContentType 'application/json' -Body $workflowJson | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path $workflowPath) | Out-Null
    Set-Content -LiteralPath $workflowPath -Value $workflowJson -Encoding UTF8

    Invoke-JsonPost "$endpoint/projects/$session/workflow/run" @{execution_mode='detailed'} | Out-Null
    $deadline = [DateTime]::UtcNow.AddMinutes(20)
    do {
        Start-Sleep -Seconds 3
        $state = Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow/state"
        Write-Host ("state={0} progress={1}/{2}" -f $state.state, $state.progress.completed, $state.progress.total)
    } while ($state.state -in @('queued','running','cancelling') -and [DateTime]::UtcNow -lt $deadline)
    if ($state.state -ne 'completed') { throw ("Workflow ended in state $($state.state).") }

    $resultRoot = Join-Path ([IO.Path]::ChangeExtension($database, '.data')) 'results'
    $rows = @()
    foreach ($operationId in $debugOperationIds) {
        $target = $expected[$operationId - 2]
        $manifest = Get-ChildItem -LiteralPath $resultRoot -Filter manifest.json -Recurse |
            ForEach-Object { Get-Content -LiteralPath $_.FullName | ConvertFrom-Json } |
            Where-Object {$_.producer_instance -eq [string]$operationId -and $_.contract_id -like '*findFeaturesDebugResult'} |
            Select-Object -First 1
        $log = ''
        if ($manifest) {
            $payloadPath = Join-Path ([IO.Path]::ChangeExtension($database, '.data')) $manifest.payload
            $payload = Get-Content -LiteralPath $payloadPath -Raw | ConvertFrom-Json
            $log = $payload.log_text
        }
        $rows += [ordered]@{operation_id=$operationId;expected_rt=[double]$target.rt;expected_mz=[double]$target.mz;formula=$target.formula;result_id=if($manifest){$manifest.result_id}else{''};debug_log=$log}
    }
    $rows | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    Write-Host "Project: $database"
    Write-Host "Workflow: $workflowPath"
    Write-Host "Summary: $summaryPath"
} finally {
    if ($serviceProcess -and -not $serviceProcess.HasExited) { $serviceProcess.Kill(); $serviceProcess.WaitForExit() }
    if (-not $KeepProject) {
        Remove-Item -Force -ErrorAction SilentlyContinue $database
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue ([IO.Path]::ChangeExtension($database, '.data'))
    }
}
