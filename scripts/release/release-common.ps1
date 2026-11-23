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
        $_.FullName -match '(?i)(ClearCore|ProteoWizard|msconvert|baf2sql|WinDbg|(?<![A-Za-z0-9])CDB(?:\.exe)?(?![A-Za-z0-9])|vendor[-_ ]?(sdk|dll)|confidential|oracle)'
    }
    if ($forbidden) {
        throw "Distribution payload contains development-only material: $($forbidden.FullName -join ', ')"
    }
}

function Assert-CppDistributionPayload([string]$PackageRoot) {
    Assert-DistributionPayload $PackageRoot
    $abiHeaderPath = Join-Path $Script:REPO_ROOT 'cpp/sdk/include/streamfind/plugin_abi.h'
    $abiHeader = Get-Content -Raw $abiHeaderPath
    if ($abiHeader -notmatch 'STREAMFIND_PLUGIN_ABI_MAJOR\s*=\s*(\d+)') {
        throw "Unable to read plugin ABI major version from $abiHeaderPath"
    }
    $expectedPluginAbiMajor = [int]$Matches[1]
    $required = @(
        'bin/streamfind.exe',
        'bin/streamfind-app.exe',
        'bin/streamfind-server.exe',
        'bin/streamfind-worker.exe',
        'bin/streamfind-mcp.exe',
        'bin/streamfind-dev.exe',
        'bin/streamfind-tray.exe',
        'bin/streamfind.json',
        'plugins/native/native.plugin.mass_spec/plugin.json',
        'plugins/native/native.plugin.mass_spec/catalogue.duckdb',
        'plugins/native/native.plugin.mass_spec/streamfind_mass_spec.dll',
        'plugins/native/native.plugin.mass_spec/ontology/operations.ttl',
        'plugins/native/native.plugin.raman/plugin.json',
        'plugins/native/native.plugin.raman/catalogue.duckdb',
        'plugins/native/native.plugin.raman/streamfind_raman.dll',
        'plugins/native/native.plugin.sensors/plugin.json',
        'plugins/native/native.plugin.sensors/catalogue.duckdb',
        'plugins/native/native.plugin.sensors/streamfind_sensors.dll',
        'core/catalogue.duckdb',
        'core/ontology/operations.ttl',
        'bin/openbabel_streamfind.dll',
        'bin/duckdb.dll',
        'bin/libgcc_s_seh-1.dll',
        'bin/libstdc++-6.dll',
        'bin/libwinpthread-1.dll',
        'bin/data/openbabel/logp.txt',
        'app/index.html',
        'dev/streamfind-dev-setup.ps1',
        'dev/StreamFindProvisioning.psm1',
        'dev/toolchains.lock.json',
        'dev/setup-manifest.json',
        'sdk/native/sdk-manifest.json',
        'sdk/native/cmake/streamfind/streamfindConfig.cmake',
        'sdk/native/bin/streamfind_sdk_plugin_validator.exe',
        'sdk/native/bin/streamfind_sdk_catalogue.exe',
        'sdk/viewer/@streamfind/viewer-sdk/package.json',
        'sdk/viewer/@streamfind/viewer-sdk/src/index.ts'
    )
    foreach ($relative in $required) {
        if (-not (Test-Path (Join-Path $PackageRoot $relative))) {
            throw "C++ distribution payload is missing $relative"
        }
    }
    $machineLocal = Get-ChildItem -Recurse -File $PackageRoot | Where-Object {
        $relative = $_.FullName.Substring($PackageRoot.Length).TrimStart('\','/')
        $relative -match '(?i)(^|[\\/])(node_modules|\.venv|tmp[\\/]build)([\\/]|$)|(^|[\\/])(home|Users|mnt)[\\/]'
    }
    if ($machineLocal) {
        throw "C++ distribution contains machine-local or generated development paths: $($machineLocal.FullName -join ', ')"
    }

    $legacyVendorRoot = Join-Path $PackageRoot 'core/vendors'
    if (Test-Path $legacyVendorRoot) {
        throw "C++ distribution contains the removed legacy runtime directory: $legacyVendorRoot"
    }
    $nativeOutsideBin = Get-ChildItem -Recurse -File $PackageRoot |
        Where-Object {
            $_.Extension -match '(?i)^\.(dll|so|dylib)$' -and
            $_.FullName.Substring($PackageRoot.Length).TrimStart('\','/') -notmatch '^(bin[\\/]|sdk[\\/]native[\\/](bin|lib)[\\/]|plugins[\\/][^\\/]+[\\/])'
        }
    if ($nativeOutsideBin) {
        throw "C++ distribution contains native runtime files outside bin or plugin packages: $($nativeOutsideBin.FullName -join ', ')"
    }

    $pluginRoot = Join-Path $PackageRoot 'plugins'
    $nativePluginRoot = Join-Path $pluginRoot 'native'
    $pluginDirectories = @(Get-ChildItem -Path $nativePluginRoot -Directory)
    if ($pluginDirectories.Count -eq 0) {
        throw "C++ distribution contains no native plugins under $nativePluginRoot"
    }
    foreach ($pluginDirectory in $pluginDirectories) {
        $plugin = $pluginDirectory.Name
        $manifestPath = Join-Path $pluginDirectory.FullName 'plugin.json'
        try {
            $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
        } catch {
            throw "Invalid plugin manifest: $manifestPath ($($_.Exception.Message))"
        }
        if ($manifest.plugin_id -ne $plugin) { throw "Plugin manifest ID mismatch: $manifestPath" }
        $expectedPluginName = $plugin -replace '^native\.plugin\.', ''
        if ($manifest.plugin -ne $expectedPluginName) { throw "Plugin manifest plugin mismatch: $manifestPath" }
        if ($manifest.version -ne $env:STREAMFIND_PACKAGE_VERSION -and $env:STREAMFIND_PACKAGE_VERSION) {
            throw "Plugin manifest version mismatch: $manifestPath"
        }
        if ($manifest.static_composition) { throw "Plugin manifest must declare static_composition=false: $manifestPath" }
        if ($manifest.abi_version.major -ne $expectedPluginAbiMajor) {
            throw "Plugin manifest ABI major mismatch: expected $expectedPluginAbiMajor, got $($manifest.abi_version.major): $manifestPath"
        }
        if ($manifest.abi_version.minor -lt 0) { throw "Plugin manifest ABI minor is invalid: $manifestPath" }
        if ($manifest.semantic_catalogue -ne 'catalogue.duckdb') { throw "Plugin manifest catalogue mismatch: $manifestPath" }
        $libraryName = $manifest.library.'windows-x86_64'
        if (-not $libraryName) { throw "Plugin manifest Windows library is missing: $manifestPath" }
        foreach ($requiredPluginFile in @('catalogue.duckdb', $libraryName, 'ontology')) {
            if (-not (Test-Path (Join-Path $pluginDirectory.FullName $requiredPluginFile))) {
                throw "C++ distribution plugin payload is missing $requiredPluginFile in $plugin"
            }
        }
    }
    $setupPreview = & pwsh -NoProfile -File (Join-Path $PackageRoot 'dev/streamfind-dev-setup.ps1') setup -Target all -Workspace (Join-Path $PackageRoot '..\package-workspace-preview') -ManagedHome (Join-Path $PackageRoot '..\package-managed-home-preview') -DryRun | ConvertFrom-Json
    if ($setupPreview.target -ne 'all' -or (($setupPreview.actions -join "`n") -notmatch 'Python')) {
        throw 'Combined runtime package setup preview did not include workspace Python provisioning'
    }
}

function Write-ReleaseChecksums([string]$Version) {
    $versionPattern = [regex]::Escape($Version)
    $sums = Get-ChildItem $Script:RELEASE_OUTPUT -File |
        Where-Object { $_.Name -match "^streamfind-.+-$versionPattern-(Windows|Linux)-.+\.(zip|tgz|tar\.gz)$" } |
        Sort-Object Name |
        ForEach-Object {
            $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $($_.Name)"
        }
    [System.IO.File]::WriteAllText(
        (Join-Path $Script:RELEASE_OUTPUT 'sha256sums.txt'),
        (($sums -join "`n") + "`n"),
        [System.Text.UTF8Encoding]::new($false)
    )
}
