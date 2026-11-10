[CmdletBinding()]
param(
    [string]$SourceProject = '',
    [int]$Port = 18793
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if ([string]::IsNullOrWhiteSpace($SourceProject)) {
    $SourceProject = Join-Path $repoRoot 'tmp\projects\wastewater-nta-progress.duckdb'
}
$sourceDb = (Resolve-Path -LiteralPath $SourceProject).Path
$sourceData = [IO.Path]::ChangeExtension($sourceDb, '.data')
$workflowPath = [IO.Path]::ChangeExtension($sourceDb, '.workflow.json')
foreach ($requiredPath in @($sourceData, $workflowPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required wastewater project sidecar not found: $requiredPath"
    }
}

$workflow = Get-Content -LiteralPath $workflowPath -Raw | ConvertFrom-Json
$blankNode = $workflow.operations | Where-Object { $_.operation -eq 'mass_spec.subtract_blank' } | Select-Object -First 1
if (-not $blankNode) { throw 'The source workflow has no mass_spec.subtract_blank operation.' }
$readNode = $workflow.operations | Where-Object { $_.operation -eq 'mass_spec.read_mass_spec_files' } | Select-Object -First 1
if (-not $readNode) { throw 'The source workflow has no mass_spec.read_mass_spec_files operation.' }
foreach ($inputPath in $readNode.parameters.source_paths) {
    if (-not (Test-Path -LiteralPath $inputPath)) {
        throw "Wastewater source file from the workflow is unavailable: $inputPath"
    }
}

$projectsRoot = Join-Path $repoRoot 'tmp\projects'
$logsRoot = Join-Path $repoRoot 'tmp\logs'
New-Item -ItemType Directory -Force -Path $projectsRoot, $logsRoot | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$projectName = "phase6-wastewater-blank-check-$stamp-$([guid]::NewGuid().ToString('N').Substring(0, 6))"
$destinationDb = Join-Path $projectsRoot "$projectName.duckdb"
$destinationData = [IO.Path]::ChangeExtension($destinationDb, '.data')
if ((Test-Path -LiteralPath $destinationDb) -or (Test-Path -LiteralPath $destinationData)) {
    throw "Validation project destination already exists; refusing to overwrite it: $destinationDb"
}
Copy-Item -LiteralPath $sourceDb -Destination $destinationDb
Copy-Item -LiteralPath $sourceData -Destination $destinationData -Recurse
$storePath = Join-Path $destinationData 'store.json'
$store = Get-Content -LiteralPath $storePath -Raw | ConvertFrom-Json
$store.database_filename = [IO.Path]::GetFileName($destinationDb)
$store.store_id = 'store-phase6-' + [guid]::NewGuid().ToString('N')
$store | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $storePath -Encoding UTF8

$buildRoot = Join-Path $repoRoot 'tmp\build\windows\core'
$serviceExecutable = Join-Path $buildRoot 'streamfind-server.exe'
if (-not (Test-Path -LiteralPath $serviceExecutable)) {
    throw "Build the Windows service first; executable not found: $serviceExecutable"
}
$endpoint = "http://127.0.0.1:$Port"
$session = "phase6-wastewater-$stamp"
$stdout = Join-Path $logsRoot "$projectName-service.stdout.log"
$stderr = Join-Path $logsRoot "$projectName-service.stderr.log"
$serviceProcess = $null
$projectOpened = $false

function Invoke-JsonPost([string]$Uri, [object]$Body) {
    return Invoke-RestMethod -Uri $Uri -Method Post -ContentType 'application/json' `
        -Body ($Body | ConvertTo-Json -Depth 40) -TimeoutSec 60
}

try {
    $serviceProcess = Start-Process -FilePath $serviceExecutable -ArgumentList ([string]$Port) `
        -WorkingDirectory $buildRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $healthy = $false
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        Start-Sleep -Milliseconds 250
        try {
            $null = Invoke-RestMethod -Uri "$endpoint/health" -TimeoutSec 2
            $healthy = $true
            break
        }
        catch {
            if ($serviceProcess.HasExited) { throw "Validation service exited; see $stderr" }
        }
    }
    if (-not $healthy) { throw "Validation service did not become healthy; see $stderr" }

    $null = Invoke-JsonPost "$endpoint/projects" @{
        mode = 'open'
        session_id = $session
        database_path = $destinationDb
    }
    $projectOpened = $true
    $null = Invoke-JsonPost "$endpoint/projects/$session/results/cache/clear" @{}
    $null = Invoke-JsonPost "$endpoint/projects/$session/results/node/clear" @{ producer_instance = [string]$blankNode.id }

    $accepted = Invoke-JsonPost "$endpoint/projects/$session/operations/$($blankNode.operation)/run" @{
        operation_instance = [string]$blankNode.id
        parameters = $blankNode.parameters
    }
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    do {
        Start-Sleep -Seconds 1
        $state = Invoke-RestMethod -Uri "$endpoint/projects/$session/operations/$($blankNode.id)/state" -TimeoutSec 15
    } while ($state.state -in @('queued', 'starting', 'running', 'cancelling') -and [DateTime]::UtcNow -lt $deadline)
    if ($state.state -ne 'completed') {
        throw "subtract_blank ended in state '$($state.state)': $($state | ConvertTo-Json -Depth 12 -Compress)"
    }

    $inventory = Invoke-RestMethod -Uri "$endpoint/projects/$session/results" -TimeoutSec 60
    $results = @($inventory.results)
    $inputInstance = [string](@($workflow.connections | Where-Object {
        $_.target_operation -eq $blankNode.id -and $_.target_port -eq 'featuresTable'
    } | Select-Object -First 1).source_operation)
    $inputResult = $results | Where-Object {
        $_.contract_id -eq 'featuresTable' -and $_.producer_instance -eq $inputInstance -and $_.status -eq 'published'
    } | Select-Object -First 1
    $outputResult = $results | Where-Object {
        $_.contract_id -eq 'featuresTable' -and $_.producer_instance -eq [string]$blankNode.id -and $_.status -eq 'published'
    } | Select-Object -First 1
    if (-not $inputResult -or -not $outputResult) {
        throw 'Could not resolve the input and output featuresTable results for subtract_blank.'
    }

    $columns = @('analysis', 'feature', 'blank', 'replicate', 'intensity', 'filtered', 'filter')
    $inputPage = Invoke-JsonPost "$endpoint/projects/$session/results/query" @{
        result_id = $inputResult.result_id; mode = 'page'; offset = 0; limit = 250000; columns = $columns
    }
    $outputPage = Invoke-JsonPost "$endpoint/projects/$session/results/query" @{
        result_id = $outputResult.result_id; mode = 'page'; offset = 0; limit = 250000; columns = $columns
    }
    $inputRows = @($inputPage.rows)
    $outputRows = @($outputPage.rows)
    $inputByKey = @{}
    foreach ($row in $inputRows) { $inputByKey[([string]$row.analysis + '|' + [string]$row.feature)] = $row }
    $outputByKey = @{}
    foreach ($row in $outputRows) { $outputByKey[([string]$row.analysis + '|' + [string]$row.feature)] = $row }

    $missingRows = 0
    $newBlankFiltered = 0
    $blankReasonCount = 0
    $preservedReasons = 0
    $overwrittenReasons = 0
    foreach ($key in $inputByKey.Keys) {
        if (-not $outputByKey.ContainsKey($key)) { $missingRows++; continue }
        $before = $inputByKey[$key]
        $after = $outputByKey[$key]
        $reasonTokens = ([string]$after.filter) -split '\s+'
        if ($reasonTokens -contains 'blank_subtraction') { $blankReasonCount++ }
        if (-not [bool]$before.filtered -and [bool]$after.filtered -and $reasonTokens -contains 'blank_subtraction') {
            $newBlankFiltered++
        }
        $priorReason = [string]$before.filter
        if (-not [string]::IsNullOrWhiteSpace($priorReason)) {
            if ([string]$after.filter -like "*$priorReason*") { $preservedReasons++ } else { $overwrittenReasons++ }
        }
    }

    $summary = [ordered]@{
        project = $destinationDb
        source_project = $sourceDb
        workflow_node = $blankNode.id
        operation = $blankNode.operation
        state = $state.state
        input_result_id = $inputResult.result_id
        output_result_id = $outputResult.result_id
        input_rows = $inputRows.Count
        output_rows = $outputRows.Count
        missing_output_rows = $missingRows
        input_filtered_rows = @($inputRows | Where-Object { [bool]$_.filtered }).Count
        output_filtered_rows = @($outputRows | Where-Object { [bool]$_.filtered }).Count
        rows_with_blank_subtraction_reason = $blankReasonCount
        newly_filtered_by_blank = $newBlankFiltered
        prior_nonempty_reasons_preserved = $preservedReasons
        prior_nonempty_reasons_overwritten = $overwrittenReasons
        accepted = $accepted
        operation_state = $state
    }
    $summaryPath = Join-Path $logsRoot "$projectName-summary.json"
    $summary | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary | ConvertTo-Json -Depth 6
    Write-Output "Summary: $summaryPath"
}
finally {
    if ($projectOpened) {
        try { Invoke-RestMethod -Uri "$endpoint/projects/$session" -Method Delete -TimeoutSec 10 | Out-Null }
        catch {}
    }
    if ($serviceProcess -and -not $serviceProcess.HasExited) {
        $serviceProcess.Kill()
        $serviceProcess.WaitForExit()
    }
}
