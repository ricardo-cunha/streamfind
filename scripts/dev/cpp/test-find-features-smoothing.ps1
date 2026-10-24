[CmdletBinding()]
param(
    [switch]$KeepProject
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildRoot = Join-Path $repoRoot 'tmp\build\mingw-ucrt64'
$serviceExecutable = Join-Path $buildRoot 'streamfind-service.exe'
$database = 'E:\example_files\ms_merck\ACC1_27166_smoothing_comparison.duckdb'
$workflowPath = 'E:\example_files\ms_merck\streamfind_workflows\ACC1_27166_find_features_smoothing_comparison.json'
$session = 'acc1_27166_smoothing_comparison'
$endpoint = 'http://127.0.0.1:8792'
$sourcePaths = @(
    'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4_blank_P1-E-5_1_2022_11895.mzML',
    'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4_P1-F-1_1_2022_11896.mzML'
)
$matchingRtToleranceSeconds = 120

$logRoot = Join-Path $repoRoot 'tmp\logs'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$serviceProcess = Start-Process -FilePath $serviceExecutable -ArgumentList '8792' -WorkingDirectory $buildRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $logRoot 'find-features-smoothing.stdout.log') `
    -RedirectStandardError (Join-Path $logRoot 'find-features-smoothing.stderr.log')

function Invoke-JsonPost([string]$uri, [object]$body) {
    return Invoke-RestMethod -Uri $uri -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 40)
}

try {
    Start-Sleep -Milliseconds 700
    Remove-Item -Force -ErrorAction SilentlyContinue $database
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue ([IO.Path]::ChangeExtension($database, '.data'))
    Invoke-JsonPost "$endpoint/projects" @{mode='create';session_id=$session;database_path=$database} | Out-Null

    # Match the ACC1 Merck workflows (ACC1_27803/ACC1_27989): only the
    # smoothing controls differ between the comparison branches.
    $baseParameters = @{ppm_threshold=15;noise_threshold=10;min_snr=2;min_traces=3;baseline_window=300;max_feature_width=200;base_quantile=0.1;rt_windows=@(@{rtmin=300;rtmax=3400});debug_analysis='';debug_mz=0;debug_spec_idx=-1}
    $parameters = @(
        @{smoothing_enabled=$true;smoothing_window=7;smoothing_sharp_window=3;smoothing_strength=1.0},
        @{smoothing_enabled=$false;smoothing_window=7;smoothing_sharp_window=3;smoothing_strength=1.0},
        @{smoothing_enabled=$true;smoothing_window=5;smoothing_sharp_window=3;smoothing_strength=0.5}
    )
    $ops = @(
        @{id=1;operation='mass_spec.read_mass_spec_files';parameters=@{source_paths=$sourcePaths};inputs=@{}},
        @{id=2;operation='mass_spec.find_features';parameters=($baseParameters + $parameters[0]);inputs=@{}},
        @{id=3;operation='mass_spec.find_features';parameters=($baseParameters + $parameters[1]);inputs=@{}},
        @{id=4;operation='mass_spec.find_features';parameters=($baseParameters + $parameters[2]);inputs=@{}}
    )
    $connections = @()
    foreach ($id in 2..4) {
        $connections += @{source_operation=1;source_port='analysesTable';target_operation=$id;target_port='analysesTable'}
        $connections += @{source_operation=1;source_port='spectraHeadersTable';target_operation=$id;target_port='spectraHeadersTable'}
    }
    $workflow = @{name='ACC1 27166 Find Features Smoothing Comparison';schema_version=2;workflow_id='acc1-27166-find-features-smoothing-comparison';version=1;operations=$ops;connections=$connections}
    $workflowJson = $workflow | ConvertTo-Json -Depth 40
    Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow" -Method Post -ContentType 'application/json' -Body $workflowJson | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path $workflowPath) | Out-Null
    Set-Content -LiteralPath $workflowPath -Value $workflowJson -Encoding UTF8

    Invoke-JsonPost "$endpoint/projects/$session/workflow/run" @{execution_mode='detailed'} | Out-Null
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    do {
        Start-Sleep -Seconds 2
        $state = Invoke-RestMethod -Uri "$endpoint/projects/$session/workflow/state"
        Write-Host ("state={0} progress={1}/{2}" -f $state.state, $state.progress.completed, $state.progress.total)
    } while ($state.state -in @('queued','running','cancelling') -and [DateTime]::UtcNow -lt $deadline)
    if ($state.state -ne 'completed') { throw ("Workflow ended in state $($state.state).") }

    $expectedPath = 'E:\example_files\ms_merck\More Data OLED Routine for Ricardo\ACC1_27166_4.csv'
    $expected = Import-Csv -LiteralPath $expectedPath
    $results = (Invoke-RestMethod -Uri "$endpoint/projects/$session/results").results
    $inventoryPath = 'E:\example_files\ms_merck\streamfind_workflows\ACC1_27166_find_features_smoothing_comparison.inventory.json'
    $results | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $inventoryPath -Encoding UTF8
    $featureResults = @($results | Where-Object {$_.contract_id -eq 'featuresTable' -and $_.status -eq 'published' -and $_.result_id} | Sort-Object producer_instance)
    $summary = @()
    foreach ($result in $featureResults) {
        $data = Invoke-JsonPost "$endpoint/projects/$session/results/query" @{result_id=$result.result_id;mode='detail';offset=0;limit=65536}
        $rows = @($data.rows)
        $matched = 0
        $matchedTargets = @()
        $missingTargets = @()
        foreach ($target in $expected) {
            $targetRt = [double]$target.rt
            $targetMz = [double]$target.mz
            if (@($rows | Where-Object {[math]::Abs([double]$_.rt - $targetRt) -le $matchingRtToleranceSeconds -and [math]::Abs([double]$_.mz - $targetMz) -le ($targetMz * 15 / 1000000)}).Count -gt 0) {
                $matched++
                $matchedTargets += [ordered]@{rt=$targetRt;mz=$targetMz;formula=$target.formula}
            } else {
                $missingTargets += [ordered]@{rt=$targetRt;mz=$targetMz;formula=$target.formula}
            }
        }
        $operation = $ops | Where-Object { $_.id -eq [int]$result.producer_instance } | Select-Object -First 1
        $rawSmoothedEqual = $null
        if ($operation.parameters.smoothing_enabled -eq $false) {
            $rawSmoothedEqual = (@($rows | Where-Object {$_.eic_intensity -ne $_.eic_smoothed}).Count -eq 0)
        }
        $summary += [ordered]@{producer_instance=$result.producer_instance;result_id=$result.result_id;matching_rt_tolerance_seconds=$matchingRtToleranceSeconds;feature_rows=$rows.Count;expected_peaks=$expected.Count;matched_expected_peaks=$matched;matched_targets=$matchedTargets;missing_targets=$missingTargets;raw_eic_equals_smoothed=$rawSmoothedEqual;settings=$operation.parameters}
    }
    $summaryPath = 'E:\example_files\ms_merck\streamfind_workflows\ACC1_27166_find_features_smoothing_comparison.summary.json'
    $summary | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary | Format-Table -AutoSize
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
