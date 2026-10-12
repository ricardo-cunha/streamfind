[CmdletBinding()]
param(
    [ValidateSet('Shimadzu', 'Sciex', 'AgilentChemstation', 'AgilentMassHunter', 'Thermo')]
    [string]$Vendor = 'Shimadzu',
    [string]$InputPath = '',
    [switch]$SkipBuild
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$vendorRoot = Get-StreamfindVendorRoot
$catalogue = Join-Path $repoRoot 'tmp\build\mingw-ucrt64\semantic_catalogue\catalogue.duckdb'

if (-not $InputPath) {
    switch ($Vendor) {
        'Shimadzu' {
            $InputPath = Join-Path $vendorRoot 'shimadzu\karl.lcd'
        }
        'Sciex' {
            $InputPath = (Get-ChildItem -LiteralPath (Join-Path $vendorRoot 'sciex') -Recurse -File -Filter '*.wiff' | Select-Object -First 1).FullName
        }
        'AgilentChemstation' {
            $InputPath = (Get-ChildItem -LiteralPath (Join-Path $vendorRoot 'agilent_chemstation') -Recurse -Directory -Filter '*.D' | Select-Object -First 1).FullName
        }
        'AgilentMassHunter' {
            $InputPath = (Get-ChildItem -LiteralPath (Join-Path $vendorRoot 'agilent_mass_hunter') -Recurse -Directory -Filter '*.d' | Select-Object -First 1).FullName
        }
        'Thermo' {
            $InputPath = (Get-ChildItem -LiteralPath (Join-Path $vendorRoot 'thermo') -Recurse -File -Filter '*.raw' | Select-Object -First 1).FullName
        }
    }
}
if ([string]::IsNullOrWhiteSpace($InputPath) -or -not (Test-Path $InputPath)) {
    throw "No $Vendor fixture was found under $vendorRoot"
}
$InputPath = (Resolve-Path $InputPath).Path
$requestedPolarity = if ($InputPath.ToLowerInvariant().Contains('_neg')) { -1 } else { 1 }
if (-not $SkipBuild) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'scripts\build\cpp\build-cpp.ps1') -Clean -Config Release
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
}

$executable = Get-BackendMcpExecutable -RepositoryRoot $repoRoot
$database = Join-Path $repoRoot "tmp\projects\streamfind-cpp-$($Vendor.ToLowerInvariant())-reader-script.duckdb"
$projectId = "reader-cpp-$($Vendor.ToLowerInvariant())"
New-Item -ItemType Directory -Force -Path (Split-Path $database -Parent) | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $database
$env:STREAMFIND_CATALOGUE = $catalogue
$env:PATH = (Join-Path $repoRoot 'tmp\build\mingw-ucrt64\tests') + ';' + $env:PATH

$process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue
try {
    Initialize-Mcp $process | Out-Null
    Invoke-McpTool $process 2 'create' @{
        database_path = $database
        domain = 'mass_spec'
    } | Out-Null
    $workflow = @{
        schema_version = 1
        operations = @(
            @{
                id = 'read-1'
                operation = 'mass_spec.read_mass_spec_files'
                parameters = @{ source_paths = @($InputPath) }
                inputs = @{}
                position = @{}
            },
            @{
                id = 'headers-1'
                operation = 'mass_spec.get_spectra_headers'
                parameters = @{ analysis_names = @(); indices = @(0) }
                inputs = @{}
                position = @{}
            },
            @{
                id = 'spectra-1'
                operation = 'mass_spec.get_raw_spectra'
                parameters = @{ analysis_names = @(); indices = @(0); polarity = $requestedPolarity }
                inputs = @{}
                position = @{}
            }
        )
        connections = @(
            @{
                source_operation = 'read-1'
                source_port = 'analysesTable'
                target_operation = 'headers-1'
                target_port = 'analysesTable'
            },
            @{
                source_operation = 'read-1'
                source_port = 'spectraHeadersTable'
                target_operation = 'headers-1'
                target_port = 'spectraHeadersTable'
            },
            @{
                source_operation = 'read-1'
                source_port = 'analysesTable'
                target_operation = 'spectra-1'
                target_port = 'analysesTable'
            },
            @{
                source_operation = 'headers-1'
                source_port = 'spectraHeadersTable'
                target_operation = 'spectra-1'
                target_port = 'spectraHeadersTable'
            }
        )
    }
    Invoke-McpTool $process 3 'set_workflow' @{
        database_path = $database
        workflow = $workflow
    } | Out-Null
    $validation = Invoke-McpTool $process 4 'validate_workflow' @{
        database_path = $database
        workflow = $workflow
    }
    if (-not $validation.valid) {
        throw "Thermo workflow validation failed: $($validation | ConvertTo-Json -Compress -Depth 8)"
    }
    $execution = Invoke-McpTool $process 5 'run_workflow' @{ database_path = $database }
    if ($null -eq $execution) {
        throw "Expected a completed $Vendor reader workflow execution"
    }
    $inventory = Invoke-McpTool $process 6 'get_artifact_inventory' @{ database_path = $database }
    if (@($inventory).Count -lt 3) {
        throw "Expected analyses, spectra headers, and spectra artifacts after $Vendor reader workflow"
    }
    Write-Host "C++ $Vendor reader workflow test passed: $InputPath ($(@($inventory).Count) artifacts)"
} finally {
    Stop-StreamfindMcp $process
    Remove-Item -Force -ErrorAction SilentlyContinue $database
}
