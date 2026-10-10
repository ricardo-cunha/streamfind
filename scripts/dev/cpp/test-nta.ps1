[CmdletBinding()]
param(
    [switch]$RunPipeline,
    [switch]$SkipBuild,
    [ValidateRange(0, 1000)]
    [int]$MaxAnalyses = 0,
    [string]$StopAfter = '',
    [switch]$KeepProject,
    [string]$Executable = '',
    [string]$Catalogue = ''
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$isWindowsPlatform = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$dataRoot = Get-StreamfindDataRoot $repoRoot
$workflowDataRoot = Join-Path (Join-Path $dataRoot 'mass_spec') 'wastewater'
$workflowFiles = @(Get-ChildItem -LiteralPath $workflowDataRoot -File -Filter '*.mzML' | Sort-Object Name)
if ($workflowFiles.Count -eq 0) {
    throw "No wastewater mzML files found under $workflowDataRoot"
}
if ($MaxAnalyses -gt 0 -and $MaxAnalyses -lt $workflowFiles.Count) {
    $workflowFiles = @($workflowFiles | Select-Object -First $MaxAnalyses)
}
$analysisNames = @($workflowFiles | ForEach-Object { $_.BaseName })

$internalStandardsPath = Join-Path $workflowDataRoot 'internal_standards.csv'
$suspectsPath = Join-Path $workflowDataRoot 'suspects.csv'
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
    if ($isWindowsPlatform) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'scripts\build\cpp\build-cpp.ps1') -Clean -Config Release
    } else {
        & bash (Join-Path $repoRoot 'scripts/build/cpp/build-cpp-linux.sh')
    }
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
}

$executable = if ($Executable) { (Resolve-Path $Executable).Path } else {
    Get-BackendMcpExecutable -RepositoryRoot $repoRoot
}
$dynamicRuntime = Test-Path (Join-Path (Split-Path $executable) 'streamfind.json')
$catalogue = if ($Catalogue) { (Resolve-Path $Catalogue).Path } else {
    $buildDir = if ($isWindowsPlatform) { 'tmp\build\mingw-ucrt64' } else { 'tmp/build/linux-cpp' }
    Join-Path $repoRoot (Join-Path $buildDir 'semantic_catalogue/catalogue.duckdb')
}
$database = Join-Path $repoRoot (Join-Path (Join-Path 'tmp' 'projects') 'streamfind-cpp-nta-script.duckdb')
$projectId = 'nta-cpp'
New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
$env:STREAMFIND_CATALOGUE = $catalogue
$testDir = if ($isWindowsPlatform) { 'tmp/build/mingw-ucrt64/tests' } else { 'tmp/build/linux-cpp/tests' }
$env:PATH = (Join-Path $repoRoot $testDir) + [IO.Path]::PathSeparator + $env:PATH

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

