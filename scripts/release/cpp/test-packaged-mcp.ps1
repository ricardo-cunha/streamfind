<# Run MCP startup, discovery, and a minimal operation against an extracted C++ package. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageRoot
)

. "$PSScriptRoot\..\..\dev\mcp-common.ps1"

$packageRoot = (Resolve-Path $PackageRoot -ErrorAction Stop).Path
$executable = Join-Path $packageRoot 'bin\streamfind_mcp.exe'
$catalogue = Join-Path $packageRoot 'share\streamfind\catalogue.duckdb'
$database = Join-Path $packageRoot '..\..\projects\packaged-mcp-smoke.duckdb'

foreach ($path in @($executable, $catalogue, (Join-Path $packageRoot 'bin\streamfind.json'))) {
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
    foreach ($requiredTool in @('create', 'mass_spec.add_analyses', 'mass_spec.get_analysis_names')) {
        if ($toolNames -notcontains $requiredTool) {
            throw "Packaged MCP did not advertise required tool: $requiredTool"
        }
    }

    Invoke-McpTool $process 3 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $names = Invoke-McpTool $process 4 'mass_spec.get_analysis_names' @{
        database_path = $database
    }
    if ($null -eq $names) {
        throw 'Packaged MassSpec analysis-name operation returned no result'
    }

    Write-Host ("Packaged MCP smoke passed: startup, tools/list, create, and mass_spec.get_analysis_names ({0} tools)." -f $toolNames.Count)
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
