#requires -Version 7.0
param([string]$Root = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'msys2-common.ps1')
$managedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
if (!$Root) { $Root = Get-StreamFindMSYS2Root $managedHome }
$Root = [IO.Path]::GetFullPath($Root)
$bash = Join-Path $Root 'usr/bin/bash.exe'
$freshInstall = $false

if (!(Test-Path -LiteralPath $bash -PathType Leaf)) {
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or !$IsWindows) {
        throw 'Managed MSYS2 installation currently supports Windows x64.'
    }
    $version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'msys2-version.txt') -Raw).Trim()
    $installerName = "msys2-x86_64-$version.exe"
    $installerUrl = "https://repo.msys2.org/distrib/x86_64/$installerName"
    $stagingRoot = Join-Path $managedHome "tools/.staging/msys2-$([guid]::NewGuid().ToString('N'))"
    $installer = Join-Path $stagingRoot $installerName
    [void][IO.Directory]::CreateDirectory($stagingRoot)
    try {
        Invoke-WebRequest -Uri $installerUrl -OutFile $installer
        $signature = Get-AuthenticodeSignature -FilePath $installer
        if ($signature.Status -ne 'Valid') { throw "MSYS2 installer signature validation failed: $($signature.Status)" }
        $rootArgument = $Root.Replace('\', '/')
        & $installer in --confirm-command --accept-messages --root $rootArgument
        if ($LASTEXITCODE -ne 0) { throw "MSYS2 installer failed with exit code $LASTEXITCODE" }
        if (!(Test-Path -LiteralPath $bash -PathType Leaf)) { throw "MSYS2 installer completed but bash.exe was not found under $Root" }
        $freshInstall = $true
    } finally {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$pacman = Join-Path $Root 'usr/bin/pacman.exe'
if (!(Test-Path -LiteralPath $pacman -PathType Leaf)) { throw "MSYS2 pacman was not found under $Root" }
$ucrtBin = Join-Path $Root 'ucrt64/bin'
$msysBin = Join-Path $Root 'usr/bin'
$previousPath = $env:PATH
$previousMSYSTEM = $env:MSYSTEM
try {
    $env:MSYSTEM = 'UCRT64'
    $env:PATH = "$ucrtBin;$msysBin;$previousPath"
    if ($freshInstall) {
        & $pacman -Syu --noconfirm
        if ($LASTEXITCODE -ne 0) {
            throw 'The initial MSYS2 system upgrade did not finish. Run sfdev setup --components native again to complete the upgrade.'
        }
    }
    $packages = @(
        'mingw-w64-ucrt-x86_64-gcc',
        'mingw-w64-ucrt-x86_64-cmake',
        'mingw-w64-ucrt-x86_64-ninja',
        'mingw-w64-ucrt-x86_64-arrow'
    )
    & $pacman -S --needed --noconfirm @packages
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 package installation failed with exit code $LASTEXITCODE" }
} finally {
    $env:PATH = $previousPath
    $env:MSYSTEM = $previousMSYSTEM
}

foreach ($tool in @('gcc.exe', 'g++.exe', 'cmake.exe', 'ninja.exe')) {
    $path = Join-Path $ucrtBin $tool
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "MSYS2 package setup did not provide $path" }
}
if (!(Test-Path -LiteralPath (Join-Path $Root 'ucrt64/include/arrow/api.h') -PathType Leaf)) {
    throw 'MSYS2 package setup did not provide the Apache Arrow headers.'
}
[Environment]::SetEnvironmentVariable('STREAMFIND_MSYS2_ROOT', $Root, 'User')
Write-Host "MSYS2 UCRT64 tools are ready at $Root"
Write-Host 'STREAMFIND_MSYS2_ROOT was set for the current Windows user; restart VS Code to use the new value there.'
