[CmdletBinding()]
param(
    [ValidateSet('Cpp')]
    [string]$Backend = 'Cpp',
    [switch]$RunPipeline,
    [switch]$SkipBuild,
    [ValidateRange(0, 1000)]
    [int]$MaxAnalyses = 0,
    [string]$StopAfter = '',
    [switch]$KeepProject
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dataRoot = Get-StreamfindDataRoot $repoRoot
$workflowDataRoot = Join-Path $dataRoot 'mass_spec\wastewater'
$workflowFiles = @(Get-ChildItem -LiteralPath $workflowDataRoot -File -Filter '*.mzML' | Sort-Object Name)
if ($workflowFiles.Count -eq 0) {
    throw "No wastewater mzML files found under $workflowDataRoot"
}
if ($MaxAnalyses -gt 0 -and $MaxAnalyses -lt $workflowFiles.Count) {
    $workflowFiles = @($workflowFiles | Select-Object -First $MaxAnalyses)
}
$analysisNames = @($workflowFiles | ForEach-Object { $_.BaseName })

$devDuckdb = Join-Path $repoRoot 'bindings\r\dev\dev_duckdb'
$internalStandardsPath = Join-Path $devDuckdb 'internal_standards_v3.csv'
$suspectsPath = Join-Path $devDuckdb 'suspects_with_ms2_template.csv'
foreach ($path in @($internalStandardsPath, $suspectsPath)) {
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Required NTA target table not found: $path"
    }
}

function Convert-FragmentPairs {
    param([AllowNull()][string]$Value)
    $mz = [System.Collections.Generic.List[double]]::new()
    $intensity = [System.Collections.Generic.List[double]]::new()
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return @{ mz = @(); intensity = @() }
    }
    foreach ($pair in $Value -split ';') {
        $parts = $pair.Trim() -split '\s+'
        if ($parts.Count -lt 2) { continue }
        $mz.Add([double]::Parse($parts[0], [Globalization.CultureInfo]::InvariantCulture))
        $intensity.Add([double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture))
    }
    return @{ mz = @($mz); intensity = @($intensity) }
}

function Convert-TargetRow {
    param([Parameter(Mandatory = $true)]$Row)
    $target = [ordered]@{}
    foreach ($name in @('name', 'formula', 'SMILES', 'InChI', 'InChIKey')) {
        $value = if ($Row.PSObject.Properties.Name -contains $name) { $Row.$name } else { $null }
        if (-not [string]::IsNullOrWhiteSpace($value)) { $target[$name] = $value }
    }
    $name = if ($Row.PSObject.Properties.Name -contains 'name') { $Row.name } else { $null }
    $mass = if ($Row.PSObject.Properties.Name -contains 'mass') { $Row.mass } else { $null }
    $rt = if ($Row.PSObject.Properties.Name -contains 'rt') { $Row.rt } else { $null }
    $xlogp = if ($Row.PSObject.Properties.Name -contains 'xLogP') { $Row.xLogP } else { $null }
    if (-not [string]::IsNullOrWhiteSpace($name)) { $target.id = $name }
    if (-not [string]::IsNullOrWhiteSpace($mass)) {
        $target.mass = [double]::Parse($mass, [Globalization.CultureInfo]::InvariantCulture)
    }
    if (-not [string]::IsNullOrWhiteSpace($rt)) {
        $target.rt = [double]::Parse($rt, [Globalization.CultureInfo]::InvariantCulture)
    }
    if (-not [string]::IsNullOrWhiteSpace($xlogp)) {
        $target.xLogP = [double]::Parse($xlogp, [Globalization.CultureInfo]::InvariantCulture)
    }
    foreach ($mode in @('positive', 'negative')) {
        $fragmentColumn = "ms2_$mode"
        $fragmentValue = if ($Row.PSObject.Properties.Name -contains $fragmentColumn) { $Row.$fragmentColumn } else { $null }
        $pairs = Convert-FragmentPairs $fragmentValue
        if ($pairs.mz.Count -gt 0) {
            $target["fragments_mz_$mode"] = $pairs.mz
            $target["fragments_intensity_$mode"] = $pairs.intensity
        }
    }
    return $target
}

$internalTargets = @(Import-Csv -LiteralPath $internalStandardsPath |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_.rt) } |
    ForEach-Object { Convert-TargetRow $_ })
$suspectTargets = @(Import-Csv -LiteralPath $suspectsPath |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_.mass) -or -not [string]::IsNullOrWhiteSpace($_.mz) } |
    ForEach-Object { Convert-TargetRow $_ })
if ($internalTargets.Count -eq 0) { throw 'No internal-standard targets were loaded' }
if ($suspectTargets.Count -eq 0) { throw 'No suspect targets were loaded' }

