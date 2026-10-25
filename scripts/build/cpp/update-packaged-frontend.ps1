<#
    Rebuild and copy only the frontend into the unpacked local test package.

    Use this after frontend-only changes when a native rebuild is unnecessary:
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\update-packaged-frontend.ps1

    A complete package refresh remains:
      powershell -ExecutionPolicy Bypass -File scripts\build\cpp\package-cpp.ps1
#>
[CmdletBinding()]
param()

. "$PSScriptRoot\..\build-common.ps1"

Start-ScriptLog 'update-packaged-frontend'
Set-RepositoryTemp
$root = $Script:REPO_ROOT
$frontendDir = Join-Path $root 'frontend'
$frontendDist = Join-Path $frontendDir 'dist'
$packageApp = Join-Path $root 'tmp\packaged_build\app'

if (-not (Test-Path (Join-Path $root 'tmp\packaged_build\bin\streamfind-app.exe'))) {
    throw 'tmp\packaged_build is not a complete packaged build. Run package-cpp.ps1 first.'
}

Write-Log 'Building frontend application...'
Push-Location $frontendDir
try {
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw "Frontend build failed ($LASTEXITCODE)" }
} finally { Pop-Location }

if (-not (Test-Path (Join-Path $frontendDist 'index.html'))) {
    throw 'Frontend build did not produce dist/index.html'
}

Write-Log "Refreshing packaged frontend: $packageApp"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $packageApp
New-Item -ItemType Directory -Path $packageApp -Force | Out-Null
Copy-Item -Recurse -Force (Join-Path $frontendDist '*') $packageApp

Write-Log "Packaged frontend ready: $packageApp"
