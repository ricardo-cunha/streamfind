[CmdletBinding()]
param(
    [string]$ProjectName = 'dev-impurities-acc1-27166',
    [string]$ServiceEndpoint = '',
    [string]$FixtureRoot = 'E:\example_files\ms_merck\More Data OLED Routine for Ricardo',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repoRoot 'scripts/dev/_lib/workflow-project-common.ps1')
$fixtureRoot = (Resolve-Path -LiteralPath $FixtureRoot).Path
$files = @{
    blank = Join-Path $fixtureRoot 'ACC1_27166_4_blank_P1-E-5_1_2022_11895.mzML'
    sample = Join-Path $fixtureRoot 'ACC1_27166_4_P1-F-1_1_2022_11896.mzML'
    targets = Join-Path $fixtureRoot 'ACC1_27166_4_targets.csv'
    mol = Join-Path $fixtureRoot 'ACC1_27166_4.mol'
}
foreach ($path in $files.Values) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required ACC1_27166_4 fixture is missing: $path" }
}

$workflowPath = Join-Path $repoRoot 'cpp/plugins/mass_spec/resources/workflows/hrms-compound-purity-estimation.json'
$workflow = Get-Content -LiteralPath $workflowPath -Raw | ConvertFrom-Json
$workflow.operations | Where-Object id -eq 1 | ForEach-Object { $_.parameters.source_paths = @($files.blank, $files.sample) }
$workflow.operations | Where-Object id -eq 2 | ForEach-Object { $_.parameters.replicate_names = @('blank', 'sample') }
$workflow.operations | Where-Object id -eq 3 | ForEach-Object { $_.parameters.blank_names = @('blank', 'blank') }
$workflow.operations | Where-Object id -eq 11 | ForEach-Object { $_.parameters.targets_csv_path = $files.targets }
$workflow.operations | Where-Object id -eq 14 | ForEach-Object {
    $_.parameters.compound_name = 'ACC1_27166_4'
    $_.parameters.mol_file_path = $files.mol
}
$workflow.metadata.name = 'ACC1_27166_4 impurity analysis (development)'
$workflow.metadata.description = 'Editable HRMS purity and MetFrag workflow using the ACC1_27166_4 Merck blank and sample files.'

New-StreamfindWorkflowProject -RepositoryRoot $repoRoot -ProjectName $ProjectName `
    -Workflow $workflow -Description $workflow.metadata.description -ServiceEndpoint $ServiceEndpoint -Force:$Force