function Get-ReplicateLabel {
    param([Parameter(Mandatory = $true)][string]$Name)
    if ($Name -match '_is_(pos|neg)_(blank|influent|o3sw_effluent)-r\d+$') {
        $sample = if ($matches[2] -eq 'o3sw_effluent') { 'effluent' } else { $matches[2] }
        return "$($matches[1])_$sample"
    }
    throw "Cannot derive replicate label from analysis name: $Name"
}
$replicateNames = @($analysisNames | ForEach-Object { Get-ReplicateLabel $_ })
$blankNames = @($analysisNames | ForEach-Object {
    if ($_ -match '_is_(pos|neg)_') { "$($matches[1])_blank" } else { '' }
})

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
$database = Join-Path $repoRoot "tmp\projects\streamfind-$($Backend.ToLowerInvariant())-nta-script.duckdb"
$projectId = "nta-$($Backend.ToLowerInvariant())"
New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
$env:STREAMFIND_CATALOGUE = $catalogue
if ($Backend -eq 'Cpp') {
    $env:PATH = (Join-Path $repoRoot 'tmp\build\core-default\tests') + ';' + $env:PATH
}

$process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
$baseArguments = @{
    database_path = $database
}

function Invoke-NtaMethodWithDiagnostics {
    param(
        [Parameter(Mandatory = $true)][int]$Id,
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][hashtable]$Parameters,
        [Parameter(Mandatory = $true)][string]$DiagnosticTool
    )
    $arguments = @{}
    foreach ($entry in $baseArguments.GetEnumerator()) { $arguments[$entry.Key] = $entry.Value }
    $arguments.method = $Method
    $arguments.parameters = $Parameters
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "[$Method] starting"
    $result = Invoke-McpTool $process $Id 'run_method' $arguments
    $clock.Stop()
    Write-Host ("[$Method] completed in {0:N2}s: {1}" -f $clock.Elapsed.TotalSeconds, ($result | ConvertTo-Json -Compress -Depth 8))

    $diagnosticArguments = @{}
    foreach ($entry in $baseArguments.GetEnumerator()) { $diagnosticArguments[$entry.Key] = $entry.Value }
    $diagnosticArguments.analysis_names = $analysisNames
    $diagnostic = Invoke-McpTool $process ($Id + 100) $DiagnosticTool $diagnosticArguments
    if ($diagnostic.PSObject.Properties.Name -contains 'row_count') {
        Write-Host "[$Method] diagnostic $DiagnosticTool rows=$($diagnostic.row_count)"
    } else {
        Write-Host ("[$Method] diagnostic {0}: {1}" -f $DiagnosticTool, ($diagnostic | ConvertTo-Json -Compress -Depth 6))
    }
}

