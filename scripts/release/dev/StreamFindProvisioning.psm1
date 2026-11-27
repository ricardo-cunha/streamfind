Set-StrictMode -Version Latest

function Resolve-StreamFindManagedHome {
    param([string]$Requested)
    if ($Requested) { return [IO.Path]::GetFullPath($Requested) }
    if ($env:STREAMFIND_HOME) { return [IO.Path]::GetFullPath($env:STREAMFIND_HOME) }
    return [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind'))
}

function Resolve-StreamFindWorkspace {
    param([string]$ManagedHome, [string]$Requested)
    $config = Join-Path $ManagedHome 'config/development.json'
    if ($Requested) {
        $workspace = [IO.Path]::GetFullPath($Requested)
        [IO.Directory]::CreateDirectory((Split-Path -Parent $config)) | Out-Null
        @{ workspace = $workspace } | ConvertTo-Json | Set-Content -LiteralPath $config -Encoding UTF8
        return $workspace
    }
    if (Test-Path -LiteralPath $config -PathType Leaf) {
        try {
            $saved = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
            if ($saved.workspace) { return [IO.Path]::GetFullPath([string]$saved.workspace) }
        } catch { }
    }
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (!$documents) { $documents = [Environment]::GetFolderPath('UserProfile') }
    return [IO.Path]::GetFullPath((Join-Path $documents 'streamfind-dev'))
}

function Read-StreamFindManifest {
    param([string]$ManifestPath)
    if (!(Test-Path -LiteralPath $ManifestPath -PathType Leaf) ) { throw "Provisioning manifest is missing: $ManifestPath" }
    return Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
}

function Enter-StreamFindSetupLock {
    param([string]$ManagedHome, [int]$TimeoutSeconds = 120)
    $lockPath = Join-Path $ManagedHome 'dev/setup.lock'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $lockPath)) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        try {
            $stream = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $stream.SetLength(0)
            $bytes = [Text.Encoding]::UTF8.GetBytes("pid=$PID`nutc=$([DateTime]::UtcNow.ToString('o'))`n")
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush()
            return $stream
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "Another StreamFind setup is active ($lockPath). Wait for it to finish, then retry. The lock is released automatically if that process exits."
            }
            Start-Sleep -Milliseconds 250
        }
    }
}

