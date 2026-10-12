[CmdletBinding()]
param(
    [string]$RawPath = 'E:\example_files\raw_vendor_files\thermo\others\22090901_H2O_1_pos.raw',
    [string]$ProfileMzmlPath = 'E:\example_files\raw_vendor_files\thermo\others\profile\22090901_H2O_1_pos.mzML',
    [string]$CentroidMzmlPath = 'E:\example_files\raw_vendor_files\thermo\others\centroided\22090901_H2O_1_pos.mzML',
    [switch]$SkipBuild
)

. (Join-Path $PSScriptRoot '..\mcp-common.ps1')
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$catalogue = Join-Path $repoRoot 'tmp\build\mingw-ucrt64\semantic_catalogue\catalogue.duckdb'
$paths = @($RawPath, $ProfileMzmlPath, $CentroidMzmlPath)
foreach ($path in $paths) {
    if (-not (Test-Path $path -PathType Leaf)) { throw "Reader fixture not found: $path" }
}
if (-not $SkipBuild) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'scripts\build\cpp\build-cpp.ps1') -Config Release -Plugins mass_spec
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
}

$executable = Get-BackendMcpExecutable -RepositoryRoot $repoRoot
$env:STREAMFIND_CATALOGUE = $catalogue
$env:PATH = (Join-Path $repoRoot 'tmp\build\mingw-ucrt64\tests') + ';' + $env:PATH
$process = Start-StreamfindMcp -Executable $executable -Catalogue $catalogue

function Read-ReaderArtifacts {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$Id,
        [Parameter(Mandatory = $true)][string]$Database,
        [Parameter(Mandatory = $true)][string]$SourcePath
    )

    Invoke-McpTool $Process $Id 'create' @{ database_path = $Database; domain = 'mass_spec' } | Out-Null
    $analysisName = [System.IO.Path]::GetFileNameWithoutExtension($SourcePath)
    $workflow = @{
        schema_version = 1
        operations = @(
            @{ id = 'read-1'; operation = 'mass_spec.read_mass_spec_files'; parameters = @{ source_paths = @($SourcePath) }; inputs = @{}; position = @{} },
            @{ id = 'headers-1'; operation = 'mass_spec.get_spectra_headers'; parameters = @{ analysis_names = @($analysisName); indices = @(0) }; inputs = @{}; position = @{} },
            @{ id = 'spectra-1'; operation = 'mass_spec.get_raw_spectra'; parameters = @{ analysis_names = @($analysisName); indices = @(0); polarity = 1 }; inputs = @{}; position = @{} }
        )
        connections = @(
            @{ source_operation = 'read-1'; source_port = 'analysesTable'; target_operation = 'headers-1'; target_port = 'analysesTable' },
            @{ source_operation = 'read-1'; source_port = 'spectraHeadersTable'; target_operation = 'headers-1'; target_port = 'spectraHeadersTable' },
            @{ source_operation = 'read-1'; source_port = 'analysesTable'; target_operation = 'spectra-1'; target_port = 'analysesTable' },
            @{ source_operation = 'headers-1'; source_port = 'spectraHeadersTable'; target_operation = 'spectra-1'; target_port = 'spectraHeadersTable' }
        )
    }
    Invoke-McpTool $Process ($Id + 1) 'set_workflow' @{ database_path = $Database; workflow = $workflow } | Out-Null
    $validation = Invoke-McpTool $Process ($Id + 2) 'validate_workflow' @{ database_path = $Database; workflow = $workflow }
    if (-not $validation.valid) { throw "Cross-format workflow validation failed for $SourcePath" }
    Invoke-McpTool $Process ($Id + 3) 'run_workflow' @{ database_path = $Database } | Out-Null
    $inventory = @((Invoke-McpTool $Process ($Id + 4) 'get_artifact_inventory' @{ database_path = $Database }).GetEnumerator())
    $header = $inventory | Where-Object { $_.contract_id -eq 'spectraHeadersTable' -and $_.producer_instance -eq 'read-1' } | Select-Object -Last 1
    $spectra = $inventory | Where-Object { $_.contract_id -eq 'spectraTable' } | Select-Object -Last 1
    if ($null -eq $header -or $null -eq $spectra) {
        throw "Cross-format artifacts were not published for ${SourcePath}: $($inventory | ConvertTo-Json -Compress -Depth 5)"
    }
    $headerData = @((Invoke-McpTool $Process ($Id + 6) 'request_artifact' @{ database_path = $Database; artifact_id = [string]$header.artifact_id; include_data = $true; limit = 10000 }).GetEnumerator())
    $spectraData = @((Invoke-McpTool $Process ($Id + 7) 'request_artifact' @{ database_path = $Database; artifact_id = [string]$spectra.artifact_id; include_data = $true; limit = 20 }).GetEnumerator())
    if (@($headerData).Count -eq 0) {
        throw "Cross-format header artifact data was empty for $SourcePath"
    }
    if ($null -eq $headerData[0].data -or @($headerData[0].data).Count -eq 0) {
        throw "Cross-format header data was empty for ${SourcePath}: $($headerData | ConvertTo-Json -Compress -Depth 5)"
    }
    if (@($spectraData).Count -eq 0 -or $null -eq $spectraData[0].data -or @($spectraData[0].data).Count -eq 0) {
        throw "Flattened spectrum artifact data was empty for $SourcePath"
    }
    return [pscustomobject]@{
        Path = $SourcePath
        Headers = @($headerData[0].data)
        Spectra = @($spectraData[0].data)
    }
}

