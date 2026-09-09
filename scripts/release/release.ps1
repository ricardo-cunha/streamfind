<#
    release.ps1 — create local, versioned release archives in <repo-root>/tmp/release-output/.

    Produces strictly separate, self-contained archives per backend:
      tmp/release-output/streamfind-core-cpp-<ver>-Windows-<arch>.zip (CPack ZIP; libs+headers+DLLs+catalogue)
      tmp/release-output/streamfind-rust-<ver>-Windows-<arch>.zip     (assembled: bin/ + share/streamfind)
    With -Linux, additionally builds in WSL and copies the Linux TGZs into
    tmp/release-output/ (see scripts/release/release-linux.sh, which runs inside WSL).

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 0.2.0
      powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Version 0.2.0 -Rust -Linux

    Args:
      -Version    required; the release version (e.g. 0.2.0).
      -Core       build + package the C++ core (default: on)
      -Rust       build + package the Rust workspace (default: on)
      -Linux      also produce the Linux archives via WSL (default: off)
      -SkipTests  skip the official test suites before packaging
      -Config     C++ build config (default Release)
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Core = $true,
    [switch]$Rust = $true,
    [switch]$Linux,
    [switch]$SkipTests,
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$releases = Join-Path $root 'tmp\release-output'
New-Item -ItemType Directory -Force -Path $releases | Out-Null

function Log([string]$message) {
    Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $message)
}

function Assert-DistributionPayload([string]$root, [string]$licensePayload = 'licenses') {
    foreach ($required in @('NOTICE.md', 'LICENSE.md', $licensePayload)) {
        if (-not (Test-Path (Join-Path $root $required))) {
            throw "Distribution payload is missing $required"
        }
    }
    $forbidden = Get-ChildItem -Recurse -File $root | Where-Object {
        $_.FullName -match '(?i)(ClearCore|ProteoWizard|msconvert|baf2sql|WinDbg|CDB|vendor[-_ ]?(sdk|dll)|confidential|oracle)'
    }
    if ($forbidden) {
        throw "Distribution payload contains development-only material: $($forbidden.FullName -join ', ')"
    }
}

. (Join-Path $PSScriptRoot '..\build\build-common.ps1')
Start-ScriptLog 'release'
Invoke-SemanticChecks

