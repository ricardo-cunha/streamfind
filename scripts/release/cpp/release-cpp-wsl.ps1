#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('prepare', 'verify')]
    [string]$Command = 'prepare',
    [Parameter(Mandatory)]
    [string]$Version,
    [string]$Distribution = 'Ubuntu',
    [string]$LinuxRoot
)

$ErrorActionPreference = 'Stop'
$scriptRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$root = (Resolve-Path -LiteralPath (Join-Path $scriptRoot '..\..\..')).Path
$worktreeName = Split-Path -Leaf $root

function Quote-Bash([string]$Value) {
    $replacement = "'" + [char]34 + "'" + [char]34 + "'"
    return "'" + $Value.Replace("'", $replacement) + "'"
}

function Invoke-WslCommand([string]$CommandText) {
    & wsl.exe --distribution $Distribution -- bash -lc $CommandText
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE"
    }
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw 'wsl.exe is required for Linux release preparation from Windows'
}

if (-not $LinuxRoot) {
    $linuxHome = (& wsl.exe --distribution $Distribution -- bash -lc 'printf %s "$HOME"').Trim()
    if ($LASTEXITCODE -ne 0 -or $linuxHome -notmatch '^/home/[^/]+$') {
        throw "Unable to resolve a native WSL home directory for distribution $Distribution"
    }
    $LinuxRoot = "$linuxHome/$worktreeName"
}
if ($LinuxRoot -notmatch '^/home/[^/]+/[^/]+$') {
    throw "LinuxRoot must be a worktree directory directly under a Linux user's home: $LinuxRoot"
}

$uncRoot = "\\wsl$\$Distribution$($LinuxRoot -replace '/', '\')"
New-Item -ItemType Directory -Force -Path $uncRoot | Out-Null

# Keep the native Linux mirror independent from Windows build outputs and
# package dependencies. The excluded directories remain owned by WSL.
$robocopyArguments = @(
    $root, $uncRoot, '/MIR', '/NFL', '/NDL', '/NJH', '/NJS', '/NP',
    '/XD', (Join-Path $root 'tmp'), (Join-Path $root '.git'),
    (Join-Path $root '.venv'), (Join-Path $root 'frontend\node_modules'),
    (Join-Path $root 'frontend\dist')
)
& robocopy @robocopyArguments
if ($LASTEXITCODE -ge 8) {
    throw "Unable to mirror the checkout into $LinuxRoot (robocopy exit code $LASTEXITCODE)"
}

$quotedRoot = Quote-Bash $LinuxRoot
$setup = "cd $quotedRoot && pwsh -NoProfile -File scripts/sfdev.ps1 setup --components python"
Invoke-WslCommand $setup

$archive = "streamfind-core-cpp-$Version-Linux-x86_64.tgz"
$linuxArchive = "$LinuxRoot/tmp/release-output/$archive"
$releaseScript = 'scripts/release/cpp/release-cpp-linux.sh'
if ($Command -eq 'prepare') {
    Invoke-WslCommand "cd $quotedRoot && bash $releaseScript $Version"
} else {
    $packageRoot = "$LinuxRoot/tmp/scratch/sfdev-linux-release-verify/streamfind-core-cpp-$Version-Linux-x86_64"
    $quotedPackageRoot = Quote-Bash $packageRoot
    $verify = @(
        "cd $quotedRoot",
        "test -f tmp/release-output/$archive",
        "rm -rf tmp/scratch/sfdev-linux-release-verify",
        "mkdir -p tmp/scratch/sfdev-linux-release-verify",
        "tar -xzf tmp/release-output/$archive -C tmp/scratch/sfdev-linux-release-verify",
        ".venv/bin/python scripts/dev/validate_native_dependencies.py $quotedPackageRoot --platform linux",
        ".venv/bin/python scripts/release/cpp/test-packaged-mcp-linux.py $quotedPackageRoot"
    ) -join ' && '
    Invoke-WslCommand $verify
}

if (-not (Test-Path -LiteralPath $uncRoot -PathType Container)) {
    throw "WSL checkout mirror is missing: $LinuxRoot"
}
if (-not (Test-Path -LiteralPath ("$uncRoot\tmp\release-output\$archive") -PathType Leaf)) {
    throw "Linux release archive was not produced: $linuxArchive"
}

$windowsOutput = Join-Path $root 'tmp\release-output'
New-Item -ItemType Directory -Force -Path $windowsOutput | Out-Null
Copy-Item -LiteralPath (Join-Path $uncRoot "tmp\release-output\$archive") `
    -Destination (Join-Path $windowsOutput $archive) -Force

$releaseFiles = Get-ChildItem -LiteralPath $windowsOutput -File | Where-Object {
    $_.Name -match "^streamfind-.*-$([regex]::Escape($Version))-(Windows|Linux)-x86_64\.(zip|tgz|tar\.gz)$"
} | Sort-Object Name
if (-not $releaseFiles) { throw "No release archives found under $windowsOutput" }
$checksums = foreach ($file in $releaseFiles) {
    "{0}  {1}" -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $file.Name
}
$checksums | Set-Content -LiteralPath (Join-Path $windowsOutput 'sha256sums.txt') -Encoding ascii
Write-Output "Linux release exported to $windowsOutput\$archive"
