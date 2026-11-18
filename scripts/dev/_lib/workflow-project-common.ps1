Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'mcp-common.ps1')

function New-StreamfindWorkflowProject {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ProjectName,
        [Parameter(Mandatory = $true)]$Workflow,
        [Parameter(Mandatory = $true)][string]$Description,
        [string]$ServiceEndpoint = '',
        [switch]$Force
    )

    if ($ProjectName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw 'Project name may contain only letters, numbers, dot, underscore, and hyphen.'
    }
    $projectRoot = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'tmp/projects'))
    New-Item -ItemType Directory -Path $projectRoot -Force | Out-Null
    $container = [IO.Path]::GetFullPath((Join-Path $projectRoot "$ProjectName.sf"))
    $projectPrefix = $projectRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $container.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to create a project outside tmp/projects: $container"
    }
    $workflowSidecar = Join-Path $projectRoot "$ProjectName.workflow.json"
    if (Test-Path -LiteralPath $container) {
        if (-not $Force) { throw "Project already exists: $container. Choose another --project-name or pass --force." }
        $lockPath = Join-Path $container '.streamfind.lock'
        if (Test-Path -LiteralPath $lockPath) {
            try {
                $lockProbe = [IO.File]::Open($lockPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $lockProbe.Dispose()
            } catch {
                throw "Project is open in a StreamFind service and cannot be replaced: $container"
            }
        }
        $resolvedContainer = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $container).Path)
        $relativeContainer = [IO.Path]::GetRelativePath($projectRoot, $resolvedContainer)
        if ([IO.Path]::IsPathRooted($relativeContainer) -or $relativeContainer.StartsWith('..')) {
            throw "Refusing to remove a project outside tmp/projects: $resolvedContainer"
        }
        Remove-Item -LiteralPath $resolvedContainer -Recurse -Force
        if (Test-Path -LiteralPath $workflowSidecar) { Remove-Item -LiteralPath $workflowSidecar -Force }
    }

    $buildRoot = Join-Path $RepositoryRoot $(if ($IsWindows) { 'tmp/build/windows/core' } else { 'tmp/build/linux/core' })
    $catalogue = Join-Path $buildRoot 'semantic_catalogue/catalogue.duckdb'
    $coreCatalogue = Join-Path $buildRoot 'semantic_catalogue/core/catalogue.duckdb'
    $executable = Get-BackendMcpExecutable -RepositoryRoot $RepositoryRoot
    if (-not (Test-Path -LiteralPath $catalogue -PathType Leaf)) { throw "Native catalogue not found: $catalogue. Build the backend first." }
    if (-not (Test-Path -LiteralPath $coreCatalogue -PathType Leaf)) { throw "Core catalogue not found: $coreCatalogue. Build the backend first." }

    $previousCatalogue = $env:STREAMFIND_CATALOGUE
    $previousCoreCatalogue = $env:STREAMFIND_CORE_CATALOGUE
    $previousServiceEndpoint = $env:STREAMFIND_SERVICE_ENDPOINT
    $previousPath = $env:PATH
    $process = $null
    try {
        $env:STREAMFIND_CATALOGUE = $catalogue
        $env:STREAMFIND_CORE_CATALOGUE = $coreCatalogue
        if ($ServiceEndpoint) { $env:STREAMFIND_SERVICE_ENDPOINT = $ServiceEndpoint }
        $runtimePaths = @(@((Join-Path $buildRoot 'bin'), (Join-Path $buildRoot 'tests')) |
            Where-Object { Test-Path -LiteralPath $_ -PathType Container })
        if ($runtimePaths.Count -gt 0) {
            $env:PATH = ($runtimePaths -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH
        }
        $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
        Initialize-Mcp $process | Out-Null
        $script:workflowProjectRequestId = 2
        $metadata = @{ name = $ProjectName; description = $Description }
        Invoke-McpTool $process $script:workflowProjectRequestId 'create' @{
            project_path = $container
            metadata = @{ name = $ProjectName }
            workflow_metadata = $metadata
        } | Out-Null
        $script:workflowProjectRequestId++

        $current = Invoke-McpTool $process $script:workflowProjectRequestId 'get_workflow' @{ project_path = $container }
        $script:workflowProjectRequestId++
        $definitionRevision = [int]$current.definition_revision
        if ($Workflow.PSObject.Properties.Name -contains 'definition_revision') {
            $Workflow.definition_revision = $definitionRevision
        } else {
            $Workflow | Add-Member -MemberType NoteProperty -Name definition_revision -Value $definitionRevision
        }
        $validation = Invoke-McpTool $process $script:workflowProjectRequestId 'validate_workflow' @{
            project_path = $container
            workflow = $Workflow
        }
        $script:workflowProjectRequestId++
        if (-not $validation.valid) {
            throw "Workflow validation failed: $($validation | ConvertTo-Json -Depth 12 -Compress)"
        }
        Invoke-McpTool $process $script:workflowProjectRequestId 'set_workflow' @{
            project_path = $container
            workflow = $Workflow
        } | Out-Null

        $Workflow | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $workflowSidecar -Encoding utf8
        Write-Host "Created workflow project: $container"
        Write-Host "Workflow sidecar: $workflowSidecar"
        Write-Host "Operations: $(@($Workflow.operations).Count); validation: passed; execution: not started."
    } finally {
        if ($process) { Stop-StreamfindMcp $process }
        $env:STREAMFIND_CATALOGUE = $previousCatalogue
        $env:STREAMFIND_CORE_CATALOGUE = $previousCoreCatalogue
        $env:STREAMFIND_SERVICE_ENDPOINT = $previousServiceEndpoint
        $env:PATH = $previousPath
    }
}