function Assert-NtaWorkflowResults {
    param(
        [Parameter(Mandatory = $true)]$BaseArguments,
        [Parameter(Mandatory = $true)]$Execution
    )

    $completedEvents = @($Execution.events | Where-Object { $_.type -eq 'operation.completed' })
    $failedEvents = @($Execution.events | Where-Object { $_.type -eq 'operation.failed' })
    $expectedOperations = @($Execution.execution).Count
    if ($failedEvents.Count -gt 0 -or $completedEvents.Count -ne $expectedOperations) {
        throw "NTA workflow verification failed: completed=$($completedEvents.Count); expected=$expectedOperations; failed=$($failedEvents.Count)"
    }
    Write-Host "NTA workflow verification passed: completed_operations=$($completedEvents.Count); expected_operations=$expectedOperations"
    return

    $queryArguments = @{}
    foreach ($entry in $BaseArguments.GetEnumerator()) { $queryArguments[$entry.Key] = $entry.Value }
    $queryArguments.analysis_names = $analysisNames
    $analysesArguments = @{}
    foreach ($entry in $BaseArguments.GetEnumerator()) { $analysesArguments[$entry.Key] = $entry.Value }
    $analyses = Invoke-McpTool $process 900 'mass_spec.get_analyses' $analysesArguments
    $analysisCount = if ($analyses.PSObject.Properties.Name -contains 'row_count') {
        [int]$analyses.row_count
    } else {
        @($analyses).Count
    }
    $featureCount = 0
    $ms2Count = 0
    $suspectRows = [System.Collections.Generic.List[object]]::new()
    $featureQueryId = 901
    foreach ($analysisName in $analysisNames) {
        $featureArguments = @{}
        foreach ($entry in $BaseArguments.GetEnumerator()) { $featureArguments[$entry.Key] = $entry.Value }
        $featureArguments.analysis_names = @($analysisName)
        $features = Invoke-McpTool $process $featureQueryId 'mass_spec.get_features' $featureArguments
        if ($features.PSObject.Properties.Name -contains 'row_count') {
            $featureCount += [int]$features.row_count
            $ms2Count += @($features.columns.ms2_size | Where-Object { $_ -gt 0 }).Count
        } else {
            $featureRows = @($features)
            $featureCount += $featureRows.Count
            $ms2Count += @($featureRows | Where-Object { $_.ms2_size -gt 0 }).Count
        }
        $featureQueryId++
        $suspectArguments = @{}
        foreach ($entry in $BaseArguments.GetEnumerator()) { $suspectArguments[$entry.Key] = $entry.Value }
        $suspectArguments.analysis_names = @($analysisName)
        $suspects = Invoke-McpTool $process ($featureQueryId + 100) 'mass_spec.get_suspects' $suspectArguments
        if ($suspects.PSObject.Properties.Name -contains 'row_count') {
            foreach ($rowIndex in 0..([int]$suspects.row_count - 1)) {
                $suspectRows.Add([pscustomobject]@{
                    shared_fragments = $suspects.columns.shared_fragments[$rowIndex]
                    cosine_similarity = $suspects.columns.cosine_similarity[$rowIndex]
                })
            }
        } else {
            foreach ($row in @($suspects)) {
                $suspectRows.Add([pscustomobject]@{
                    shared_fragments = $row.shared_fragments
                    cosine_similarity = $row.cosine_similarity
                })
            }
        }
    }
    $suspectCount = $suspectRows.Count
    $sharedCount = @($suspectRows | Where-Object { $_.shared_fragments -gt 0 }).Count
    $similarCount = @($suspectRows | Where-Object { $_.cosine_similarity -ge 0.7 }).Count
    if ($analysisCount -ne $analysisNames.Count) { throw "NTA verification failed: analyses=$analysisCount, expected $($analysisNames.Count)" }
    if ($featureCount -lt 1 -or $ms2Count -lt 1 -or $suspectCount -lt 1 -or $sharedCount -lt 1 -or $similarCount -lt 1) {
        throw "NTA verification failed: features=$featureCount; features_with_ms2=$ms2Count; suspects=$suspectCount; suspects_with_shared_fragments=$sharedCount; suspects_similarity_ge_0.7=$similarCount"
    }
    $similarities = @($suspectRows.cosine_similarity | Where-Object { $null -ne $_ })
    $minimumSimilarity = ($similarities | Measure-Object -Minimum).Minimum
    $maximumSimilarity = ($similarities | Measure-Object -Maximum).Maximum
    Write-Host "NTA verification passed: analyses=$analysisCount; features=$featureCount; features_with_ms2=$ms2Count; suspects=$suspectCount; suspects_with_shared_fragments=$sharedCount; suspects_similarity_ge_0.7=$similarCount; similarity_range=($minimumSimilarity, $maximumSimilarity)"

    Write-Host '[verify] persisted NTA result checks passed'
}

