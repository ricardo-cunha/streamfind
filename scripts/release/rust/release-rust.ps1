<# Build and package the alternative Rust backend against the C++ catalogue. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')][string]$Version,
    [Parameter(Mandatory = $true)][string]$CppCatalogue,
    [switch]$SkipTests
)

. "$PSScriptRoot\..\..\build\build-common.ps1"
. "$PSScriptRoot\..\release-common.ps1"
Invoke-SemanticChecks
Invoke-VcvarsAll -Arch 'x64'

$catalogue = (Resolve-Path $CppCatalogue -ErrorAction Stop).Path
if (-not (Test-Path $catalogue -PathType Leaf)) { throw "C++ catalogue not found: $CppCatalogue" }
$env:STREAMFIND_CATALOGUE = $catalogue
$env:CARGO_TARGET_DIR = Join-Path $Script:REPO_ROOT 'tmp\build\rust-target'
$duckdbRoot = Join-Path $Script:REPO_ROOT 'cpp\vendor\duckdb'
$env:DUCKDB_INCLUDE_DIR = Join-Path $duckdbRoot 'include'
$env:DUCKDB_LIB_DIR = Join-Path $duckdbRoot 'lib\windows-x64'
$env:DUCKDB_STATIC = '0'
$cargo = Get-Cargo
$rustDir = Join-Path $Script:REPO_ROOT 'rust'
Push-Location $rustDir
try {
    & $cargo build --release --workspace --exclude streamfind-rust-test-support
    if ($LASTEXITCODE -ne 0) { throw "Cargo build failed ($LASTEXITCODE)" }
    if (-not $SkipTests) {
        & $cargo test --workspace
        if ($LASTEXITCODE -ne 0) { throw "Rust tests failed ($LASTEXITCODE)" }
    }
} finally { Pop-Location }

$packagedRoot = Join-Path $Script:REPO_ROOT "tmp\staging\streamfind-rust-$Version-Windows-x86_64"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $packagedRoot
New-Item -ItemType Directory -Force -Path (Join-Path $packagedRoot 'bin') | Out-Null
Copy-Item (Join-Path $env:CARGO_TARGET_DIR 'release\streamfind-rust-cli.exe') (Join-Path $packagedRoot 'bin')
Copy-Item (Join-Path $env:CARGO_TARGET_DIR 'release\streamfind-rust-mcp.exe') (Join-Path $packagedRoot 'bin')
Copy-Item (Join-Path $Script:REPO_ROOT 'cpp\vendor\duckdb\lib\windows-x64\duckdb.dll') (Join-Path $packagedRoot 'bin')
$share = Join-Path $packagedRoot 'share\streamfind'
New-Item -ItemType Directory -Force -Path $share | Out-Null
Copy-Item $catalogue $share
Copy-Item (Join-Path $Script:REPO_ROOT 'LICENSE.md') $packagedRoot
Copy-Item (Join-Path $Script:REPO_ROOT 'NOTICE.md') $packagedRoot
Copy-Item (Join-Path $rustDir 'LICENSES.md') $packagedRoot
Assert-DistributionPayload $packagedRoot 'LICENSES.md'
$zip = Join-Path $Script:RELEASE_OUTPUT "streamfind-rust-$Version-Windows-x86_64.zip"
Remove-Item -Force -ErrorAction SilentlyContinue $zip
Compress-Archive -Path $packagedRoot -DestinationPath $zip -CompressionLevel Optimal -Force
Write-ReleaseChecksums
Write-ReleaseLog "Rust archive written to $Script:RELEASE_OUTPUT"
