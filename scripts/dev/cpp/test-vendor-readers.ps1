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
    $added = Invoke-McpTool $process 3 'mass_spec.add_analyses' @{
        database_path = $database
        analyses = @(@{ path = $InputPath })
    }
    $expectedCount = [int]$added.row_count
    if ($expectedCount -lt 1) {
        throw "Expected at least one parsed $Vendor analysis, received $expectedCount"
    }
    $info = Invoke-McpTool $process 4 'mass_spec.get_analyses' @{
        database_path = $database
    }
    if ([int]$info.row_count -ne $expectedCount) {
        throw "Expected $expectedCount persisted $Vendor analyses, received $($info.row_count)"
    }
    Write-Host "C++ $Vendor reader test passed: $InputPath"
} finally {
    Stop-StreamfindMcp $process
    Remove-Item -Force -ErrorAction SilentlyContinue $database
}
