[CmdletBinding()]
param(
    [string]$Endpoint = 'http://127.0.0.1:8787',
    [switch]$Cleanup
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$databaseRoot = Join-Path $repoRoot 'tmp\projects'
New-Item -ItemType Directory -Force -Path $databaseRoot | Out-Null

$cases = @(
    [ordered]@{
        key = 'pac'
        source = 'E:\example_files\raw_vendor_files\sciex\201023_Pac.wiff'
        target_id = 'pac'
        target_name = 'Paclitaxel'
        quantifier_regex = '^SRM.*Q1=854\.233 Q3=569\.1.*$'
        qualifier_regex = '^SRM.*Q1=854\.233 Q3=285\.9.*$'
        quantifier_precursor = 854.232971
        quantifier_product = 569.099976
        qualifier_precursor = 854.232971
        qualifier_product = 285.899994
    },
    [ordered]@{
        key = 'if'
        source = 'E:\example_files\raw_vendor_files\sciex\201209_MM_2.wiff'
        target_id = 'if'
        target_name = 'Ifosfamide'
        quantifier_regex = '^SRM.*Q1=260\.958 Q3=153\.8.*$'
        qualifier_regex = '^SRM.*Q1=260\.958 Q3=92(?: |$).*'
        quantifier_precursor = 260.958008
        quantifier_product = 153.800003
        qualifier_precursor = 260.958008
        qualifier_product = 92.0
    }
)

function Invoke-Json {
    param([string]$Uri, [string]$Method = 'Get', [object]$Body)
    if ($null -eq $Body) { return Invoke-RestMethod -Uri $Uri -Method $Method }
    return Invoke-RestMethod -Uri $Uri -Method $Method -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Depth 60)
}

function Get-LatestResult {
    param([string]$Session, [string]$Contract)
    $inventory = (Invoke-Json "$Endpoint/projects/$Session/results").results
    return $inventory | Where-Object { $_.contract_id -eq $Contract -and $_.status -eq 'published' } |
        Sort-Object created_at -Descending | Select-Object -First 1
}

function Get-Rows {
    param([string]$Session, [string]$Contract, [int]$Limit = 10000)
    $result = Get-LatestResult $Session $Contract
    if ($null -eq $result) { throw "No published $Contract result for $Session" }
    return (Invoke-Json "$Endpoint/projects/$Session/results/query" 'Post' @{
        result_id = $result.result_id
        mode = 'page'
        limit = $Limit
    }).rows
}

function Get-SampleAssignment {
    param([string]$Analysis)
    $trimmed = $Analysis.Trim()
    if ($trimmed -match '(?i)(?:BLW|Blank)') {
        return [ordered]@{ analysis = $Analysis; target_id = $script:TargetId; sample_type = 'blank'; exclude = $false; dilution_factor = 1.0 }
    }
    if ($trimmed -match '(?i)QC\s+([0-9]+(?:\.[0-9]+)?)') {
        return [ordered]@{ analysis = $Analysis; target_id = $script:TargetId; sample_type = 'qc'; nominal_concentration = [double]$Matches[1]; exclude = $false; dilution_factor = 1.0 }
    }
    if ($trimmed -match '_(\d+(?:\.\d+)?)(?: \(\d+\))?$') {
        $concentration = [double]$Matches[1]
        return [ordered]@{ analysis = $Analysis; target_id = $script:TargetId; sample_type = 'calibrator'; nominal_concentration = $concentration; calibration_level = $script:Levels[[string]$concentration]; exclude = $false; dilution_factor = 1.0 }
    }
    return [ordered]@{ analysis = $Analysis; target_id = $script:TargetId; sample_type = 'unknown'; exclude = $false; dilution_factor = 1.0 }
}

