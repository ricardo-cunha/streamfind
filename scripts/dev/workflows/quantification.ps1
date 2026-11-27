[CmdletBinding()]
param(
    [string]$ProjectName = 'dev-quantification-sciex-mix1',
    [string]$ServiceEndpoint = '',
    [string]$TemplatePath = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repoRoot 'scripts/dev/_lib/workflow-project-common.ps1')
if (-not $TemplatePath) { $TemplatePath = Join-Path $PSScriptRoot 'resources/quantification-sciex-mix1.workflow.json' }
$templatePath = (Resolve-Path -LiteralPath $TemplatePath).Path
$workflow = Get-Content -LiteralPath $templatePath -Raw | ConvertFrom-Json
$workflow.metadata.name = 'SCIEX Mix1 quantification (development)'
$workflow.metadata.description = 'Editable quantification workflow based on the quantification-sciex-mix1 development project.'

New-StreamfindWorkflowProject -RepositoryRoot $repoRoot -ProjectName $ProjectName `
    -Workflow $workflow -Description $workflow.metadata.description -ServiceEndpoint $ServiceEndpoint -Force:$Force