function Get-StreamFindCachedArtifact {
    param($Artifact, [string]$ManagedHome, [switch]$Offline, [int]$Retries = 3)
    $uri = [Uri]$Artifact.url
    if ($uri.Scheme -ne 'https' -or $uri.Host -notin @('nodejs.org', 'repo.msys2.org', 'github.com')) {
        throw "Untrusted provisioning source: $($Artifact.url)"
    }
    $cache = Join-Path $ManagedHome "downloads/$($Artifact.file)"
    [IO.Directory]::CreateDirectory((Split-Path -Parent $cache)) | Out-Null
    $expected = ([string]$Artifact.sha256).ToLowerInvariant()
    if (Test-Path -LiteralPath $cache -PathType Leaf) {
        $actual = (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $expected) { return $cache }
        if ($Offline) { throw "Offline cache hash mismatch for $($Artifact.file). Replace $cache with the verified artifact ($expected)." }
        Remove-Item -LiteralPath $cache -Force
    }
    if ($Offline) { throw "Offline setup needs a verified cached artifact at $cache (SHA-256 $expected). Pre-download it before retrying." }
    $staging = Join-Path $ManagedHome "dev/.staging/$([guid]::NewGuid().ToString('N'))"
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    $partial = Join-Path $staging $Artifact.file
    try {
        for ($attempt = 1; $attempt -le $Retries; $attempt++) {
            try {
                Invoke-WebRequest -Uri ([string]$Artifact.url) -OutFile $partial
                $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($actual -ne $expected) { throw "SHA-256 mismatch: expected $expected, got $actual" }
                Move-Item -LiteralPath $partial -Destination $cache -Force
                return $cache
            } catch {
                if ($attempt -eq $Retries) { throw "Download failed for $($Artifact.file) after $Retries attempts. URL: $($Artifact.url). $($_.Exception.Message)" }
                Start-Sleep -Seconds ([math]::Min(8, [math]::Pow(2, $attempt - 1)))
            }
        }
    } finally { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
}

function Get-StreamFindNodeInfo {
    param([string]$ManagedHome, $Manifest)
    $platform = if ($IsWindows) { 'windows-x64' } elseif ($IsLinux) { 'linux-x64' } else { throw 'Managed Node.js supports Windows x64 and Linux x64 only.' }
    $archivePlatform = if ($IsWindows) { 'win-x64' } else { 'linux-x64' }
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') { throw 'Managed Node.js supports x64 only.' }
    $archive = $Manifest.node.archives.$platform
    $root = Join-Path $ManagedHome "tools/node/v$($Manifest.node.version)/$platform"
    $node = Join-Path $root $(if ($IsWindows) { 'node.exe' } else { 'bin/node' })
    $npm = Join-Path $root $(if ($IsWindows) { 'npm.cmd' } else { 'bin/npm' })
    [pscustomobject]@{ platform=$platform; archive_platform=$archivePlatform; archive=$archive; root=$root; node=$node; npm=$npm }
}

function Install-StreamFindNode {
    param([string]$ManagedHome, $Manifest, [switch]$Offline, [switch]$DryRun)
    $info = Get-StreamFindNodeInfo $ManagedHome $Manifest
    if ((Test-Path $info.node) -and (Test-Path $info.npm)) {
        return $info
    }
    if ($DryRun) { return $info }
    $archive = Get-StreamFindCachedArtifact -Artifact $info.archive -ManagedHome $ManagedHome -Offline:$Offline
    $staging = Join-Path $ManagedHome "dev/.staging/node-$([guid]::NewGuid().ToString('N'))"
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    try {
        if ($IsWindows) { Expand-Archive -LiteralPath $archive -DestinationPath $staging }
        else { & tar -xJf $archive -C $staging; if ($LASTEXITCODE) { throw "Node.js extraction failed ($LASTEXITCODE)" } }
        $extracted = Join-Path $staging "node-v$($Manifest.node.version)-$($info.archive_platform)"
        if (!(Test-Path (Join-Path $extracted $(if ($IsWindows) {'node.exe'} else {'bin/node'})))) { throw 'Verified Node.js archive has an unexpected layout.' }
        [IO.Directory]::CreateDirectory((Split-Path -Parent $info.root)) | Out-Null
        if (Test-Path $info.root) { Remove-Item $info.root -Recurse -Force }
        Move-Item $extracted $info.root
        return $info
    } finally { Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue }
}

function Get-StreamFindPythonInfo {
    param([string]$ManagedHome, $Manifest)
    $platform = if ($IsWindows) { 'windows-x64' } elseif ($IsLinux) { 'linux-x64' } else { throw 'Managed Python supports Windows x64 and Linux x64 only.' }
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') { throw 'Managed Python supports x64 only.' }
    $archive = $Manifest.python.archives.$platform
    $root = Join-Path $ManagedHome "tools/python/v$($Manifest.python.version)/$platform"
    $python = Join-Path $root $(if ($IsWindows) { 'python.exe' } else { 'bin/python3.11' })
    [pscustomobject]@{ platform=$platform; archive=$archive; root=$root; python=$python; version=$Manifest.python.version }
}

function Install-StreamFindPython {
    param([string]$ManagedHome, $Manifest, [string]$Workspace, [switch]$Offline, [string]$Requirements, [switch]$DryRun)
    $info = Get-StreamFindPythonInfo $ManagedHome $Manifest
    if (!(Test-Path $info.python)) {
        if ($DryRun) { return [pscustomobject]@{ runtime=$info; venv=(Join-Path $Workspace '.venv') } }
        $archive = Get-StreamFindCachedArtifact -Artifact $info.archive -ManagedHome $ManagedHome -Offline:$Offline
        $staging = Join-Path $ManagedHome "dev/.staging/python-$([guid]::NewGuid().ToString('N'))"
        [IO.Directory]::CreateDirectory($staging) | Out-Null
        try {
            & tar -xzf $archive -C $staging
            if ($LASTEXITCODE) { throw "Python extraction failed ($LASTEXITCODE)" }
            $extractedPython = Get-ChildItem -LiteralPath $staging -Recurse -File -Filter $(if ($IsWindows) {'python.exe'} else {'python3.11'}) | Select-Object -First 1
            if (!$extractedPython) { throw 'Verified Python archive did not contain the expected interpreter.' }
            $extractedRoot = if ($IsWindows) { Split-Path -Parent $extractedPython.FullName } else { Split-Path -Parent (Split-Path -Parent $extractedPython.FullName) }
            [IO.Directory]::CreateDirectory((Split-Path -Parent $info.root)) | Out-Null
            if (Test-Path $info.root) { Remove-Item $info.root -Recurse -Force }
            Move-Item $extractedRoot $info.root
        } finally { Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue }
    }
    $venv = Join-Path ([IO.Path]::GetFullPath($Workspace)) '.venv'
    $venvPython = Join-Path $venv $(if ($IsWindows) {'Scripts/python.exe'} else {'bin/python'})
    if (!(Test-Path $venvPython)) {
        [IO.Directory]::CreateDirectory((Split-Path -Parent $venv)) | Out-Null
        & $info.python -m venv $venv
        if ($LASTEXITCODE) { throw "Workspace Python virtual environment creation failed ($LASTEXITCODE)" }
    }
    & $venvPython -c 'import sys; assert sys.version_info >= (3, 11); print(sys.executable)'
    if ($LASTEXITCODE) { throw "Workspace Python virtual environment validation failed: $venv" }
    if ($Requirements) {
        $requirementsPath = [IO.Path]::GetFullPath($Requirements)
        if (!(Test-Path -LiteralPath $requirementsPath -PathType Leaf)) { throw "Python requirements file was not found: $requirementsPath" }
        & $venvPython -m pip install --requirement $requirementsPath
        if ($LASTEXITCODE) { throw "Workspace Python requirements installation failed ($LASTEXITCODE)" }
    }
    return [pscustomobject]@{ runtime=$info; venv=$venv; python=$venvPython }
}

function Get-StreamFindMsys2Info {
    param([string]$ManagedHome, $Manifest)
    $root = if ($env:STREAMFIND_MSYS2_ROOT) { [IO.Path]::GetFullPath($env:STREAMFIND_MSYS2_ROOT) } else { Join-Path $ManagedHome 'tools/msys2' }
    [pscustomobject]@{ root=$root; bash=(Join-Path $root 'usr/bin/bash.exe'); installer=$Manifest.msys2.installer; packages=@($Manifest.msys2.ucrt64_packages) }
}

function Install-StreamFindMsys2 {
    param([string]$ManagedHome, $Manifest, [switch]$Offline, [switch]$DryRun)
    if (!$IsWindows) { throw 'MSYS2 provisioning is Windows-only; Linux uses the documented APT packages.' }
    $info = Get-StreamFindMsys2Info $ManagedHome $Manifest
    if ($DryRun) { return $info }
    if (!(Test-Path $info.bash)) {
        $installer = Get-StreamFindCachedArtifact -Artifact $info.installer -ManagedHome $ManagedHome -Offline:$Offline
        $signature = Get-AuthenticodeSignature -FilePath $installer
        if ($signature.Status -ne 'Valid') { throw "MSYS2 installer Authenticode validation failed: $($signature.Status)" }
        [IO.Directory]::CreateDirectory($info.root) | Out-Null
        & $installer in --confirm-command --accept-messages --root ($info.root.Replace('\','/'))
        if ($LASTEXITCODE) { throw "MSYS2 installer failed ($LASTEXITCODE)" }
    }
    $pacman = Join-Path $info.root 'usr/bin/pacman.exe'
    if (!(Test-Path $pacman)) { throw "MSYS2 pacman is missing: $pacman" }
    $oldPath=$env:PATH; $oldSystem=$env:MSYSTEM
    try { $env:MSYSTEM='UCRT64'; $env:PATH="$(Join-Path $info.root 'ucrt64/bin');$(Join-Path $info.root 'usr/bin');$oldPath"; & $pacman -S --needed --noconfirm @($info.packages); if ($LASTEXITCODE) { throw "MSYS2 package installation failed ($LASTEXITCODE)" } }
    finally { $env:PATH=$oldPath; $env:MSYSTEM=$oldSystem }
    [Environment]::SetEnvironmentVariable('STREAMFIND_MSYS2_ROOT', $info.root, 'User')
    return $info
}

function Install-StreamFindLinuxNative {
    param($Manifest, [switch]$Offline, [switch]$DryRun)
    if (!$IsLinux) { throw 'Linux native provisioning was requested on a non-Linux host.' }
    $id = ''; if (Test-Path '/etc/os-release') { $line=(Select-String -Path '/etc/os-release' -Pattern '^ID=' | Select-Object -First 1); if ($line) { $id=$line.Line.Substring(3).Trim('"') } }
    if ($id -notin @('ubuntu','debian')) { throw "Unsupported Linux distribution '$id'. Supported distributions: Ubuntu and Debian." }
    $packages=@($Manifest.linux.packages); if ($DryRun) { return [pscustomobject]@{ distribution=$id; command="sudo apt-get install --no-upgrade $($packages -join ' ')" } }
    if ($Offline) { throw 'Offline Linux setup cannot install missing system packages. Install the documented APT packages while online, then rerun with --offline.' }
    $sudo=Get-Command sudo -ErrorAction SilentlyContinue; if (!$sudo) { throw 'sudo is required for Linux native setup because dependencies are system packages.' }
    & $sudo.Source apt-get install --no-upgrade @packages; if ($LASTEXITCODE) { throw "APT package installation failed ($LASTEXITCODE)" }
    return [pscustomobject]@{ distribution=$id; packages=$packages }
}

Export-ModuleMember -Function Resolve-StreamFindManagedHome,Resolve-StreamFindWorkspace,Read-StreamFindManifest,Enter-StreamFindSetupLock,Get-StreamFindCachedArtifact,Get-StreamFindNodeInfo,Install-StreamFindNode,Get-StreamFindPythonInfo,Install-StreamFindPython,Get-StreamFindMsys2Info,Install-StreamFindMsys2,Install-StreamFindLinuxNative