function Compare-HeaderTables {
    param(
        [Parameter(Mandatory = $true)][object[]]$Left,
        [Parameter(Mandatory = $true)][object[]]$Right,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][bool]$CompareArrayLength
    )
    if ($Left.Count -ne $Right.Count) {
        throw "$Label header row counts differ: $($Left.Count) vs $($Right.Count)"
    }
    # `scan` is a source-specific identifier: Thermo stores the vendor scan
    # number, while mzML uses the zero-based spectrum index.  Row order and
    # the normalized `index` column are compared below instead.
    $integerColumns = @('index', 'level', 'mode', 'polarity', 'configuration', 'precursor_charge')
    $realColumns = @('lowmz', 'highmz', 'bpmz', 'bpint', 'tic', 'rt', 'mobility', 'window_mz', 'window_mzlow', 'window_mzhigh', 'precursor_mz', 'precursor_intensity', 'activation_ce')
    if ($CompareArrayLength) { $integerColumns += 'array_length' }
    $maxError = @{}
    foreach ($column in $realColumns) { $maxError[$column] = 0.0 }
    $strictRealColumns = @('rt', 'mobility', 'window_mz', 'window_mzlow', 'window_mzhigh',
                           'precursor_mz', 'activation_ce')
    $sourceDependentColumns = @('lowmz', 'highmz', 'bpmz', 'bpint', 'tic', 'precursor_intensity')
    for ($rowIndex = 0; $rowIndex -lt $Left.Count; ++$rowIndex) {
        $leftRow = $Left[$rowIndex]
        $rightRow = $Right[$rowIndex]
        foreach ($column in ($integerColumns + $realColumns)) {
            if (-not ($leftRow.PSObject.Properties.Name -contains $column) -or
                -not ($rightRow.PSObject.Properties.Name -contains $column)) {
                throw "$Label is missing header column '$column' at row $rowIndex"
            }
        }
        foreach ($column in $integerColumns) {
            if ([int64]$leftRow.$column -ne [int64]$rightRow.$column) {
                throw "$Label mismatch at row $rowIndex column ${column}: $($leftRow.$column) vs $($rightRow.$column)"
            }
        }
        foreach ($column in $realColumns) {
            $leftValue = $leftRow.$column
            $rightValue = $rightRow.$column
            if ($null -eq $leftValue -or $null -eq $rightValue) {
                if ($column -in $strictRealColumns -and $null -ne $leftValue -and $null -ne $rightValue) {
                    throw "$Label mismatch at row $rowIndex column ${column}: $leftValue vs $rightValue"
                }
                continue
            }
            $error = [math]::Abs([double]$leftValue - [double]$rightValue)
            if ($error -gt $maxError[$column]) { $maxError[$column] = $error }
            $tolerance = if ($column -eq 'rt') { 0.01 } elseif ($column -match 'mz|bpmz|precursor') { 0.01 } else { 0.1 }
            if ($column -in $strictRealColumns -and $error -gt $tolerance) {
                throw "$Label mismatch at row $rowIndex column ${column}: $leftValue vs $rightValue (rt $($leftRow.rt)/$($rightRow.rt), levels $($leftRow.level)/$($rightRow.level), precursor $($leftRow.precursor_mz)/$($rightRow.precursor_mz), windows $($leftRow.window_mzlow)-$($leftRow.window_mzhigh) / $($rightRow.window_mzlow)-$($rightRow.window_mzhigh))"
            }
        }
    }
    $ms2Count = @($Left | Where-Object { [int]$_.level -ge 2 }).Count
    Write-Host "$Label passed: $($Left.Count) scans, $ms2Count MS2 scans."
    Write-Host "$Label maximum numeric differences: $($maxError | ConvertTo-Json -Compress)"
    Write-Host "$Label source-dependent summary fields were validated for presence and numeric form: $($sourceDependentColumns -join ', ')"
}