if ($Core) {
    Log "=== C++ core release (Windows, $Config) ==="
    $buildDir = Join-Path $root "tmp\build\release-core"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $buildDir
    $cmake = Get-CMake
    $ninja = Get-Ninja
    $coreSrc = Join-Path $root 'core'
    Log "Configuring + building C++ core... (this takes a few minutes)"
    Invoke-VcvarsAll x64
    & $cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$Config" `
        -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_BUILD_SHARED=OFF `
        "-B $buildDir" "-S $coreSrc"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    & $cmake --build $buildDir --config $Config -j 8
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
    if (-not $SkipTests) {
        Log "Running official C++ tests..."
        $ctest = Get-CTest
        & $ctest --test-dir $buildDir -C $Config --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "CTest failed ($LASTEXITCODE)" }
    }
    Log "Packaging C++ core with CPack..."
    $cpack = Get-CPack
    # CPack writes its staging tree + the final archive into CPackPackageDir;
    # the archive is then moved into the temporary output directory and the
    # staging tree is deleted so no CPack internals leak into the output.
    $cpackDir = Join-Path $root "tmp\build\cpack-out"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cpackDir
    & $cpack -G ZIP -C $Config -B $cpackDir --config $buildDir/CPackConfig.cmake
    if ($LASTEXITCODE -ne 0) { throw "CPack failed ($LASTEXITCODE)" }
    $built = Get-ChildItem (Join-Path $cpackDir "streamfind-core-cpp-$Version-Windows-*.zip") | Select-Object -First 1
    if (-not $built) { throw "CPack archive not produced for core-cpp-$Version" }
    $verifyDir = Join-Path $root "tmp\build\package-verify\cpp"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $verifyDir
    Expand-Archive -Path $built.FullName -DestinationPath $verifyDir
    $packageRoot = Get-ChildItem -Path $verifyDir -Directory | Select-Object -First 1
    if (-not $packageRoot) { throw "CPack archive has no top-level package directory" }
    Assert-DistributionPayload $packageRoot.FullName
    Move-Item $built.FullName (Join-Path $releases $built.Name) -Force
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cpackDir
    $archive = Get-Item (Join-Path $releases $built.Name)
    Log "C++ archive: $($archive.Name) ($([math]::Round($archive.Length / 1MB, 1)) MB)"
}

if ($Rust) {
    Log "=== Rust release (Windows) ==="
    $cargo = Get-Cargo
    $targetDir = Join-Path $root 'tmp\build\rust-target'
    $env:CARGO_TARGET_DIR = $targetDir
    $env:TMP = 'C:\Windows\Temp'; $env:TEMP = 'C:\Windows\Temp'; $env:TMPDIR = 'C:\Windows\Temp'
    $duckdbRoot = Join-Path $root 'core\vendor\duckdb'
    $env:DUCKDB_INCLUDE_DIR = Join-Path $duckdbRoot 'include'
    $env:DUCKDB_LIB_DIR = Join-Path $duckdbRoot 'lib\windows-x64'
    $env:DUCKDB_STATIC = '0'
    $rustDir = Join-Path $root 'rust'
    Log "Building Rust workspace (release, stripped)..."
    Push-Location $rustDir
    try {
        & $cargo build --release --workspace --exclude streamfind-rust-test-support
        if ($LASTEXITCODE -ne 0) { throw "cargo build --release failed ($LASTEXITCODE)" }
        if (-not $SkipTests) {
            Log "Running official Rust release test suite..."
            & $cargo test --workspace
            if ($LASTEXITCODE -ne 0) { throw "cargo test failed ($LASTEXITCODE)" }
        }
    } finally {
        Pop-Location
    }
    # Top-level folder name inside the archive: matches the versioned
    # directory layout of the C++ core archive (streamfind-<backend>-<ver>-...).
    $packagedRoot = Join-Path $root "tmp\staging\streamfind-rust-$Version-Windows-x86_64"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $packagedRoot
    New-Item -ItemType Directory -Force -Path (Join-Path $packagedRoot 'bin') | Out-Null
    Copy-Item (Join-Path $targetDir "release\streamfind-rust-cli.exe") (Join-Path $packagedRoot 'bin')
    Copy-Item (Join-Path $targetDir "release\streamfind-rust-mcp.exe")  (Join-Path $packagedRoot 'bin')
    Copy-Item (Join-Path $root 'core\vendor\duckdb\lib\windows-x64\duckdb.dll') (Join-Path $packagedRoot 'bin')
    $share = Join-Path $packagedRoot 'share\streamfind'
    New-Item -ItemType Directory -Force -Path $share | Out-Null
    Copy-Item (Join-Path $root 'semantic\generated\catalogue.duckdb') $share
    Copy-Item (Join-Path $root 'LICENSE.md') $packagedRoot
    Copy-Item (Join-Path $root 'NOTICE.md') $packagedRoot
    Copy-Item (Join-Path $rustDir 'LICENSES.md') $packagedRoot
    Assert-DistributionPayload $packagedRoot 'LICENSES.md'
    Log "Compressing Rust archive..."
    $zip = Join-Path $releases "streamfind-rust-$Version-Windows-x86_64.zip"
    Remove-Item -Force -ErrorAction SilentlyContinue $zip
    # Compress the packaged root folder itself so the archive has a single
    # top-level directory (streamfind-rust-<ver>-.../) matching the CPack
    # layout of the C++ core archive.
    Compress-Archive -Path $packagedRoot -DestinationPath $zip -CompressionLevel Optimal -Force
    $info = Get-Item $zip
    Log "Rust archive: $($info.Name) ($([math]::Round($info.Length / 1MB, 1)) MB)"
}

if ($Linux) {
    Log "=== Linux release via WSL ==="
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { throw "WSL not found; install WSL2 for Linux releases." }
    # Windows releases dir as an /mnt/c path visible inside WSL.
    $wslReleases = '/mnt/c/' + ($releases -replace '^C:\\', '' -replace '\\', '/')
    $wslScript = '/mnt/c/' + ((Join-Path $PSScriptRoot 'release-linux.sh') -replace '^C:\\', '' -replace '\\', '/')
    Log "Invoking WSL: release-linux.sh -v $Version (build tree on ext4)"
    & wsl.exe -d Ubuntu -- bash -lc "STREAMFIND_RELEASE_VERSION='$Version' STREAMFIND_RELEASE_DIR='$wslReleases' bash '$wslScript'"
    if ($LASTEXITCODE -ne 0) { throw "WSL Linux release failed ($LASTEXITCODE)" }
    Log "Linux archives:"
    Get-ChildItem $releases -Filter "*-Linux-*" | ForEach-Object {
        Log "  $($_.Name) ($([math]::Round($_.Length / 1MB, 1)) MB)"
    }
}

# Content hashes for every archive (decision: content hash, auto & authoritative).
Log "Computing SHA-256 hashes..."
$sums = Get-ChildItem $releases -File |
    Where-Object { $_.Name -like 'streamfind-*.zip' -or $_.Name -like 'streamfind-*.tgz' -or $_.Name -like 'streamfind-*.tar.gz' } |
    Sort-Object Name |
    ForEach-Object {
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($_.Name)"
}
$sums | Set-Content (Join-Path $releases 'sha256sums.txt')
Log "=== Done. Archives in $releases ==="
Get-ChildItem $releases -File | Sort-Object Name | ForEach-Object {
    Log ("  {0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB))
}