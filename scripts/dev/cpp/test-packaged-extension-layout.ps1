[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$PackageRoot
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path $PackageRoot).Path
$requiredDirectories = @(
    'plugins/native',
    'plugins/custom',
    'viewers/native',
    'viewers/custom'
)
foreach ($relative in $requiredDirectories) {
    $path = Join-Path $package $relative
    if (!(Test-Path -LiteralPath $path -PathType Container)) {
        throw "Packaged extension directory is missing: $relative"
    }
}

$nativePlugins = @(Get-ChildItem (Join-Path $package 'plugins/native') -Directory)
if ($nativePlugins.Count -lt 1) { throw 'No native plugin packages were staged' }
foreach ($plugin in $nativePlugins) {
    $manifest = Join-Path $plugin.FullName 'plugin.json'
    if (!(Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "Native plugin package has no plugin.json: $($plugin.Name)"
    }
}

$nativeViewers = @(Get-ChildItem (Join-Path $package 'viewers/native') -Directory |
    Where-Object { $_.Name -ne '_shared' })
if ($nativeViewers.Count -lt 1) { throw 'No native viewer packages were staged' }
if (Test-Path -LiteralPath (Join-Path $package 'viewers/native/native') -PathType Container) {
    throw 'Native viewers have an unexpected nested viewers/native/native directory'
}
foreach ($viewer in $nativeViewers) {
    $manifest = Join-Path $viewer.FullName 'plugin.manifest.json'
    if (!(Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "Native viewer package has no plugin.manifest.json: $($viewer.Name)"
    }
}

Write-Output ("Packaged extension layout passed: native_plugins={0}; native_viewers={1}" -f $nativePlugins.Count, $nativeViewers.Count)