foreach ($case in $cases) {
    if (-not (Test-Path -LiteralPath $case.source -PathType Leaf)) { throw "Missing WIFF fixture: $($case.source)" }
    $session = "quantification-sciex-$($case.key)"
    $database = Join-Path $databaseRoot "$session.duckdb"
    $dataStore = [IO.Path]::ChangeExtension($database, '.data')
    try { Invoke-Json "$Endpoint/projects/$session" 'Delete' | Out-Null } catch { }
    if (Test-Path -LiteralPath $database) { Remove-Item -LiteralPath $database -Force }
    if (Test-Path -LiteralPath $dataStore) { Remove-Item -LiteralPath $dataStore -Recurse -Force }
    Invoke-Json "$Endpoint/projects" 'Post' @{ mode = 'create'; session_id = $session; database_path = $database } | Out-Null

    $readWorkflow = [ordered]@{
        schema_version = 2
        operations = @(@{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = @($case.source) }; inputs = @{}; position = @{} })
        connections = @()
    }
    $readWorkflow.definition_revision = [int](Invoke-Json "$Endpoint/projects/$session/workflow").workflow.definition_revision
    Invoke-Json "$Endpoint/projects/$session/workflow" 'Post' $readWorkflow | Out-Null
    Invoke-Json "$Endpoint/projects/$session/workflow/run" 'Post' @{ execution_mode = 'detailed' } | Out-Null
    do { Start-Sleep -Milliseconds 500; $state = Invoke-Json "$Endpoint/projects/$session/workflow/state" } while ($state.state -in @('queued', 'running', 'cancelling'))
    if ($state.state -ne 'completed') { throw "Reader workflow failed for ${session}: $($state | ConvertTo-Json -Depth 8 -Compress)" }

    $analysisRows = @(Get-Rows $session 'analysesTable')
    $analysisNames = @($analysisRows | ForEach-Object { [string]$_.analysis } | Where-Object { $_ } | Select-Object -Unique)
    $concentrations = @($analysisNames | ForEach-Object {
        if ($_ -match '_(\d+(?:\.\d+)?)(?: \(\d+\))?$') { [double]$Matches[1] }
    } | Sort-Object -Unique)
    $script:Levels = @{}
    $level = 1
    foreach ($concentration in $concentrations) { $script:Levels[[string]$concentration] = $level++ }
    $script:TargetId = $case.target_id
    $assignments = @($analysisNames | ForEach-Object { Get-SampleAssignment $_ })

    $method = [ordered]@{
        target_id = $case.target_id
        name = $case.target_name
        role = 'analyte'
        response_metric = 'area'
        normalization = 'none'
        concentration_unit = 'ng'
        regression_model = 'linear'
        weighting = 'none'
        intercept_mode = 'free'
        quantifier_signal_type = 'MS'
        quantifier_chromatogram_type = 'SRM'
        quantifier_polarity = 1
        quantifier_precursor_mz = $case.quantifier_precursor
        quantifier_product_mz = $case.quantifier_product
        quantifier_detector = 'SCIEX'
        qualifier_signal_type = 'MS'
        qualifier_chromatogram_type = 'SRM'
        qualifier_polarity = 1
        qualifier_precursor_mz = $case.qualifier_precursor
        qualifier_product_mz = $case.qualifier_product
        qualifier_detector = 'SCIEX'
        enabled = $true
    }

    $workflow = [ordered]@{
        schema_version = 2
        metadata = [ordered]@{
            name = "SCIEX $($case.target_name) quantification diagnostic"
            description = 'Alternative no-internal-standard diagnostic using the same peak-finding settings as the NDMA workflow.'
        }
        operations = @(
            @{ id = 1; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = @($case.source) }; inputs = @{}; position = @{ x = 700; y = 650 } },
            @{ id = 2; operation = 'mass_spec.load_chromatograms'; parameters = @{ analysis_names = @(); chromatogram_id_regex = @($case.quantifier_regex, $case.qualifier_regex); invert = $false; ignore_case = $false }; inputs = @{}; position = @{ x = 1150; y = 550 } },
            @{ id = 3; operation = 'mass_spec.find_chromatogram_peaks'; parameters = @{ analysis_names = @(); indices = @(); merge = $true; merge_distance = 5; min_peak_height = 0; min_snr = 5; peak_trace_padding_seconds = 5 }; inputs = @{}; position = @{ x = 1550; y = 650 } },
            @{ id = 4; operation = 'mass_spec.set_quantification_plan'; parameters = @{ quantification_method = @($method); quant_sample_assignments = $assignments }; inputs = @{}; position = @{ x = 700; y = 1050 } },
            @{ id = 5; operation = 'mass_spec.quantify'; parameters = @{ minimum_calibration_points = 3; require_qualifier = $false; qc_max_relative_error = 20 }; inputs = @{}; position = @{ x = 1950; y = 850 } }
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
    $workflow.definition_revision = [int](Invoke-Json "$Endpoint/projects/$session/workflow").workflow.definition_revision
    $validation = Invoke-Json "$Endpoint/projects/$session/workflow/validate" 'Post' @{ workflow = $workflow }
    if (-not $validation.valid) { throw "Validation failed for ${session}: $($validation | ConvertTo-Json -Depth 10 -Compress)" }
    Invoke-Json "$Endpoint/projects/$session/workflow" 'Post' $workflow | Out-Null
    $exportPath = Join-Path $databaseRoot "$session.workflow.json"
    $workflow | ConvertTo-Json -Depth 60 | Set-Content -LiteralPath $exportPath -Encoding utf8
    Invoke-Json "$Endpoint/projects/$session/workflow/run" 'Post' @{ execution_mode = 'detailed' } | Out-Null
    Write-Output "Started $session with $($analysisNames.Count) analyses and $($assignments.Count) assignment rows"
}

