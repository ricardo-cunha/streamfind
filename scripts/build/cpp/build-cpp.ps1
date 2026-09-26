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
$buildDir = Join-Path $Script:TMP_BUILD 'core-default'
$srcDir   = Join-Path $Script:REPO_ROOT 'cpp'

Write-Log "build : $buildDir"
Write-Log "source: $srcDir"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Log "Cleaning build tree: $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

$toolchain = Initialize-MinGWUcrt64
$cmake = $toolchain.CMake
$ninja = $toolchain.Ninja
Write-Log "MinGW root: $($toolchain.Root)"
Write-Log "C compiler: $($toolchain.CCompiler)"
Write-Log "C++ compiler: $($toolchain.CxxCompiler)"

$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $cache = Get-Content -LiteralPath $cacheFile -Raw
    $expectedCCompiler = [Regex]::Escape($toolchain.CCompiler.Replace('\', '/'))
    $expectedCxxCompiler = [Regex]::Escape($toolchain.CxxCompiler.Replace('\', '/'))
    $mixedToolchain =
        $cache -notmatch "CMAKE_GENERATOR:INTERNAL=Ninja" -or
        $cache -notmatch "CMAKE_C_COMPILER:.*$expectedCCompiler" -or
        $cache -notmatch "CMAKE_CXX_COMPILER:.*$expectedCxxCompiler" -or
        $cache -match 'CMAKE_C_FLAGS:.*(/DWIN32|/D_WINDOWS)' -or
        $cache -match 'CMAKE_CXX_FLAGS:.*(/DWIN32|/D_WINDOWS|/EHsc)'
    if ($mixedToolchain) {
        Write-Log "Discarding mixed-toolchain CMake cache: $buildDir"
        Remove-Item -Recurse -Force $buildDir
    }
}

$configureArgs = @(
    '-G', 'Ninja',
    '-Wno-dev',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_C_COMPILER=$($toolchain.CCompiler)",
    "-DCMAKE_CXX_COMPILER=$($toolchain.CxxCompiler)",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DSTREAMFIND_MINGW_RUNTIME_DIR=$($toolchain.Bin)",
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
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