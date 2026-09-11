<#
    build-cpp.ps1 — configure and build the standalone C++ backend (cpp/) with
    Ninja into tmp/build/core-default, then (optionally) run the CTest suite.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1 -Clean
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1 -Tests
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\build-cpp.ps1 -Target streamfind_mcp

    Flags:
      -Clean    wipe the build tree first
      -Tests    after building, run ctest --output-on-failure
      -Target   a specific CMake target to build (default: all)
      -CMakeArgs additional configure arguments, e.g. -CMakeArgs '-DNAME=value'
      -Config   Debug|Release (default Debug)
#>
param(
    [switch]$Clean,
    [switch]$Tests,
    [string]$Target = '',
    [string[]]$CMakeArgs = @(),
    [string]$Config = 'Debug'
)

. "$PSScriptRoot\..\build-common.ps1"
Start-ScriptLog 'build-cpp'
$cmake   = Get-CMake
$ninja   = Get-Ninja
$buildDir = Join-Path $Script:TMP_BUILD 'core-default'
$srcDir   = Join-Path $Script:REPO_ROOT 'cpp'

Write-Log "cmake : $cmake"
Write-Log "ninja : $ninja"
Write-Log "build : $buildDir"
Write-Log "source: $srcDir"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Log "Cleaning build tree: $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

# Ninja targets need the MSVC environment (cl.exe, link.exe, rc.exe).
Invoke-VcvarsAll x64

$configureArgs = @(
    '-G', 'Ninja',
    '-Wno-dev',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_BUILD_TYPE=$Config",
    '-DSTREAMFIND_BUILD_TESTS=ON',
    '-DSTREAMFIND_BUILD_SHARED=OFF',
    "-B $buildDir", "-S $srcDir"
)
$configureArgs += $CMakeArgs
Write-Log "Configuring: cmake $($configureArgs -join ' ')"
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

$buildArgs = @('--build', $buildDir, '--config', $Config)
if ($Target) { $buildArgs += @('--target', $Target) }
Write-Log "Building: cmake $($buildArgs -join ' ')"
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
Write-Log 'C++ core build succeeded.'

if ($Tests) {
    Write-Log 'Running CTest...'
    $ctest = Get-CTest
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed ($LASTEXITCODE)" }
    Write-Log 'CTest passed.'
}

Write-Log "Done. Build tree: $buildDir"