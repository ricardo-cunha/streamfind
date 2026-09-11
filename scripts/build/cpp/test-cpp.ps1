<#
    test-cpp.ps1 — run the C++ backend CTest suite against the build tree in
    tmp/build/core-default. Build first with build-cpp.ps1 (or with -Tests).

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\test-cpp.ps1
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\test-cpp.ps1 -Config Release
#>
param(
    [string]$Config = 'Debug'
)

. "$PSScriptRoot\..\build-common.ps1"
Start-ScriptLog 'test-cpp'

$ctest    = Get-CTest
$buildDir = Join-Path $Script:TMP_BUILD 'core-default'
if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
    throw "No CMake build tree at $buildDir - run scripts\build\cpp\build-cpp.ps1 first."
}

Write-Log "ctest: $ctest"
Write-Log "ctest dir: $buildDir (config $Config)"
$ctestArgs = @('--test-dir', $buildDir, '-C', $Config, '--output-on-failure')
& $ctest @ctestArgs
if ($LASTEXITCODE -ne 0) { throw "CTest failed ($LASTEXITCODE)" }
Write-Log 'CTest passed.'