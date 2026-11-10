function Get-StreamFindMSYS2Root {
    param([string]$ManagedHome)

    if ($env:STREAMFIND_MSYS2_ROOT) { return $env:STREAMFIND_MSYS2_ROOT }
    if (!$ManagedHome) {
        $ManagedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
    }
    $pathRoot = $null
    $pathCompiler = Get-Command gcc.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pathCompiler) {
        $compilerDirectory = Split-Path -Parent $pathCompiler.Source
        if ((Split-Path -Leaf (Split-Path -Parent $compilerDirectory)) -ieq 'ucrt64') {
            $pathRoot = Split-Path -Parent (Split-Path -Parent $compilerDirectory)
        }
    }
    $candidates = @(
        $pathRoot,
        'C:\msys64',
        (Join-Path $ManagedHome 'tools/msys2')
    )
    foreach ($candidate in $candidates) {
        if (!$candidate) { continue }
        if (Test-Path -LiteralPath (Join-Path $candidate 'usr/bin/bash.exe') -PathType Leaf) { return $candidate }
    }
    return $candidates[1]
}
