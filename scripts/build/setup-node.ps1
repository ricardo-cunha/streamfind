#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$managedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
$toolsRoot = Join-Path $managedHome 'tools'
$version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'node-version.txt') -Raw).Trim()
$architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($architecture -ne 'X64' -or (!$IsWindows -and !$IsLinux)) {
    throw 'Managed Node.js setup currently supports Windows x64 and Linux x64.'
}
$asset = if ($IsWindows) { "node-v$version-win-x64.zip" } else { "node-v$version-linux-x64.tar.xz" }
# SHA-256 values are pinned from the official Node.js v22.23.4 SHASUMS256.txt.
$archiveSha256 = switch ($asset) {
    'node-v22.23.4-win-x64.zip' { 'cbc74b5e0982c9dbb41525fd8fb5dcd32fbee237863f7829072f4639a1ce5f20' }
    'node-v22.23.4-linux-x64.tar.xz' { '1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce' }
    default { throw "No verified Node.js archive is configured for $asset" }
}
$platform = if ($IsWindows) { 'win-x64' } else { 'linux-x64' }
$destination = Join-Path $toolsRoot "node/v$version/$platform"
$nodeName = if ($IsWindows) { 'node.exe' } else { 'node' }
$npmName = if ($IsWindows) { 'npm.cmd' } else { 'npm' }
$existingNode = Join-Path $destination $(if ($IsWindows) { $nodeName } else { "bin/$nodeName" })
$existingNpm = Join-Path $destination $(if ($IsWindows) { $npmName } else { "bin/$npmName" })
if ((Test-Path -LiteralPath $existingNode -PathType Leaf) -and (Test-Path -LiteralPath $existingNpm -PathType Leaf)) {
    $reportedVersion = (& $existingNode --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -eq 0 -and $reportedVersion -eq "v$version") {
        Write-Host "Managed Node.js already available at $destination"
        exit 0
    }
    throw "Managed Node.js installation exists but failed version validation: $destination"
}
if (Test-Path -LiteralPath $destination) { throw "Managed Node.js destination exists but is incomplete: $destination" }

$stagingRoot = Join-Path $toolsRoot ".staging/node-$([guid]::NewGuid().ToString('N'))"
$archive = Join-Path $stagingRoot $asset
$uri = "https://nodejs.org/dist/v$version/$asset"
[void][IO.Directory]::CreateDirectory($stagingRoot)
try {
    Invoke-WebRequest -Uri $uri -OutFile $archive
    $actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $archiveSha256) { throw "Node.js archive SHA-256 mismatch: $asset" }

    if ($IsWindows) { Expand-Archive -LiteralPath $archive -DestinationPath $stagingRoot }
    else {
        & tar -xJf $archive -C $stagingRoot
        if ($LASTEXITCODE -ne 0) { throw "Node.js archive extraction failed ($LASTEXITCODE)" }
    }
    $extracted = Join-Path $stagingRoot "node-v$version-$platform"
    $node = Join-Path $extracted $(if ($IsWindows) { $nodeName } else { "bin/$nodeName" })
    $npm = Join-Path $extracted $(if ($IsWindows) { $npmName } else { "bin/$npmName" })
    if (!(Test-Path -LiteralPath $node -PathType Leaf) -or !(Test-Path -LiteralPath $npm -PathType Leaf)) {
        throw 'The verified Node.js archive did not contain the expected node and npm executables.'
    }
    $reportedVersion = (& $node --version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $reportedVersion -ne "v$version") { throw "Extracted Node.js version check failed: $reportedVersion" }
    $previousPath = $env:PATH
    try {
        $nodeBin = if ($IsWindows) { $extracted } else { Join-Path $extracted 'bin' }
        $env:PATH = if ($IsWindows) { "$nodeBin;$previousPath" } else { "$nodeBin`:$previousPath" }
        & $npm --version
        if ($LASTEXITCODE -ne 0) { throw 'The bundled npm executable failed validation.' }
    } finally { $env:PATH = $previousPath }

    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    Move-Item -LiteralPath $extracted -Destination $destination
    Write-Host "Installed managed Node.js at $destination"
} finally {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
