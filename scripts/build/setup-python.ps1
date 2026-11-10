#requires -Version 7.0
param()
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$managedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
$venv = Join-Path $repoRoot '.venv'
$venvPython = Join-Path $venv $(if ($IsWindows) { 'Scripts/python.exe' } else { 'bin/python' })

if (Test-Path -LiteralPath $venvPython -PathType Leaf) {
    $existingVersion = (& $venvPython -c 'import sys; print(".".join(map(str, sys.version_info[:3])))' 2>$null | Select-Object -First 1)
    if ($LASTEXITCODE -eq 0 -and $existingVersion -match '^(\d+)\.(\d+)\.(\d+)$' -and
        [version]$existingVersion -ge [version]'3.11' -and [version]$existingVersion -lt [version]'3.15') {
        Write-Host "Repository Python environment is ready: $venvPython (Python $existingVersion)"
        exit 0
    }
    throw "The existing repository .venv is not a supported Python 3.11–3.14 environment. Preserve it, or remove it deliberately before running setup again."
}

. (Join-Path $PSScriptRoot 'python-common.ps1')
$base = Get-StreamFindBasePython
$pin = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'python-version.txt') -Raw).Trim()
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'X64' -and ($IsWindows -or $IsLinux)) {
    $managedPlatform = if ($IsWindows) { 'win-x64' } else { 'linux-x64' }
    $managedDestination = Join-Path $managedHome "tools/python/$pin/$managedPlatform"
    $managedExecutable = if ($IsWindows) { Join-Path $managedDestination 'python.exe' }
        else { Join-Path $managedDestination 'bin/python3.11' }
    if (!$base -and (Test-Path -LiteralPath $managedExecutable -PathType Leaf)) {
        & $managedExecutable -c 'import venv, ensurepip' *> $null
        if ($LASTEXITCODE -ne 0) { throw "Managed Python exists but failed validation: $managedExecutable" }
        $base = [pscustomobject]@{ Path = $managedExecutable; PrefixArguments = @(); Version = $pin.Split('+')[0] }
    }
}
if (!$base) {
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or (!$IsWindows -and !$IsLinux)) {
        throw 'Managed Python setup currently supports Windows and Linux x64.'
    }
    $platform = if ($IsWindows) { 'win-x64' } else { 'linux-x64' }
    $asset = if ($IsWindows) {
        "cpython-$pin-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"
    } else {
        "cpython-$pin-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
    }
    $sha256 = if ($IsWindows) {
        '67215a7bf1643e33d01b17ee3f1e91a8b0b39163746855ee02594bf0608f68b4'
    } else {
        '17c4978a36c496dc53c08899ef8dad1ad2734f6325f08d6033a1e66419e0af26'
    }
    $releaseTag = ($pin -split '\+')[-1]
    $urlAsset = $asset.Replace('+', '%2B')
    $url = "https://github.com/astral-sh/python-build-standalone/releases/download/$releaseTag/$urlAsset"
    $destination = Join-Path $managedHome "tools/python/$pin/$platform"
    if (Test-Path -LiteralPath $destination) { throw "Managed Python destination exists but is incomplete: $destination" }
    $scratch = Join-Path $repoRoot "tmp/scratch/python-$([guid]::NewGuid().ToString('N'))"
    [void][IO.Directory]::CreateDirectory($scratch)
    $archive = Join-Path $scratch $asset
    try {
        Invoke-WebRequest -Uri $url -OutFile $archive
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $sha256) {
            throw 'Managed Python archive SHA-256 mismatch.'
        }
        & tar -xzf $archive -C $scratch
        if ($LASTEXITCODE -ne 0) { throw "Managed Python archive extraction failed ($LASTEXITCODE)." }
        $pythonName = if ($IsWindows) { 'python.exe' } else { 'python3.11' }
        $extractedPython = Get-ChildItem -LiteralPath $scratch -Recurse -File -Filter $pythonName |
            Where-Object { $_.FullName -ne $archive } | Select-Object -First 1
        if (!$extractedPython) { throw "Managed Python archive did not contain $pythonName." }
        $extractedRoot = if ($IsWindows) {
            Split-Path -Parent $extractedPython.FullName
        } else {
            Split-Path -Parent (Split-Path -Parent $extractedPython.FullName)
        }
        $probe = & $extractedPython.FullName -c 'import sys, venv; print(sys.version_info[:3])' 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Extracted managed Python failed validation: $probe" }
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
        Move-Item -LiteralPath $extractedRoot -Destination $destination
        $basePath = if ($IsWindows) { Join-Path $destination 'python.exe' } else { Join-Path $destination 'bin/python3.11' }
        if (!(Test-Path -LiteralPath $basePath -PathType Leaf)) { throw "Installed managed Python executable is missing: $basePath" }
        $base = [pscustomobject]@{ Path = $basePath; PrefixArguments = @(); Version = $pin.Split('+')[0] }
        Write-Host "Installed managed Python under $destination"
    } finally {
        Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "Using installed Python $($base.Version) at $($base.Path)"
}

$basePrefixArguments = @($base.PrefixArguments)
& $base.Path @basePrefixArguments -m venv $venv
if ($LASTEXITCODE -ne 0) { throw "Could not create repository virtual environment at $venv (exit $LASTEXITCODE)." }
if (!(Test-Path -LiteralPath $venvPython -PathType Leaf)) { throw "Virtual environment creation did not produce $venvPython" }
$createdVersion = (& $venvPython -c 'import sys; print(".".join(map(str, sys.version_info[:3])))' 2>$null | Select-Object -First 1)
if ($LASTEXITCODE -ne 0 -or $createdVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$' -or
    [version]$createdVersion -lt [version]'3.11' -or [version]$createdVersion -ge [version]'3.15') {
    throw "Created repository Python environment is not supported: $createdVersion"
}
Write-Host "Created repository Python environment: $venvPython (Python $createdVersion)"
