<#
    publish-release.ps1 — publish verified archives from tmp/release-output to
    an existing or new GitHub Release.

    This script does not build or test. Run the C++ and/or Rust release script
    first, review the output, then run this script when the release is ready.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\release\publish-release.ps1 -Version 0.2.0
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
    [ValidateSet('Cpp', 'Rust', 'All')]
    [string]$Backend = 'All',
    [switch]$Replace
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $root 'tmp\release-output'
$tag = "v$Version"

$gh = Get-Command gh.exe -ErrorAction SilentlyContinue
if (-not $gh) { $gh = Get-Command gh -ErrorAction SilentlyContinue }
if (-not $gh) {
    throw 'GitHub CLI (gh) is required. Install it and run gh auth login first.'
}

& $gh.Path auth status --hostname github.com
if ($LASTEXITCODE -ne 0) {
    throw 'GitHub CLI is not authenticated. Run gh auth login and retry.'
}

if (-not (Test-Path $output)) {
    throw "Release output directory not found: $output. Run scripts\release\cpp\release-cpp.ps1 or scripts\release\rust\release-rust.ps1 first."
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
if (($Backend -in @('Cpp', 'All')) -and -not ($archives | Where-Object { $_.Name -like "streamfind-core-cpp-$Version-*" })) {
    throw "The C++ archive for $Version is missing from $output."
}
if (($Backend -in @('Rust', 'All')) -and -not ($archives | Where-Object { $_.Name -like "streamfind-rust-$Version-*" })) {
    throw "The Rust archive for $Version is missing from $output."
}
$archives = @($archives | Where-Object {
    $Backend -eq 'All' -or ($Backend -eq 'Cpp' -and $_.Name -like "streamfind-core-cpp-$Version-*") -or ($Backend -eq 'Rust' -and $_.Name -like "streamfind-rust-$Version-*")
})

$checksum = Join-Path $output 'sha256sums.txt'
if (-not (Test-Path $checksum)) {
    throw "Checksum file not found: $checksum"
}

$checksumLines = Get-Content $checksum
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
}

$assets = @($archives.FullName) + $checksum
Write-Host "Validated $($archives.Count) archive(s) and sha256sums.txt for $tag."
Write-Host "Repository: $Repository"

if ($Replace) {
    Write-Host "Replacing assets in existing GitHub Release $tag..."
    & $gh.Path release upload $tag @assets --repo $Repository --clobber
    if ($LASTEXITCODE -ne 0) { throw "GitHub Release asset upload failed ($LASTEXITCODE)." }
} else {
    Write-Host "Creating GitHub Release $tag..."
    $notes = "Development release of the independent native C++ and Rust backends. See the documentation for package contents and compatibility scope."
    & $gh.Path release create $tag @assets --repo $Repository --title "streamfind $Version" --notes $notes --generate-notes
    if ($LASTEXITCODE -ne 0) { throw "GitHub Release creation failed ($LASTEXITCODE)." }
}

Write-Host "GitHub Release operation completed for $tag."