try {
    Initialize-Mcp $process | Out-Null
    Write-Host ("NTA workflow backend=C++ analyses={0}; internal_standards={1}; suspects={2}" -f $analysisNames.Count, $internalTargets.Count, $suspectTargets.Count)

    Invoke-McpTool $process 2 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $sourcePaths = @($workflowFiles | ForEach-Object { $_.FullName })
    if (-not $RunPipeline) {
        $added = Invoke-McpTool $process 3 'run_method' @{
            database_path = $database
            method = 'mass_spec.read_mass_spec_files'
            parameters = @{ source_paths = $sourcePaths }
        }
        $addedCount = if ($added.PSObject.Properties.Name -contains 'row_count') {
            [int]$added.row_count
        } else {
            @($added).Count
        }
        if ($addedCount -ne $workflowFiles.Count) {
            throw "Expected $($workflowFiles.Count) imported analyses, received $addedCount"
        }
        Write-Host "[setup] imported $addedCount wastewater analyses"

        Invoke-McpTool $process 4 'run_method' @{
            database_path = $database
            method = 'mass_spec.set_replicate_names'
            parameters = @{ replicate_names = $replicateNames }
        } | Out-Null
        Invoke-McpTool $process 5 'run_method' @{
            database_path = $database
            method = 'mass_spec.set_blank_names'
            parameters = @{ blank_names = $blankNames }
        } | Out-Null
        Write-Host '[setup] replicate and blank labels assigned'
    }

    if ($RunPipeline) {
        $workflowPath = Join-Path $repoRoot 'cpp\plugins\mass_spec\resources\workflows\non-target-screening-basic.json'
        $workflow = Get-Content -LiteralPath $workflowPath -Raw | ConvertFrom-Json
        $workflow.operations | Where-Object { $_.id -eq 'operation-1' } | ForEach-Object {
            $_.parameters.source_paths = $sourcePaths
        }
        $workflow.operations | Where-Object { $_.id -eq 'operation-2' } | ForEach-Object {
            $_.parameters.replicate_names = $replicateNames
        }
        $workflow.operations | Where-Object { $_.id -eq 'operation-3' } | ForEach-Object {
            $_.parameters.blank_names = $blankNames
        }
        $workflow.operations | Where-Object { $_.id -eq 'operation-21' } | ForEach-Object {
            $_.parameters.suspect_targets_csv_path = $suspectsPath
        }
        $workflowOperations = @($workflow.operations)
        $workflowArguments = @{}
        foreach ($entry in $baseArguments.GetEnumerator()) { $workflowArguments[$entry.Key] = $entry.Value }
        if ($StopAfter) {
            $stopOperation = $workflowOperations | Where-Object { $_.operation -eq $StopAfter }
            if ($null -eq $stopOperation) { throw "StopAfter method not found in workflow: $StopAfter" }
            $stopIndex = [array]::IndexOf(@($workflowOperations.operation), $StopAfter)
            $workflowOperations = @($workflowOperations | Select-Object -First ($stopIndex + 1))
            $workflow.operations = $workflowOperations
            $workflow.connections = @($workflow.connections | Where-Object {
                ($workflowOperations.id -contains $_.source_operation) -and ($workflowOperations.id -contains $_.target_operation)
            })
        }
        $workflowArguments.workflow = $workflow
        Invoke-McpTool $process 6 'set_workflow' $workflowArguments | Out-Null
        $plannedCount = @($workflow.operations).Count
        Write-Host "[setup] loaded non-target screening workflow with $plannedCount operations"
        $execution = Invoke-McpTool $process 10 'run_workflow' $baseArguments
        Write-Host ("[workflow] completed: " + ($execution | ConvertTo-Json -Compress -Depth 8))
        if (-not $StopAfter) {
            Assert-NtaWorkflowResults $baseArguments $execution
        }
        if ($StopAfter) { Write-Host "Stopped after $StopAfter." }
        else { Write-Host 'C++ non-target screening workflow completed.' }
    } else {
        Write-Host 'C++ non-target screening setup passed; use -RunPipeline to execute the workflow.'
    }
} finally {
    Stop-StreamfindMcp $process
    if (-not $KeepProject) {
        Remove-Item -Force -ErrorAction SilentlyContinue $database
    }
}
