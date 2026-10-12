<# Build the optional StreamFind backend/frontend plugin developer kit. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')][string]$Version,
    [switch]$SkipTests,
    [ValidateSet('Debug', 'Release')][string]$Config = 'Release'
)

. "$PSScriptRoot\..\..\build\build-common.ps1"
. "$PSScriptRoot\..\release-common.ps1"

$root = $Script:REPO_ROOT
$versionFile = Join-Path $root 'VERSION'
$canonicalVersion = (Get-Content -Raw $versionFile).Trim()
if ($canonicalVersion -ne $Version) {
    throw "Release version $Version does not match the canonical version $canonicalVersion"
}
$toolchain = Initialize-MinGWUcrt64
$buildDir = Join-Path $root 'tmp\build\mingw-plugin-devkit'
$installDir = Join-Path $buildDir 'install'
$outDir = Join-Path $root 'tmp\release-output'
$frontendDist = Join-Path $root 'frontend\dist'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $buildDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Push-Location (Join-Path $root 'frontend')
try {
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw "Frontend build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
& $toolchain.CMake -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$($toolchain.Ninja)" `
    "-DCMAKE_C_COMPILER=$($toolchain.CCompiler)" `
    "-DCMAKE_CXX_COMPILER=$($toolchain.CxxCompiler)" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DSTREAMFIND_MINGW_RUNTIME_DIR=$($toolchain.Bin)" `
    '-DSTREAMFIND_BUILD_TESTS=ON' '-DSTREAMFIND_BUILD_SHARED=OFF' `
    '-DSTREAMFIND_INSTALL_PLUGIN_DEVELOPMENT_KIT=ON' `
    "-DSTREAMFIND_APP_DIR=$frontendDist" `
    "-DCMAKE_INSTALL_PREFIX=$installDir" `
    "-B $buildDir" "-S $(Join-Path $root 'cpp')"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
& $toolchain.CMake --build $buildDir --config $Config --parallel 8
if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
if (-not $SkipTests) {
    & (Get-CTest) --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed ($LASTEXITCODE)" }
}
& $toolchain.CMake --install $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Developer-kit install failed ($LASTEXITCODE)" }
$python = Join-Path $root '.venv\Scripts\python.exe'
if (-not (Test-Path $python)) { throw "Repository Python environment is missing: $python" }
& $python (Join-Path $root 'scripts\dev\test_plugin_consumer.py') `
    --source (Join-Path $root 'cpp\sdk\examples\minimal_plugin') `
    --prefix $installDir `
    --build (Join-Path $buildDir 'consumer') `
    --cmake $toolchain.CMake `
    --ninja $toolchain.Ninja
if ($LASTEXITCODE -ne 0) { throw "Out-of-tree developer-kit consumer failed ($LASTEXITCODE)" }
$cpackDir = Join-Path $buildDir 'cpack'
& (Get-CPack) -G ZIP -C $Config -B $cpackDir --config (Join-Path $buildDir 'CPackConfig.cmake')
if ($LASTEXITCODE -ne 0) { throw "CPack failed ($LASTEXITCODE)" }
$archive = Get-ChildItem (Join-Path $cpackDir "streamfind-plugin-dev-$Version-Windows-*.zip") | Select-Object -First 1
if (-not $archive) { throw 'Developer-kit archive was not produced' }
Copy-Item $archive.FullName (Join-Path $outDir $archive.Name) -Force
Write-ReleaseChecksums
Write-ReleaseLog "Plugin developer kit written to $outDir"