$pending = @($cases | ForEach-Object { "quantification-sciex-$($_.key)" })
do {
    Start-Sleep -Seconds 2
    foreach ($session in @($pending)) {
        $state = Invoke-Json "$Endpoint/projects/$session/workflow/state"
        if ($state.state -notin @('queued', 'running', 'cancelling')) {
            Write-Output "$session state=$($state.state)"
            if ($state.state -ne 'completed') { throw "Workflow failed for ${session}: $($state | ConvertTo-Json -Depth 8 -Compress)" }
            $pending = @($pending | Where-Object { $_ -ne $session })
        }
    }
} while ($pending.Count -gt 0)

foreach ($case in $cases) {
    $session = "quantification-sciex-$($case.key)"
    $model = @(Get-Rows $session 'quantificationCalibrationModelsTable' 100)[0]
    $quantification = @(Get-Rows $session 'quantificationTable')
    $points = @($quantification | Where-Object { $_.sample_type -eq 'calibrator' -and $_.role -eq 'analyte' })
    $results = @($quantification | Where-Object { $_.role -eq 'analyte' })
    Write-Output "--- $session"
    $model | Select-Object target_id,fit_status,n_points,coefficient_0,coefficient_1,r_squared,range_min,range_max | ConvertTo-Json -Depth 4
    Write-Output ("points={0}; included={1}; excluded={2}; results={3}" -f $points.Count, @($points | Where-Object included).Count, @($points | Where-Object { -not $_.included }).Count, $results.Count)
    $results | Group-Object status | Select-Object Name,Count | ConvertTo-Json -Depth 4
    if ($Cleanup) {
        try { Invoke-Json "$Endpoint/projects/$session" 'Delete' | Out-Null } catch { }
        $database = Join-Path $databaseRoot "$session.duckdb"
        $dataStore = [IO.Path]::ChangeExtension($database, '.data')
        Remove-Item -LiteralPath $database -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $dataStore -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath ([IO.Path]::ChangeExtension($database, '.workflow.json')) -Force -ErrorAction SilentlyContinue
    }
}
