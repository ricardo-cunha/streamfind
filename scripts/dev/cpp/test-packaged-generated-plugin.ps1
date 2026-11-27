[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$PackageRoot,
    [Parameter(Mandatory = $true)] [string]$PluginRoot,
    [int]$Port = 8891
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$scratchRoot = Join-Path $repoRoot 'tmp\scratch'
$package = Join-Path $scratchRoot 'phase2-packaged'
$plugin = Join-Path $package 'plugins\custom\custom.plugin.phase2_example'
$bin = Join-Path $package 'bin'
$server = Join-Path $bin 'streamfind-server.exe'
$stdout = Join-Path $package 'phase2-service.stdout.log'
$stderr = Join-Path $package 'phase2-service.stderr.log'

if (!(Test-Path -LiteralPath $PackageRoot -PathType Container)) { throw "package root is missing: $PackageRoot" }
if (!(Test-Path -LiteralPath (Join-Path $PluginRoot 'plugin.json') -PathType Leaf)) { throw "plugin manifest is missing: $PluginRoot" }
if (!(Test-Path -LiteralPath (Join-Path $PluginRoot 'catalogue.duckdb') -PathType Leaf)) { throw "plugin catalogue is missing: $PluginRoot" }

if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
New-Item -ItemType Directory -Force -Path $scratchRoot | Out-Null
Copy-Item -LiteralPath $PackageRoot -Destination $package -Recurse
New-Item -ItemType Directory -Force -Path $plugin | Out-Null
Copy-Item -LiteralPath (Join-Path $PluginRoot 'plugin.json') -Destination $plugin
Copy-Item -LiteralPath (Join-Path $PluginRoot 'catalogue.duckdb') -Destination $plugin
$library = Get-ChildItem -LiteralPath $PluginRoot -Filter 'streamfind_phase2_example.dll' -File | Select-Object -First 1
if (!$library) { throw "generated plugin library is missing: $PluginRoot" }
Copy-Item -LiteralPath $library.FullName -Destination $plugin

$configuration = [ordered]@{
    server = [ordered]@{ port = $Port; automatic_port_fallback = $false }
    worker = [ordered]@{ memory_limit_mb = 0 }
    plugin_roots = @{ development = @(); deployment = @('../plugins') }
    viewer_roots = @{ development = @(); deployment = @('../viewers/native') }
    enabled_viewers = @()
    enabled_plugins = @('custom.plugin.phase2_example')
}
$configuration | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $bin 'streamfind.json') -Encoding utf8

$process = $null
try {
    $process = Start-Process -FilePath $server -ArgumentList ([string]$Port) -WorkingDirectory $bin `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
    $healthy = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        Start-Sleep -Milliseconds 500
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2
            if ($health.health -eq 'healthy') { $healthy = $true; break }
        } catch {
            if ($process.HasExited) { break }
        }
    }
    if (!$healthy) {
        $details = @((Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue),
            (Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue)) -join "`n"
        throw "packaged service did not become healthy: $details"
    }
    $operations = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/capabilities/operations?plugin=phase2_example" -TimeoutSec 5
    if (($operations | ConvertTo-Json -Depth 20) -notmatch 'phase2_example[.]ping') {
        throw 'generated plugin operation was not published by the packaged service'
    }
    $session = 'phase2-worker'
    $projectPath = Join-Path $repoRoot "tmp\projects\phase2-worker-$Port.sf"
    $projectRequest = @{ session_id = $session; project_path = $projectPath; mode = 'create' } |
        ConvertTo-Json -Depth 5
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/projects" -ContentType 'application/json' -Body $projectRequest | Out-Null
    $runRequest = @{ operation_instance = 'phase2-worker-run'; parameters = @{}; inputs = $null } |
        ConvertTo-Json -Depth 5
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$Port/projects/$session/operations/phase2_example.ping/run" `
        -ContentType 'application/json' -Body $runRequest | Out-Null
    $state = $null
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 500
        $state = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/projects/$session/operations/phase2-worker-run/state" -TimeoutSec 2
        if ($state.state -in @('completed', 'failed', 'cancelled')) { break }
    }
    if (!$state -or $state.state -ne 'completed') { throw "packaged worker execution failed: $($state | ConvertTo-Json -Depth 20)" }
    $results = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/projects/$session/results/current" -TimeoutSec 5
    if (($results | ConvertTo-Json -Depth 20) -notmatch 'examplePingResult') {
        throw 'packaged worker did not publish the generated plugin result'
    }
    Write-Host 'packaged generated plugin loaded, executed through the worker, and published its result'
}
finally {
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force }
}
