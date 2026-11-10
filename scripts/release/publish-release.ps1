<#
    publish-release.ps1 — publish verified archives from tmp/release-output to
    an existing or new GitHub Release.

    This script does not build or test. Run the C++ release script
    first, review the output, then run this script when the release is ready.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\release\publish-release.ps1 -Version 0.4.0
      powershell -ExecutionPolicy Bypass -File scripts\release\publish-release.ps1 -Version 0.1.0 -Replace

    By default, the script creates a new v<Version> GitHub Release and fails
    if that release already exists. Use -Replace explicitly to overwrite assets
    in an existing release.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version,
    [string]$Repository = 'ricardo-cunha/streamfind',
    [switch]$Replace,
    [switch]$VerifyOnly,
    [switch]$RequireProvenance
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $root 'tmp\release-output'
$tag = "v$Version"

if (-not (Test-Path $output)) {
    throw "Release output directory not found: $output. Run scripts\release\cpp\release-cpp.ps1 first."
}

$escapedVersion = [regex]::Escape($Version)
$archives = @(
    Get-ChildItem $output -File |
        Where-Object {
            $_.Name -match "^streamfind-.+-${escapedVersion}-(Windows|Linux)-.+\.(zip|tgz|tar\.gz)$"
        } |
        Sort-Object Name
)
if ($archives.Count -eq 0) {
    throw "No versioned release archives found for $Version in $output."
}
if (-not ($archives | Where-Object { $_.Name -like "streamfind-core-cpp-$Version-*" })) {
    throw "The C++ archive for $Version is missing from $output."
}
$archives = @($archives | Where-Object { $_.Name -like "streamfind-core-cpp-$Version-*" })
if ($RequireProvenance) {
    foreach ($platformName in 'Windows', 'Linux') {
        if (@($archives | Where-Object Name -like "*-$platformName-*").Count -ne 1) {
            throw "Expected one runtime archive for $platformName before publishing"
        }
    }
}

$checksum = Join-Path $output 'sha256sums.txt'
if (-not (Test-Path $checksum)) {
    throw "Checksum file not found: $checksum"
}

$checksumLines = Get-Content $checksum
$sourceCommit = $null
foreach ($archive in $archives) {
    $line = $checksumLines |
        Where-Object { $_ -match "^[0-9a-fA-F]{64}\s+\*?$([regex]::Escape($archive.Name))$" } |
        Select-Object -First 1
    if (-not $line) {
        throw "No checksum entry found for $($archive.Name)."
    }
    $expected = ($line -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($expected -ne $actual) {
        throw "Checksum mismatch for $($archive.Name): expected $expected, got $actual."
    }
    $provenancePath = "$($archive.FullName).provenance.json"
    if ($RequireProvenance -or (Test-Path -LiteralPath $provenancePath)) {
        $record = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
        if ($record.version -ne $Version -or $record.archive -ne $archive.Name -or
            $record.sha256 -ne $actual -or $record.commit -notmatch '^[0-9a-f]{40}$') {
            throw "Invalid provenance for $($archive.Name)"
        }
        if ($sourceCommit -and $sourceCommit -ne $record.commit) { throw 'Release archives were built from different commits' }
        $sourceCommit = $record.commit
    }
}

$assets = @($archives.FullName) + $checksum
Write-Host "Validated $($archives.Count) archive(s) and sha256sums.txt for $tag."
if ($VerifyOnly) {
    $platform = if ([Environment]::OSVersion.Platform -eq 'Win32NT') { 'Windows' } else { 'Linux' }
    $hostArchive = @($archives | Where-Object Name -like "*-$platform-*")
    if ($hostArchive.Count -ne 1) { throw "Expected one runtime archive for $platform" }
    $scratch = Join-Path $root "tmp/scratch/release-verify-$([guid]::NewGuid().ToString('N'))"
    [void][IO.Directory]::CreateDirectory($scratch)
    try {
        & tar -xf $hostArchive[0].FullName -C $scratch
        if ($LASTEXITCODE) { throw 'Runtime archive extraction failed' }
        $packages = @(Get-ChildItem -LiteralPath $scratch -Directory)
        if ($packages.Count -ne 1) { throw 'Expected one extracted runtime directory' }
        $packageRoot = $packages[0].FullName
        $python = Join-Path $root $(if ($platform -eq 'Windows') { '.venv/Scripts/python.exe' } else { '.venv/bin/python' })
        & $python -c 'import sys; print(sys.executable)'
        if ($LASTEXITCODE) { throw 'Repository Python environment unavailable' }
        & $python (Join-Path $root 'scripts/dev/validate_native_dependencies.py') $packageRoot --platform $platform.ToLowerInvariant()
        if ($LASTEXITCODE) { throw 'Native dependency closure validation failed' }
        if ($platform -eq 'Windows') {
            $shell = Join-Path $PSHOME 'pwsh.exe'
            & $shell -NoProfile -File (Join-Path $PSScriptRoot 'cpp/test-packaged-mcp.ps1') -PackageRoot $packageRoot
        } else {
            & $python (Join-Path $PSScriptRoot 'cpp/test-packaged-mcp-linux.py') $packageRoot
        }
        if ($LASTEXITCODE) { throw 'Packaged MCP smoke test failed' }
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
    return
}
Write-Host "Repository: $Repository"

$gh = Get-Command gh -CommandType Application -ErrorAction Stop
& $gh.Source auth status --hostname github.com
if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI is not authenticated. Run gh auth login and retry.' }
if ($sourceCommit) {
    $reference = "refs/tags/$tag"
    $remoteRefs = & git ls-remote "https://github.com/$Repository.git" $reference ($reference + '^{}')
    if ($LASTEXITCODE -ne 0) { throw 'Could not verify the remote release tag' }
    $commits = @($remoteRefs | ForEach-Object { ($_ -split "\s+", 2)[0] })
    if ($sourceCommit -notin $commits) { throw "Remote tag $tag does not match release archive provenance" }
}

if ($Replace) {
    Write-Host "Replacing assets in existing GitHub Release $tag..."
    & $gh.Path release upload $tag @assets --repo $Repository --clobber
    if ($LASTEXITCODE -ne 0) { throw "GitHub Release asset upload failed ($LASTEXITCODE)." }
} else {
    Write-Host "Creating GitHub Release $tag..."
    $notes = "Development release of the native C++ backend for Windows and Linux. See the documentation for package contents and compatibility scope."
    & $gh.Path release create $tag @assets --verify-tag --repo $Repository --title "streamfind $Version" --notes $notes --generate-notes
    if ($LASTEXITCODE -ne 0) { throw "GitHub Release creation failed ($LASTEXITCODE)." }
}

Write-Host "GitHub Release operation completed for $tag."
