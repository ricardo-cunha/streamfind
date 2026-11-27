# Release-distributed provisioning helper. It intentionally has no checkout-root dependency.
#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('setup','doctor')][string]$Command = 'doctor',
    [ValidateSet('native','viewer','python','all')][string]$Target = 'all',
    [string]$Workspace,
    [string]$ManagedHome,
    [string]$Sdk,
    [string]$Package,
    [string]$Viewer,
    [string]$Requirements,
    [switch]$Offline,
    [switch]$DryRun,
    [switch]$Json
)
$ErrorActionPreference='Stop'
$module=Join-Path $PSScriptRoot 'StreamFindProvisioning.psm1'
$manifestPath=Join-Path $PSScriptRoot 'toolchains.lock.json'
Import-Module $module -Force
$homePath=Resolve-StreamFindManagedHome $ManagedHome
$workspacePath=Resolve-StreamFindWorkspace $homePath $Workspace
$manifest=Read-StreamFindManifest $manifestPath

function New-DoctorReport {
    $node=Get-StreamFindNodeInfo $homePath $manifest
    $python=Get-StreamFindPythonInfo $homePath $manifest
    $workspacePython = Join-Path $workspacePath $(if ($IsWindows) { '.venv/Scripts/python.exe' } else { '.venv/bin/python' })
    $requirementsPath = if ($Requirements) { [IO.Path]::GetFullPath($Requirements) } else { $null }
    $msys=if ($IsWindows) { Get-StreamFindMsys2Info $homePath $manifest } else { $null }
    [ordered]@{
        schema_version=1
        workspace=$workspacePath
        managed_home=$homePath
        sdk=if ($Sdk) {[IO.Path]::GetFullPath($Sdk)} else {$null}
        package=if ($Package) {[IO.Path]::GetFullPath($Package)} else {$null}
        requirements=$requirementsPath
        tools=[ordered]@{
            node=[ordered]@{ version=$manifest.node.version; root=$node.root; node=$node.node; npm=$node.npm; installed=(Test-Path $node.node) }
            python=[ordered]@{ version=$manifest.python.version; root=$python.root; python=$python.python; installed=(Test-Path $python.python); workspace_venv=(Join-Path $workspacePath '.venv'); workspace_venv_installed=(Test-Path $workspacePython); workspace_python=$workspacePython; pip_installed=(Test-Path (Join-Path $workspacePath $(if ($IsWindows) { '.venv/Scripts/pip.exe' } else { '.venv/bin/pip' }))) }
            msys2=if ($msys) {[ordered]@{ version=$manifest.msys2.version; root=$msys.root; bash=$msys.bash; installed=(Test-Path $msys.bash); packages=@($msys.packages) }} else {$null}
            linux=if ($IsLinux) {[ordered]@{ distributions=@($manifest.linux.supported_distributions); packages=@($manifest.linux.packages); system_dependencies=$true }} else {$null}
        }
        manifest=$manifestPath
        offline=[bool]$Offline
    }
}

if ($Command -eq 'doctor') {
    $report=New-DoctorReport
    if ($Json) { $report | ConvertTo-Json -Depth 8 } else { $report | ConvertTo-Json -Depth 8 | Write-Output }
    exit 0
}

$lock=$null
try {
    if ($DryRun) {
        $preview=[ordered]@{ command='setup'; target=$Target; workspace=$workspacePath; managed_home=$homePath; offline=[bool]$Offline; actions=@() }
        if ($Target -in @('native','all')) { $preview.actions += if ($IsWindows) { "Install MSYS2 UCRT64: $($manifest.msys2.ucrt64_packages -join ', ')" } else { "Install system packages through sudo apt-get: $($manifest.linux.packages -join ', ')" } }
        if ($Target -in @('viewer','all')) { $preview.actions += "Install Node.js $($manifest.node.version) and npm from verified archive"; if ($Viewer) { $preview.actions += "Run npm ci --engine-strict in $Viewer" } }
        if ($Target -in @('python','all')) { $preview.actions += "Install pinned Python $($manifest.python.version) and create $workspacePath/.venv"; if ($Requirements) { $preview.actions += "Install workspace Python packages from $Requirements (explicit request; environment remains extensible)" } }
        $preview | ConvertTo-Json -Depth 6
        exit 0
    }
    $lock=Enter-StreamFindSetupLock $homePath
    if ($Target -in @('native','all')) { if ($IsWindows) { Install-StreamFindMsys2 $homePath $manifest $Offline | Out-Null } else { Install-StreamFindLinuxNative $manifest $Offline | Out-Null } }
    if ($Target -in @('viewer','all')) {
        $node=Install-StreamFindNode $homePath $manifest $Offline
        if ($Viewer) {
            $viewerPath=[IO.Path]::GetFullPath($Viewer); $lockfile=Join-Path $viewerPath $manifest.viewer.lockfile
            if (!(Test-Path $lockfile)) { throw "Viewer dependency setup requires $lockfile; refusing a non-lockfile install." }
            $old=$env:PATH; try { $env:PATH="$(Split-Path -Parent $node.node)$([IO.Path]::PathSeparator)$old"; Push-Location $viewerPath; & $node.npm @($manifest.viewer.install_command); if ($LASTEXITCODE) { throw "npm ci failed ($LASTEXITCODE)" } } finally { Pop-Location; $env:PATH=$old }
        }
    }
    if ($Target -in @('python','all')) { Install-StreamFindPython $homePath $manifest $workspacePath $Offline $Requirements | Out-Null }
    Write-Output "StreamFind development setup completed. Workspace: $workspacePath; managed home: $homePath"
} finally { if ($lock) { $lock.Dispose(); $lock=$null } }
