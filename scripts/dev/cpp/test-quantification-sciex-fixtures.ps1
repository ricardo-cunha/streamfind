[CmdletBinding()]
param(
    [string[]]$InputPath = @(
        'E:\example_files\raw_vendor_files\sciex\201023_Pac.wiff',
        'E:\example_files\raw_vendor_files\sciex\201209_MM_2.wiff',
        'E:\example_files\raw_vendor_files\sciex\220511_Nitrosamine_M220426_M220503_Vierlinden.wiff'
    ),
    [switch]$SkipBuild,
    [switch]$KeepProjects,
    [switch]$KeepRuntime,
    [int]$RuntimePort = 8788,
    [int]$TimeoutMinutes = 0
)

. (Join-Path $PSScriptRoot '..\_lib\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$catalogue = Join-Path $repoRoot 'tmp\build\windows\core\semantic_catalogue\catalogue.duckdb'

if (-not $InputPath -or $InputPath.Count -eq 0) {
    throw 'At least one SCIEX .wiff path is required.'
}

$paths = @()
foreach ($path in $InputPath) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "SCIEX WIFF fixture not found: $path"
    }
    $resolved = (Resolve-Path -LiteralPath $path).Path
    if ([System.IO.Path]::GetExtension($resolved).ToLowerInvariant() -ne '.wiff') {
        throw "SCIEX fixture must be a .wiff file (the .wiff.scan companion is discovered automatically): $resolved"
    }
    $scanPath = [System.IO.Path]::ChangeExtension($resolved, '.wiff.scan')
    if (-not (Test-Path -LiteralPath $scanPath -PathType Leaf)) {
        throw "SCIEX WIFF scan companion not found: $scanPath"
    }
    $paths += $resolved
}

if (-not $SkipBuild) {
    cmake --build (Join-Path $repoRoot 'tmp\build\windows\core') --target streamfind_mcp streamfind_cli streamfind_tray streamfind_service streamfind_worker streamfind_mass_spec_plugin -j 1
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)" }
}

$buildRoot = Join-Path $repoRoot 'tmp\build\windows\core'
$env:STREAMFIND_CATALOGUE = $catalogue
$env:STREAMFIND_CORE_CATALOGUE = Join-Path $buildRoot 'semantic_catalogue\core\catalogue.duckdb'
$buildCli = Join-Path $buildRoot 'streamfind.exe'
$existingServices = @((& $buildCli service list --json 2>$null) -join "`n" | ConvertFrom-Json -ErrorAction SilentlyContinue)
if (@($existingServices | Where-Object { $_.health -eq 'healthy' }).Count -gt 0) {
    throw 'A streamfind service is already running. Refusing to start the fixture harness because streamfind-mcp would attach to that service; rerun after the service is stopped or use a separate machine/session.'
}
$runtimeRoot = Join-Path $repoRoot 'tmp\scratch\quantification-sciex-runtime'
$runtimePort = $RuntimePort
if ($runtimePort -lt 1024 -or $runtimePort -gt 65535) {
    throw "RuntimePort must be between 1024 and 65535: $runtimePort"
}
$runtimeListeners = @(Get-NetTCPConnection -LocalPort $runtimePort -State Listen -ErrorAction SilentlyContinue |
    Where-Object { $_.OwningProcess -ne 0 })
