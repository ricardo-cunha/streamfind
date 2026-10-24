<# Run MCP startup, discovery, and a minimal operation against an extracted C++ package. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageRoot
)

. "$PSScriptRoot\..\..\dev\mcp-common.ps1"

$packageRoot = (Resolve-Path $PackageRoot -ErrorAction Stop).Path
$executable = Join-Path $packageRoot 'bin\streamfind-mcp.exe'
$coreExecutable = Join-Path $packageRoot 'bin\streamfind-mcp.exe'
$catalogue = Join-Path $packageRoot 'core\catalogue.duckdb'

foreach ($path in @($executable, $coreExecutable, $catalogue, (Join-Path $packageRoot 'bin\streamfind.json'), (Join-Path $packageRoot 'plugins\mass_spec\streamfind_mass_spec.dll'))) {
    if (-not (Test-Path $path -PathType Leaf)) {
        throw "Packaged MCP prerequisite is missing: $path"
    }
}

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

    Write-Host ("Packaged MCP smoke passed: direct startup, tools/list, and mass-spec discovery ({0} tools)." -f $toolNames.Count)
} finally {
    if ($null -ne $process) {
        Stop-StreamfindMcp $process
    }
    if ($null -eq $previousCatalogue) {
        Remove-Item Env:STREAMFIND_CATALOGUE -ErrorAction SilentlyContinue
    } else {
        $env:STREAMFIND_CATALOGUE = $previousCatalogue
    }
}
