$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\example-data-common.ps1"
$data = Resolve-StreamfindExampleData -Ensure
Write-Host "streamfind.data checkout: $($data.RepositoryRoot)"
Write-Host "Example data directory: $($data.DataRoot)"