if ($runtimeListeners.Count -gt 0) {
    throw "Reserved worktree service port $runtimePort is already in use. Refusing to attach to an unknown service."
}
if (Test-Path -LiteralPath $runtimeRoot) { Remove-Item -LiteralPath $runtimeRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $buildRoot 'streamfind-mcp.exe') -Destination $runtimeRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'streamfind.exe') -Destination $runtimeRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'streamfind-worker.exe') -Destination $runtimeRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'streamfind-server.exe') -Destination $runtimeRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'streamfind-tray.exe') -Destination $runtimeRoot
Copy-Item -Path (Join-Path $buildRoot 'bin\*.dll') -Destination $runtimeRoot
Copy-Item -LiteralPath (Join-Path $buildRoot 'plugins') -Destination $runtimeRoot -Recurse
@{
    server = @{ port = $runtimePort; automatic_port_fallback = $false }
    worker = @{ memory_limit_mb = 0 }
    tray = @{ enabled = $false; start_with_app = $false; start_with_service = $false; status_interval_ms = 2000; notifications = $false }
    plugin_roots = @((Join-Path $runtimeRoot 'plugins'))
    enabled_plugins = @('mass_spec')
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $runtimeRoot 'streamfind.json') -Encoding utf8
$executable = Join-Path $runtimeRoot 'streamfind-mcp.exe'
$serviceCli = Join-Path $runtimeRoot 'streamfind.exe'
$env:PATH = $runtimeRoot + ';' + (Join-Path $buildRoot 'bin') + ';' + $env:PATH
$databaseRoot = Join-Path $repoRoot 'tmp\projects'
New-Item -ItemType Directory -Force -Path $databaseRoot | Out-Null
$summary = @()
$requestId = 1

function Invoke-ResultRows {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$Database,
        [Parameter(Mandatory = $true)][int]$OperationInstance,
        [Parameter(Mandatory = $true)][string[]]$Columns,
        [string]$OutputPort = ''
    )
    if (-not $OutputPort) { throw 'Invoke-ResultRows requires an explicit output port.' }
    $inventory = @(Invoke-McpTool $Process $script:requestId 'get_result_inventory' @{ project_path = $Database })
    $script:requestId++
    $candidate = @($inventory | Where-Object {
            [string]$_.producer_instance -eq [string]$OperationInstance -and
            [string]$_.contract_id -eq $OutputPort -and
            [string]$_.status -eq 'published'
        } | Sort-Object { [int]$_.workflow_revision } -Descending | Select-Object -First 1)
    if ($candidate.Count -ne 1) {
        throw "No unique published result found for operation $OperationInstance output $OutputPort. Inventory=$($inventory | ConvertTo-Json -Depth 6 -Compress)"
    }
    $selector = @{
        project_path = $Database
        result_id = [string]$candidate[0].result_id
        include_data = $true
        columns = $Columns
        limit = 10000
    }
    $result = Invoke-McpTool $Process $script:requestId 'request_result' $selector
    $script:requestId++
    $selected = @($result)
    if ($selected.Count -ne 1 -or $null -eq $selected[0].data) {
        throw "Expected one table result for operation $OperationInstance`: $($result | ConvertTo-Json -Depth 8 -Compress)"
    }
    return @($selected[0].data)
}

function Add-TransitionFields {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Target,
        [Parameter(Mandatory = $true)][object]$Header,
        [Parameter(Mandatory = $true)][string]$Prefix
    )
    $Target["${Prefix}_signal_type"] = [string]$Header.signal_type
    $Target["${Prefix}_chromatogram_type"] = [string]$Header.chromatogram_type
    if ($null -ne $Header.polarity -and [int]$Header.polarity -in @(-1, 1)) {
        $Target["${Prefix}_polarity"] = [int]$Header.polarity
    }
    if ([string]$Header.signal_type -eq 'MS') {
        $Target["${Prefix}_precursor_mz"] = [double]$Header.precursor_mz
        $Target["${Prefix}_product_mz"] = [double]$Header.product_mz
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$Header.detector)) {
        $Target["${Prefix}_detector"] = [string]$Header.detector
    }
    if ([string]$Header.signal_type -eq 'UV' -and [double]$Header.wavelength_nm -gt 0) {
        $Target["${Prefix}_wavelength_nm"] = [double]$Header.wavelength_nm
    }
}