try {
    Initialize-Mcp $process | Out-Null
    Write-Host ("NTA workflow backend={0}; analyses={1}; internal_standards={2}; suspects={3}" -f $Backend, $analysisNames.Count, $internalTargets.Count, $suspectTargets.Count)

    Invoke-McpTool $process 2 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $added = Invoke-McpTool $process 3 'mass_spec.add_analyses' @{
        database_path = $database
        analyses = @($workflowFiles | ForEach-Object { @{ path = $_.FullName } })
    }
    if ([int]$added.row_count -ne $workflowFiles.Count) {
        throw "Expected $($workflowFiles.Count) imported analyses, received $($added.row_count)"
    }
    Write-Host "[setup] imported $($added.row_count) wastewater analyses"

    Invoke-McpTool $process 4 'mass_spec.set_replicate_names' @{
        database_path = $database
        replicate_names = $replicateNames
    } | Out-Null
    Invoke-McpTool $process 5 'mass_spec.set_blank_names' @{
        database_path = $database
        blank_names = $blankNames
    } | Out-Null
    # The current C++ backend installs module schemas through domain operations.
    Invoke-McpTool $process 5 'mass_spec.get_features' @{
        database_path = $database
        analysis_names = $analysisNames
    } | Out-Null
    Write-Host '[setup] replicate and blank labels assigned'

    if ($RunPipeline) {
        $steps = [System.Collections.Generic.List[object]]::new()
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.find_features'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; rt_windows_min = @(); rt_windows_max = @(); ppm_threshold = 10.0; noise_threshold = 250.0; min_snr = 3.0; min_traces = 3; baseline_window = 200.0; max_feature_width = 250.0; base_quantile = 0.99
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.load_features_ms1'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; filtered = $false; rt_window = @(-1.0, 1.0); mz_window = @(-1.0, 6.0); min_traces_intensity = 250.0; mz_clust = 0.008; presence = 0.5
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.load_features_ms2'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; filtered = $false; min_traces_intensity = 10.0; isolation_window = 1.3; mz_clust = 0.008; presence = 0.5
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.create_components'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; rt_window = @(-2.5, 2.5); min_correlation = 0.85
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.annotate_components'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; max_isotopes = 8; max_charge = 1; max_gaps = 1; ppm = 10.0; isotope_elements = @('C:1-80', 'N:0-10', 'O:0-20', 'S:0-4', 'Cl:0-6', 'Br:0-4')
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.find_internal_standards'; DiagnosticTool = 'mass_spec.get_internal_standards'; Parameters = @{
            analysis_names = $analysisNames; targets = $internalTargets; ppm = 10.0; sec = 15.0; ppm_ms2 = 10.0; mzr_ms2 = 0.008; min_cosine_similarity = 0.7; min_shared_fragments = 3; filtered = $true
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.group_features'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; method = 'internal_standards'; rt_deviation = 5.0; ppm = 10.0; min_samples = 1; bin_size = 5.0
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.fill_features'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; within_replicate = $false; filtered = $false; rt_expand = 10.0; mz_expand = 0.01; max_peak_width = 30.0; min_traces_intensity = 1000.0; min_number_traces = 5; min_intensity_ms1 = 5000.0; rt_apex_deviation = 5.0; min_signal_to_noise_ratio = 3.0; min_gaussian_fit = 0.2
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.correct_matrix_suppression'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; mp_rt_window = 10.0; ref_blank_replicate = ''
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.subtract_blank'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; blank_threshold = 5.0; rt_expand = 10.0; mz_expand = 0.005
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.filter_features'; DiagnosticTool = 'mass_spec.get_features'; Parameters = @{
            analysis_names = $analysisNames; min_intensity = 10000.0; remove_isotopes = $true; remove_adducts = $true; remove_losses = $true
        } })
        $steps.Add([pscustomobject]@{ Method = 'mass_spec.suspect_screening'; DiagnosticTool = 'mass_spec.get_suspects'; Parameters = @{
            analysis_names = $analysisNames; targets = $suspectTargets; ppm = 5.0; sec = 10.0; ppm_ms2 = 10.0; mzr_ms2 = 0.008; min_cosine_similarity = 0.7; min_shared_fragments = 3; filtered = $true
        } })

        $workflow = [ordered]@{
            name = 'scripts-dev-nta'
            version = 1
            domain = 'mass_spec'
            steps = @($steps | ForEach-Object { [ordered]@{ method = $_.Method; parameters = $_.Parameters } })
        }
        $workflowArguments = @{}
        foreach ($entry in $baseArguments.GetEnumerator()) { $workflowArguments[$entry.Key] = $entry.Value }
        if ($StopAfter) {
            $stopIndex = [array]::IndexOf(@($steps | ForEach-Object { $_.Method }), $StopAfter)
            if ($stopIndex -lt 0) { throw "StopAfter method not found in workflow: $StopAfter" }
            $workflow.steps = @($workflow.steps | Select-Object -First ($stopIndex + 1))
        }
        $workflowArguments.workflow = $workflow
        Invoke-McpTool $process 6 'set_workflow' $workflowArguments | Out-Null
        $plannedCount = @($workflow.steps).Count
        Write-Host "[setup] planned $plannedCount NTA workflow methods"
        $execution = Invoke-McpTool $process 10 'run_workflow' $baseArguments
        Write-Host ("[workflow] completed: " + ($execution | ConvertTo-Json -Compress -Depth 8))
        $id = 100
        foreach ($step in @($steps | Select-Object -First $plannedCount)) {
            $diagnosticArguments = @{}
            foreach ($entry in $baseArguments.GetEnumerator()) { $diagnosticArguments[$entry.Key] = $entry.Value }
            $diagnosticArguments.analysis_names = $analysisNames
            $diagnostic = Invoke-McpTool $process $id $step.DiagnosticTool $diagnosticArguments
            if ($diagnostic.PSObject.Properties.Name -contains 'row_count') {
                Write-Host "[$($step.Method)] diagnostic $($step.DiagnosticTool) rows=$($diagnostic.row_count)"
            }
            $id++
        }
        if ($StopAfter) { Write-Host "Stopped after $StopAfter." }
        else { Write-Host "$Backend full NTA workflow completed." }
    } else {
        Write-Host "$Backend NTA setup passed; use -RunPipeline to execute all 12 methods."
    }
} finally {
    Stop-StreamfindMcp $process
    if (-not $KeepProject) {
        Remove-Item -Force -ErrorAction SilentlyContinue $database
    }
}
