<# Run MCP startup, discovery, and a minimal operation against an extracted C++ package. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageRoot
)

. "$PSScriptRoot\..\..\dev\mcp-common.ps1"

$packageRoot = (Resolve-Path $PackageRoot -ErrorAction Stop).Path
$executable = Join-Path $packageRoot 'bin\streamfind_mcp_launcher.exe'
$coreExecutable = Join-Path $packageRoot 'bin\streamfind_mcp.exe'
$catalogue = Join-Path $packageRoot 'core\catalogue.duckdb'
$database = Join-Path $packageRoot '..\..\projects\packaged-mcp-smoke.duckdb'

foreach ($path in @($executable, $coreExecutable, $catalogue, (Join-Path $packageRoot 'bin\streamfind.json'), (Join-Path $packageRoot 'plugins\mass_spec\streamfind_mass_spec.dll'))) {
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Packaged MCP prerequisite is missing: $path"
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
$previousCatalogue = $env:STREAMFIND_CATALOGUE
$env:STREAMFIND_CATALOGUE = $null
$process = $null
try {
    $process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
    $initialized = Initialize-Mcp $process
    if ($initialized.result.serverInfo.name -ne 'streamfind-cpp') {
        throw "Packaged MCP reported unexpected server: $($initialized.result.serverInfo.name)"
    }

    $tools = Send-McpRequest $process @{
        jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{}
    }
    if ($tools.PSObject.Properties.Name -contains 'error' -and $null -ne $tools.error) {
        throw "Packaged tools/list failed: $($tools.error.message)"
    }
    $toolNames = @($tools.result.tools | ForEach-Object { $_.name })
    foreach ($requiredTool in @('create', 'describe', 'get_operations', 'run_operation')) {
        if ($toolNames -notcontains $requiredTool) {
            throw "Packaged MCP did not advertise required tool: $requiredTool"
        }
    }
    $massSpecOperations = Invoke-McpTool $process 11 'get_operations' @{
        domain = 'mass_spec'
        search = 'read_mass_spec_files'
    }
    if (-not @($massSpecOperations | Where-Object { $_.canonical_id -eq 'mass_spec.read_mass_spec_files' })) {
        throw 'Packaged MCP did not expose mass_spec.read_mass_spec_files through operation discovery'
    }

    Invoke-McpTool $process 3 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $description = Invoke-McpTool $process 4 'describe' @{
        database_path = $database
    }
    if ($null -eq $description) {
        throw 'Packaged project describe operation returned no result'
    }

    $workflowDefinition = @{
        schema_version = 1
        operations = @(
            @{
                id = 'read-1'
                operation = 'mass_spec.read_mass_spec_files'
                parameters = @{ source_paths = @('fixture.mzML') }
                inputs = @{}
                position = @{}
            },
            @{
                id = 'find-1'
                operation = 'mass_spec.find_features'
                parameters = @{}
                inputs = @{}
                position = @{}
            }
        )
        connections = @(
            @{
                source_operation = 'read-1'
                source_port = 'analysesTable'
                target_operation = 'find-1'
                target_port = 'analysesTable'
            }
        )
    }
    Invoke-McpTool $process 6 'set_workflow' @{
        database_path = $database
        workflow = $workflowDefinition
    } | Out-Null
    $workflow = Invoke-McpTool $process 8 'get_workflow' @{ database_path = $database }
    $validation = Invoke-McpTool $process 9 'validate_workflow' @{
        database_path = $database
        workflow = $workflow
    }
    if (-not $validation.valid) {
        throw 'Packaged workflow construction validation returned invalid'
    }
    $inventory = Invoke-McpTool $process 12 'get_artifact_inventory' @{ database_path = $database }
    if ([int]$inventory.Count -ne 0) {
        throw 'Newly constructed workflow unexpectedly has artifacts'
    }

    Write-Host ("Packaged MCP smoke passed: startup, tools/list, create, describe, workflow construction, and validation ({0} tools)." -f $toolNames.Count)
} finally {
    if ($null -ne $process) {
        Stop-StreamfindMcp $process
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $database
    if ($null -eq $previousCatalogue) {
        Remove-Item Env:STREAMFIND_CATALOGUE -ErrorAction SilentlyContinue
    } else {
        $env:STREAMFIND_CATALOGUE = $previousCatalogue
    }
}