function Wait-Workflow {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$Database,
        [Parameter(Mandatory = $true)][object]$Workflow
    )
    $started = [DateTime]::UtcNow
    $deadline = if ($TimeoutMinutes -gt 0) { $started.AddMinutes($TimeoutMinutes) } else { $null }
    $lastStep = -1
    $lastState = ''
    $stepStarted = $started
    do {
        Start-Sleep -Milliseconds 500
        $snapshot = Invoke-McpTool $Process $script:requestId 'get_workflow_execution' @{ project_path = $Database }
        $script:requestId++
        $state = [string]$snapshot.state
        $progress = $snapshot.progress
        $currentStep = if ($null -ne $progress -and $null -ne $progress.current_step) { [int]$progress.current_step } else { 0 }
        $completed = if ($null -ne $progress -and $null -ne $progress.completed) { [int]$progress.completed } else { 0 }
        $total = if ($null -ne $progress -and $null -ne $progress.total) { [int]$progress.total } else { 0 }
        if ($currentStep -ne $lastStep -or $state -ne $lastState) {
            $elapsed = ([DateTime]::UtcNow - $stepStarted).ToString('hh\:mm\:ss')
            if ($lastStep -ge 0) { Write-Host ("  node {0} elapsed {1}" -f $lastStep, $elapsed) }
            $operationName = @($Workflow.operations | Where-Object { [int]$_.id -eq $currentStep } | Select-Object -First 1 | ForEach-Object { $_.operation })
            if ($operationName.Count -eq 0) { $operationName = @('unknown') }
            Write-Host ("  workflow state={0}, node={1}/{2}, operation={3}, completed={4}, elapsed={5}" -f $state, $currentStep, $total, $operationName[0], $completed, (([DateTime]::UtcNow - $started).ToString('hh\:mm\:ss')))
            $lastStep = $currentStep
            $lastState = $state
            $stepStarted = [DateTime]::UtcNow
        }
        if ($null -ne $deadline -and [DateTime]::UtcNow -gt $deadline) { throw "SCIEX quantification fixture workflow timed out for $Database" }
    } while ($state -in @('queued', 'running', 'cancelling'))
    Write-Host ("  workflow finished state={0}, total elapsed={1}" -f $state, (([DateTime]::UtcNow - $started).ToString('hh\:mm\:ss')))
    if ($state -ne 'completed') {
        throw "SCIEX quantification fixture workflow ended in state '$state': $($snapshot | ConvertTo-Json -Depth 8 -Compress)"
    }
}

function Get-QuantificationAssignments {
    param(
        [Parameter(Mandatory = $true)][string[]]$AnalysisNames,
        [Parameter(Mandatory = $true)][string]$AnalyteId,
        [Parameter(Mandatory = $true)][string]$InternalStandardId,
        [double]$InternalStandardConcentration = 10.0
    )

    $concentrations = @{}
    foreach ($name in $AnalysisNames) {
        if ($name -match '(?i)wasser') { continue }
        if ($name -match '(?i)_qc(?:_|$)') { continue }
        if ($name -match '(?i)_m\d{6}_\d+$' -or $name -match '(?i)_sample_') { continue }
        if ($name -match '_(\d+(?:\.\d+)?)(?: \(\d+\))?$') {
            $value = [double]$Matches[1]
            $concentrations[[string]$value] = $value
        }
    }
    $levels = @{}
    $level = 1
    foreach ($value in ($concentrations.Values | Sort-Object)) {
        $levels[[string]$value] = $level++
    }

    $rows = @()
    foreach ($name in $AnalysisNames) {
        $isBlank = $name -match '(?i)wasser'
        $isQc = $name -match '(?i)_qc(?:_|$)'
        $isUnknown = $name -match '(?i)_m\d{6}_\d+$' -or $name -match '(?i)_sample_'
        $sampleType = 'unknown'
        $nominal = $null
        $calibrationLevel = $null
        if ($isBlank) { $sampleType = 'blank' }
        elseif ($isQc) { $sampleType = 'qc'; $nominal = 9.0 }
        elseif (-not $isUnknown -and $name -match '_(\d+(?:\.\d+)?)(?: \(\d+\))?$') {
            $nominal = [double]$Matches[1]
            $sampleType = 'calibrator'
            $calibrationLevel = $levels[[string]$nominal]
        }

        $analyte = [ordered]@{ analysis = $name; target_id = $AnalyteId; sample_type = $sampleType; exclude = $false; dilution_factor = 1.0 }
        $internalStandard = [ordered]@{ analysis = $name; target_id = $InternalStandardId; sample_type = $sampleType; exclude = $false; dilution_factor = 1.0 }
        $internalStandard.nominal_concentration = $InternalStandardConcentration
        if ($null -ne $nominal) {
            $analyte.nominal_concentration = $nominal
        }
        if ($null -ne $calibrationLevel) {
            $analyte.calibration_level = $calibrationLevel
            $internalStandard.calibration_level = $calibrationLevel
        }
        $rows += $analyte
        $rows += $internalStandard
    }
    return $rows
}

