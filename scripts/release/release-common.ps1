Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Script:REPO_ROOT = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Script:RELEASE_OUTPUT = Join-Path $Script:REPO_ROOT 'tmp\release-output'
New-Item -ItemType Directory -Force -Path $Script:RELEASE_OUTPUT | Out-Null

function Write-ReleaseLog([string]$Message) {
    Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message)
}

function Assert-DistributionPayload([string]$PackageRoot, [string]$LicensePayload = 'licenses') {
    foreach ($required in @('NOTICE.md', 'LICENSE.md', $LicensePayload)) {
        if (-not (Test-Path (Join-Path $PackageRoot $required))) {
            throw "Distribution payload is missing $required"
        }
    }
    $forbidden = Get-ChildItem -Recurse -File $PackageRoot | Where-Object {
        $_.FullName -match '(?i)(ClearCore|ProteoWizard|msconvert|baf2sql|WinDbg|CDB|vendor[-_ ]?(sdk|dll)|confidential|oracle)'
    }
    if ($forbidden) {
        throw "Distribution payload contains development-only material: $($forbidden.FullName -join ', ')"
    }
}

function Assert-CppDistributionPayload([string]$PackageRoot, [switch]$RequireSdk) {
    Assert-DistributionPayload $PackageRoot
    $required = @(
        'bin/streamfind.exe',
        'bin/streamfind_service.exe',
        'bin/streamfind_mcp.exe',
        'bin/streamfind_mcp_launcher.exe',
        'bin/streamfind_cli.exe',
        'bin/streamfind.json',
        'plugins/mass_spec/plugin.json',
        'plugins/mass_spec/catalogue.duckdb',
        'plugins/mass_spec/streamfind_mass_spec.dll',
        'plugins/mass_spec/ontology/operations.ttl',
        'plugins/raman/plugin.json',
        'plugins/raman/catalogue.duckdb',
        'plugins/raman/streamfind_raman.dll',
        'plugins/sensors/plugin.json',
        'plugins/sensors/catalogue.duckdb',
        'plugins/sensors/streamfind_sensors.dll',
        'core/catalogue.duckdb',
        'core/ontology/operations.ttl',
        'core/vendors/openbabel/openbabel_streamfind.dll',
        'core/vendors/duckdb/duckdb.dll',
        'core/vendors/openbabel/data/logp.txt',
        'app/index.html'
    )
    foreach ($relative in $required) {
        if (-not (Test-Path (Join-Path $PackageRoot $relative))) {
            throw "C++ distribution payload is missing $relative"
        }
    }
    $binVendorFiles = @(Get-ChildItem -Path (Join-Path $PackageRoot 'bin') -File |
        Where-Object { $_.Name -match '(?i)^(duckdb|libgcc|libstdc\+\+|libwinpthread).*\.dll$' })
    if ($binVendorFiles.Count -ne 0) {
        throw "Vendor runtime DLLs must not be installed directly under bin: $($binVendorFiles.Name -join ', ')"
    }
    $pluginRoot = Join-Path $PackageRoot 'plugins'
    $pluginDirectories = @(Get-ChildItem -Path $pluginRoot -Directory)
    if ($pluginDirectories.Count -eq 0) {
        throw "C++ distribution contains no plugins under $pluginRoot"
    }
    foreach ($pluginDirectory in $pluginDirectories) {
        $domain = $pluginDirectory.Name
        $manifestPath = Join-Path $pluginDirectory.FullName 'plugin.json'
        try {
            $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
        } catch {
            throw "Invalid plugin manifest: $manifestPath ($($_.Exception.Message))"
        }
        if ($manifest.plugin_id -ne $domain) { throw "Plugin manifest ID mismatch: $manifestPath" }
        if ($manifest.domain -ne $domain) { throw "Plugin manifest domain mismatch: $manifestPath" }
        if ($manifest.version -ne $env:STREAMFIND_PACKAGE_VERSION -and $env:STREAMFIND_PACKAGE_VERSION) {
            throw "Plugin manifest version mismatch: $manifestPath"
        }
        if ($manifest.static_composition) { throw "Plugin manifest must declare static_composition=false: $manifestPath" }
        if ($manifest.abi_version.major -ne 1) { throw "Plugin manifest ABI major mismatch: $manifestPath" }
        if ($manifest.abi_version.minor -lt 0) { throw "Plugin manifest ABI minor is invalid: $manifestPath" }
        if ($manifest.semantic_catalogue -ne 'catalogue.duckdb') { throw "Plugin manifest catalogue mismatch: $manifestPath" }
        $libraryName = $manifest.library.'windows-x86_64'
        if (-not $libraryName) { throw "Plugin manifest Windows library is missing: $manifestPath" }
        foreach ($requiredPluginFile in @('catalogue.duckdb', $libraryName, 'ontology')) {
            if (-not (Test-Path (Join-Path $pluginDirectory.FullName $requiredPluginFile))) {
                throw "C++ distribution plugin payload is missing $requiredPluginFile in $domain"
            }
        }
    }
    if ($RequireSdk) {
        foreach ($relative in @(
            'sdk/bin/streamfind_sdk_catalogue.exe',
            'sdk/bin/streamfind_sdk_plugin_validator.exe',
            'sdk/include/streamfind/sdk/catalogue_builder.hpp',
            'sdk/cmake/streamfind/streamfindConfig.cmake',
            'sdk/cmake/streamfind/streamfind-cpp-targets.cmake',
            'sdk/tools/jena/bat/arq.bat')) {
            if (-not (Test-Path (Join-Path $PackageRoot $relative))) {
                throw "C++ SDK distribution payload is missing $relative"
            }
        }
        $sdkLibraryCandidates = @(
            (Join-Path $PackageRoot 'sdk/lib/streamfind_cpp_sdk.lib'),
            (Join-Path $PackageRoot 'sdk/lib/libstreamfind_cpp_sdk.a'))
        if (-not ($sdkLibraryCandidates | Where-Object { Test-Path $_ })) {
            throw "C++ SDK distribution payload is missing the SDK library archive"
        }
    }
}

function Write-ReleaseChecksums {
    $sums = Get-ChildItem $Script:RELEASE_OUTPUT -File |
        Where-Object { $_.Name -like 'streamfind-*.zip' -or $_.Name -like 'streamfind-*.tgz' -or $_.Name -like 'streamfind-*.tar.gz' } |
        Sort-Object Name |
        ForEach-Object {
            $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $($_.Name)"
        }
    $sums | Set-Content (Join-Path $Script:RELEASE_OUTPUT 'sha256sums.txt')
}
