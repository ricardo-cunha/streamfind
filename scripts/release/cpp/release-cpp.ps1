<# Build and package the authoritative C++ backend release. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')][string]$Version,
    [switch]$SkipTests,
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release'
)

. "$PSScriptRoot\..\..\build\build-common.ps1"
. "$PSScriptRoot\..\release-common.ps1"

$root = $Script:REPO_ROOT
$env:STREAMFIND_PACKAGE_VERSION = $Version
$buildDir = Join-Path $root 'tmp\build\release-cpp'
$cmake = Get-CMake
$ninja = Get-Ninja
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $buildDir
Invoke-VcvarsAll x64
Write-ReleaseLog "Building C++ backend ($Config)..."
& $cmake -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$Config" `
    -DSTREAMFIND_BUILD_TESTS=ON -DSTREAMFIND_BUILD_SHARED=OFF "-B $buildDir" "-S $(Join-Path $root 'cpp')"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
& $cmake --build $buildDir --config $Config -j 8
if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
if (-not $SkipTests) {
    $ctest = Get-CTest
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "C++ CTest failed ($LASTEXITCODE)" }
}

$cpack = Get-CPack
$cpackDir = Join-Path $root 'tmp\build\cpack-out-cpp'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cpackDir
& $cpack -G ZIP -C $Config -B $cpackDir --config $buildDir/CPackConfig.cmake
if ($LASTEXITCODE -ne 0) { throw "CPack failed ($LASTEXITCODE)" }
$built = Get-ChildItem (Join-Path $cpackDir "streamfind-core-cpp-$Version-Windows-*.zip") | Select-Object -First 1
if (-not $built) { throw "C++ archive not produced for $Version" }
$verifyDir = Join-Path $root 'tmp\build\package-verify\cpp'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $verifyDir
Expand-Archive -Path $built.FullName -DestinationPath $verifyDir
$packageRoot = Get-ChildItem -Path $verifyDir -Directory | Select-Object -First 1
if (-not $packageRoot) { throw 'C++ archive has no top-level package directory' }
Assert-CppDistributionPayload $packageRoot.FullName
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\release\cpp\test-packaged-mcp.ps1') `
    -PackageRoot $packageRoot.FullName
if ($LASTEXITCODE -ne 0) { throw "Packaged C++ MCP smoke test failed ($LASTEXITCODE)" }
Move-Item $built.FullName (Join-Path $Script:RELEASE_OUTPUT $built.Name) -Force
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $cpackDir
Write-ReleaseChecksums
Write-ReleaseLog "C++ archive written to $Script:RELEASE_OUTPUT"