try {
    foreach ($sourcePath in $paths) {
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($sourcePath)
        $safeStem = ($stem -replace '[^A-Za-z0-9_-]', '_').ToLowerInvariant()
        if ($safeStem.Length -gt 16) {
            $safeStem = $safeStem.Substring(0, 16)
        }
        $database = Join-Path $databaseRoot "quantification-sciex-$safeStem.sf"
        $dataStore = Join-Path $database 'data'
        if (Test-Path -LiteralPath $database) { Remove-Item -LiteralPath $database -Recurse -Force }

        $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
        try {
        Initialize-Mcp $process | Out-Null
        Invoke-McpTool $process $script:requestId 'create' @{ project_path = $database; domain = 'mass_spec' } | Out-Null
        $script:requestId++
        $workflowRevision = (Invoke-McpTool $process $script:requestId 'get_workflow' @{ project_path = $database }).definition_revision
        $script:requestId++

        $readWorkflow = @{
            schema_version = 2
            operations = @(
                @{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = @($sourcePath) }; inputs = @{}; position = @{} }
            )
            connections = @()
        }
        $readWorkflow.definition_revision = [int]$workflowRevision
        Invoke-McpTool $process $script:requestId 'set_workflow' @{ project_path = $database; workflow = $readWorkflow } | Out-Null
        $script:requestId++
        $validation = Invoke-McpTool $process $script:requestId 'validate_workflow' @{ project_path = $database; workflow = $readWorkflow }
        $script:requestId++
        if (-not $validation.valid) { throw "Reader workflow validation failed for ${sourcePath}: $($validation | ConvertTo-Json -Depth 8 -Compress)" }
        Invoke-McpTool $process $script:requestId 'run_workflow' @{ project_path = $database } | Out-Null
        $script:requestId++
        Wait-Workflow $process $database $readWorkflow

        $analyses = Invoke-ResultRows $process $database 1 @('analysis') 'analysesTable'
        if ($analyses.Count -eq 0) { throw "SCIEX reader produced no analyses for $sourcePath" }
        $analysisNames = @($analyses | ForEach-Object { [string]$_.analysis } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
        if ($analysisNames.Count -eq 0) { throw "SCIEX analysesTable contained no analysis names for $sourcePath" }

        $headerRows = Invoke-ResultRows $process $database 1 @('analysis', 'index', 'chromatogram_id', 'polarity', 'precursor_mz', 'product_mz', 'signal_type', 'chromatogram_type', 'detector', 'wavelength_nm') 'chromatogramsHeadersTable'
        $usableHeaders = @($headerRows | Where-Object {
                ([string]$_.signal_type -eq 'MS' -and [double]$_.precursor_mz -gt 0 -and [double]$_.product_mz -gt 0) -or
                ([string]$_.signal_type -eq 'UV' -and [double]$_.wavelength_nm -gt 0)
            })
        if ($usableHeaders.Count -eq 0) { throw "SCIEX fixture has no valid MS/UV chromatogram transition headers: $sourcePath" }
        $analyteHeader = $usableHeaders[0]
        $internalStandardHeader = if ($usableHeaders.Count -gt 1) { $usableHeaders[1] } else { $usableHeaders[0] }
        $chromatogramPatterns = @(
            "^$([regex]::Escape([string]$analyteHeader.chromatogram_id))$"
        )
        if ([string]$internalStandardHeader.chromatogram_id -ne [string]$analyteHeader.chromatogram_id) {
            $chromatogramPatterns += "^$([regex]::Escape([string]$internalStandardHeader.chromatogram_id))$"
        }

        $analyteMethod = [ordered]@{
                target_id = 'fixture_analyte'
                name = 'Fixture analyte'
                role = 'analyte'
                internal_standard_id = 'fixture_internal_standard'
                response_metric = 'area'
                normalization = 'internal_standard'
                concentration_unit = 'unit'
                regression_model = 'linear'
                weighting = 'none'
                intercept_mode = 'free'
                quantifier_signal_type = 'MS'
                quantifier_chromatogram_type = 'MRM'
                quantifier_polarity = 1
                quantifier_precursor_mz = 300.0
                quantifier_product_mz = 200.0
                quantifier_detector = 'SCIEX'
                enabled = $true
            }
        Add-TransitionFields $analyteMethod $analyteHeader 'quantifier'
        if ($usableHeaders.Count -gt 1) { Add-TransitionFields $analyteMethod $internalStandardHeader 'qualifier' }
        $internalStandardMethod = [ordered]@{
                target_id = 'fixture_internal_standard'
                name = 'Fixture internal standard'
                role = 'internal_standard'
                response_metric = 'area'
                normalization = 'none'
                regression_model = 'linear'
                weighting = 'none'
                intercept_mode = 'free'
                quantifier_signal_type = 'MS'
                quantifier_chromatogram_type = 'MRM'
                quantifier_polarity = 1
                quantifier_precursor_mz = 301.0
                quantifier_product_mz = 201.0
                quantifier_detector = 'SCIEX'
                enabled = $true
            }
        Add-TransitionFields $internalStandardMethod $internalStandardHeader 'quantifier'
        $method = @($analyteMethod, $internalStandardMethod)
        $assignments = Get-QuantificationAssignments -AnalysisNames $analysisNames `
            -AnalyteId 'fixture_analyte' -InternalStandardId 'fixture_internal_standard'

        $workflow = @{
            schema_version = 2
            operations = @(
                @{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = @($sourcePath) }; inputs = @{}; position = @{} },
                @{ id = 2; operation = 'mass_spec.load_chromatograms'; parameters = @{ analysis_names = @(); chromatogram_id_regex = $chromatogramPatterns; invert = $false; ignore_case = $false }; inputs = @{}; position = @{} },
                @{ id = 3; operation = 'mass_spec.find_chromatogram_peaks'; parameters = @{ analysis_names = @(); indices = @(); min_snr = 0.0; min_peak_height = 0.0; peak_trace_padding_seconds = 5.0 }; inputs = @{}; position = @{} },
                @{ id = 4; operation = 'mass_spec.set_quantification_plan'; parameters = @{ quantification_method = $method; quant_sample_assignments = $assignments }; inputs = @{}; position = @{} },
                @{ id = 5; operation = 'mass_spec.quantify'; parameters = @{ minimum_calibration_points = 3; require_qualifier = $false; qc_max_relative_error = 20.0 }; inputs = @{}; position = @{} }
            )
            connections = @(
                @{ source_operation = 1; source_port = 'analysesTable'; target_operation = 2; target_port = 'analysesTable' },
                @{ source_operation = 1; source_port = 'chromatogramsHeadersTable'; target_operation = 2; target_port = 'chromatogramsHeadersTable' },
                @{ source_operation = 2; source_port = 'chromatogramsTable'; target_operation = 3; target_port = 'chromatogramsTable' },
                @{ source_operation = 1; source_port = 'analysesTable'; target_operation = 4; target_port = 'analysesTable' },
                @{ source_operation = 4; source_port = 'quantificationMethodTable'; target_operation = 5; target_port = 'quantificationMethodTable' },
                @{ source_operation = 4; source_port = 'quantificationAnalysesTable'; target_operation = 5; target_port = 'quantificationAnalysesTable' },
                @{ source_operation = 3; source_port = 'chromatogramPeaksTable'; target_operation = 5; target_port = 'chromatogramPeaksTable' }
            )
        }
        $workflowRevision = (Invoke-McpTool $process $script:requestId 'get_workflow' @{ project_path = $database }).definition_revision
        $script:requestId++
        $workflow.definition_revision = [int]$workflowRevision
        Invoke-McpTool $process $script:requestId 'set_workflow' @{ project_path = $database; workflow = $workflow } | Out-Null
        $script:requestId++
        $validation = Invoke-McpTool $process $script:requestId 'validate_workflow' @{ project_path = $database; workflow = $workflow }
        $script:requestId++
        if (-not $validation.valid) { throw "Quantification workflow validation failed for ${sourcePath}: $($validation | ConvertTo-Json -Depth 8 -Compress)" }
        Invoke-McpTool $process $script:requestId 'run_workflow' @{ project_path = $database } | Out-Null
        $script:requestId++
        Wait-Workflow $process $database $workflow

        $methodRows = Invoke-ResultRows $process $database 4 @('target_id', 'role', 'internal_standard_id', 'quantifier_precursor_mz', 'quantifier_product_mz') 'quantificationMethodTable'
        $assignmentRows = Invoke-ResultRows $process $database 4 @('analysis', 'target_id', 'sample_type') 'quantificationAnalysesTable'
        $responseRows = Invoke-ResultRows $process $database 5 @(
            'analysis', 'target_id', 'response', 'response_ratio', 'status', 'qualifier_status',
            'quantifier_chromatogram_id', 'qualifier_chromatogram_id', 'qualifier_chromatogram_index',
            'qualifier_peak_id', 'qualifier_peak_rt', 'qualifier_peak_area', 'qualifier_peak_height',
            'qualifier_rt_error', 'qualifier_candidate_count'
        ) 'quantificationTable'
        if ($methodRows.Count -ne 2) { throw "Expected two horizontal method rows for $sourcePath, found $($methodRows.Count)" }
        if ($assignmentRows.Count -ne ($analysisNames.Count * 2)) {
            throw "Expected $($analysisNames.Count * 2) assignment rows for $sourcePath, found $($assignmentRows.Count)"
        }
        if ($responseRows.Count -eq 0) {
            throw "Expected at least one quantification row for $sourcePath, found none"
        }
        $requiredResponseColumns = @(
            'quantifier_chromatogram_id', 'qualifier_chromatogram_id', 'qualifier_chromatogram_index',
            'qualifier_peak_id', 'qualifier_peak_rt', 'qualifier_peak_area', 'qualifier_peak_height',
            'qualifier_rt_error', 'qualifier_candidate_count'
        )
        $responseColumnNames = @($responseRows[0].PSObject.Properties.Name)
        foreach ($columnName in $requiredResponseColumns) {
            if ($responseColumnNames -notcontains $columnName) {
                throw "Quantification output for $sourcePath is missing provenance column '$columnName'"
            }
        }
        $summary += [pscustomobject]@{
            File = $sourcePath
            Analyses = $analysisNames.Count
            MethodRows = $methodRows.Count
            AssignmentRows = $assignmentRows.Count
            ResponseRows = $responseRows.Count
        }
        Write-Host "Quantification workflow fixture passed: $sourcePath ($($analysisNames.Count) analyses, $($assignmentRows.Count) assignments)"
        }
        finally {
            if (-not $KeepRuntime) { Stop-StreamfindMcp $process }
            if (-not $KeepProjects) {
                Remove-Item -LiteralPath $database -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

finally {
    if (-not $KeepRuntime) {
        $services = @((& $serviceCli service list --json 2>$null) -join "`n" | ConvertFrom-Json -ErrorAction SilentlyContinue)
        foreach ($service in $services) {
            if ([string]$service.endpoint -match ":$runtimePort(?:/|$)") {
                & $serviceCli service stop --instance ([string]$service.service_instance_id) | Out-Null
            }
        }
        $runtimeProcesses = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -like "${runtimeRoot}*" })
        foreach ($runtimeProcess in $runtimeProcesses) {
            Stop-Process -Id $runtimeProcess.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $runtimeRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host 'SCIEX quantification workflow fixture summary:'
$summary | Format-Table -AutoSize
