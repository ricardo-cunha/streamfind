[CmdletBinding()]
param(
    [string]$ProjectName = 'dev-nta-wastewater',
    [string]$ServiceEndpoint = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repoRoot 'scripts/dev/_lib/workflow-project-common.ps1')

$dataRoot = Get-StreamfindDataRoot -RepositoryRoot $repoRoot
$wastewater = Join-Path $dataRoot 'mass_spec/wastewater'
$files = @(Get-ChildItem -LiteralPath $wastewater -File -Filter '*.mzML' | Sort-Object Name)
if ($files.Count -eq 0) { throw "No wastewater mzML files found under $wastewater" }
$required = @('internal_standards.csv', 'suspects.csv')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $wastewater $name) -PathType Leaf)) {
        throw "Required wastewater target file is missing: $(Join-Path $wastewater $name)"
    }
}

function Get-ReplicateLabel([string]$AnalysisName) {
    if ($AnalysisName -match '_is_(pos|neg)_(blank|influent|o3sw_effluent)-r\d+$') {
        $sample = if ($Matches[2] -eq 'o3sw_effluent') { 'effluent' } else { $Matches[2] }
        return "$($Matches[1])_$sample"
    }
    throw "Cannot derive wastewater replicate label from: $AnalysisName"
}

$workflowPath = Join-Path $repoRoot 'cpp/plugins/mass_spec/resources/workflows/non-target-screening-basic.json'
$workflow = Get-Content -LiteralPath $workflowPath -Raw | ConvertFrom-Json
$workflow.operations | Where-Object id -eq 1 | ForEach-Object { $_.parameters.source_paths = @($files.FullName) }
$workflow.operations | Where-Object id -eq 2 | ForEach-Object { $_.parameters.replicate_names = @($files | ForEach-Object { Get-ReplicateLabel $_.BaseName }) }
$workflow.operations | Where-Object id -eq 3 | ForEach-Object {
    $_.parameters.blank_names = @($files | ForEach-Object {
        if ($_.BaseName -match '_is_(pos|neg)_') { "$($Matches[1])_blank" } else { '' }
    })
}
$workflow.operations | Where-Object id -eq 13 | ForEach-Object { $_.parameters.suspect_targets_csv_path = Join-Path $wastewater 'internal_standards.csv' }
$workflow.operations | Where-Object id -eq 20 | ForEach-Object { $_.parameters.suspect_targets_csv_path = Join-Path $wastewater 'suspects.csv' }
$workflow.metadata.name = 'Wastewater non-target screening (development)'
$workflow.metadata.description = 'Editable NTA demo workflow using the shared wastewater data set and MetFrag screening.'

New-StreamfindWorkflowProject -RepositoryRoot $repoRoot -ProjectName $ProjectName `
    -Workflow $workflow -Description $workflow.metadata.description -ServiceEndpoint $ServiceEndpoint -Force:$Force
