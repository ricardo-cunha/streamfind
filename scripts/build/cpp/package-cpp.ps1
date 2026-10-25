<#
    Build and unpack a local Windows distribution for manual testing.

    Unlike the release script, this writes only to tmp/packaged_build and does
    not create an archive, tag, checksum, or GitHub release asset.

    Usage:
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\package-cpp.ps1
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\package-cpp.ps1 -SkipTests
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\package-cpp.ps1 -Plugins mass_spec,statistics
#>
[CmdletBinding()]
param(
    [switch]$SkipTests,
    [string]$Plugins = 'ALL',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
    [switch]$CleanBuild
)

. "$PSScriptRoot\..\build-common.ps1"
. "$PSScriptRoot\..\..\release\release-common.ps1"

Start-ScriptLog 'package-cpp'
Set-RepositoryTemp
$root = $Script:REPO_ROOT
$version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
$env:STREAMFIND_PACKAGE_VERSION = $version
$buildDir = Join-Path $root 'tmp\build\mingw-ucrt64'
$packageBuild = Join-Path $root 'tmp\packaged_build'
$frontendDir = Join-Path $root 'frontend'
$frontendDist = Join-Path $frontendDir 'dist'

if ($CleanBuild -and (Test-Path $buildDir)) {
    Write-Log "Cleaning build tree: $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

$toolchain = Initialize-MinGWUcrt64
$cmake = $toolchain.CMake
$ninja = $toolchain.Ninja

Write-Log 'Building frontend application...'
Push-Location $frontendDir
try {
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw "Frontend build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
if (-not (Test-Path (Join-Path $frontendDist 'index.html'))) {
    throw 'Frontend build did not produce dist/index.html'
}

Write-Log "Configuring C++ package build ($Config)..."
& $cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_C_COMPILER=$($toolchain.CCompiler)" `
    "-DCMAKE_CXX_COMPILER=$($toolchain.CxxCompiler)" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DSTREAMFIND_MINGW_RUNTIME_DIR=$($toolchain.Bin)" `
    "-DCMAKE_PREFIX_PATH=$($toolchain.Root)\ucrt64" `
    "-DSTREAMFIND_ENABLED_PLUGINS=$($Plugins -replace ',', ';')" `
    '-DSTREAMFIND_BUILD_TESTS=ON' '-DSTREAMFIND_BUILD_SHARED=OFF' `
    '-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=OFF' `
    "-DSTREAMFIND_APP_DIR=$frontendDist" `
    "-B $buildDir" "-S $(Join-Path $root 'cpp')"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

& $cmake --build $buildDir --config $Config -j 8
if ($LASTEXITCODE -ne 0) { throw "C++ build failed ($LASTEXITCODE)" }
if (-not $SkipTests) {
    $ctest = Get-CTest
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "C++ CTest failed ($LASTEXITCODE)" }
}

$cpackDir = Join-Path $root 'tmp\build\cpack-out-packaged'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cpackDir
& (Get-CPack) -G ZIP -C $Config -B $cpackDir --config (Join-Path $buildDir 'CPackConfig.cmake')
if ($LASTEXITCODE -ne 0) { throw "CPack failed ($LASTEXITCODE)" }
$archive = Get-ChildItem $cpackDir -Filter "streamfind-core-cpp-$version-Windows-*.zip" | Select-Object -First 1
if (-not $archive) { throw "C++ package archive was not produced for $version" }

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $packageBuild
New-Item -ItemType Directory -Path $packageBuild -Force | Out-Null
$extractDir = Join-Path $root 'tmp\build\package-verify-local'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $extractDir
Expand-Archive -LiteralPath $archive.FullName -DestinationPath $extractDir
$packageRoot = Get-ChildItem -Path $extractDir -Directory | Select-Object -First 1
if (-not $packageRoot) { throw 'C++ package archive has no top-level directory' }
Copy-Item -Recurse -Force (Join-Path $packageRoot.FullName '*') $packageBuild
Assert-CppDistributionPayload $packageBuild

Write-Log "Packaged build ready: $packageBuild"
Write-Log "Run: $packageBuild\bin\streamfind-app.exe"