try {
    Initialize-Mcp $process | Out-Null
    $databases = 1..3 | ForEach-Object { Join-Path $repoRoot "tmp\projects\thermo-cross-format-$($_).duckdb" }
    foreach ($database in $databases) { Remove-Item -Force -ErrorAction SilentlyContinue $database }
    $results = @(
        Read-ReaderArtifacts $process 10 $databases[0] $RawPath
        Read-ReaderArtifacts $process 30 $databases[1] $ProfileMzmlPath
        Read-ReaderArtifacts $process 50 $databases[2] $CentroidMzmlPath
    )
    $raw = $results[0]
    $profile = $results[1]
    $centroid = $results[2]
    $rawHeader = $raw.Headers[0]
    $profileHeader = $profile.Headers[0]
    $centroidHeader = $centroid.Headers[0]
    if ([int]$rawHeader.level -ne [int]$centroidHeader.level -or [int]$rawHeader.polarity -ne [int]$centroidHeader.polarity) {
        throw 'RAW and centroid mzML metadata disagree for the first scan.'
    }
    if ([math]::Abs([double]$rawHeader.rt - [double]$centroidHeader.rt) -gt 0.01) {
        throw 'RAW and centroid mzML retention times disagree for the first scan.'
    }
    if ([int]$profileHeader.array_length -le [int]$centroidHeader.array_length) {
        throw "Profile reference did not have a larger first scan than centroid reference ($($profileHeader.array_length) vs $($centroidHeader.array_length))."
    }
    Compare-HeaderTables $raw.Headers $centroid.Headers 'RAW vs centroid mzML' $true
    Compare-HeaderTables $raw.Headers $profile.Headers 'RAW vs profile mzML' $false
    Write-Host "Thermo cross-format metadata check passed."
    Write-Host "RAW first scan:       level=$($rawHeader.level) polarity=$($rawHeader.polarity) rt=$($rawHeader.rt) points=$($rawHeader.array_length)"
    Write-Host "Profile mzML first:   level=$($profileHeader.level) polarity=$($profileHeader.polarity) rt=$($profileHeader.rt) points=$($profileHeader.array_length)"
    Write-Host "Centroid mzML first:  level=$($centroidHeader.level) polarity=$($centroidHeader.polarity) rt=$($centroidHeader.rt) points=$($centroidHeader.array_length)"
    $rawPoint = @($raw.Spectra)[0]
    $centroidPoint = @($centroid.Spectra)[0]
    foreach ($column in @('mz', 'intensity')) {
        if (-not ($rawPoint.PSObject.Properties.Name -contains $column) -or
            -not ($centroidPoint.PSObject.Properties.Name -contains $column)) {
            throw "Spectrum parity data does not contain the expected '$column' column."
        }
        if ([math]::Abs([double]$rawPoint.$column - [double]$centroidPoint.$column) -gt 0.0001) {
            throw "RAW and centroid mzML '$column' values disagree for the first spectrum point."
        }
    }
    Write-Host "RAW and centroid mzML first spectrum point matched."
}
finally {
    foreach ($database in $databases) { Remove-Item -Force -ErrorAction SilentlyContinue $database }
    Stop-StreamfindMcp $process
}
