function Get-StreamFindManagedPython {
    param([string]$ManagedHome = '')

    if (!$ManagedHome) {
        $ManagedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
    }
    if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or (!$IsWindows -and !$IsLinux)) {
        return $null
    }

    $pin = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'python-version.txt') -Raw).Trim()
    $platform = if ($IsWindows) { 'win-x64' } else { 'linux-x64' }
    $destination = Join-Path $ManagedHome "tools/python/$pin/$platform"
    $path = if ($IsWindows) { Join-Path $destination 'python.exe' } else { Join-Path $destination 'bin/python3.11' }
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $null }

    return [pscustomobject]@{ Path = $path; Destination = $destination; Version = $pin.Split('+')[0] }
}
