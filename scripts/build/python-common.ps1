function Get-StreamFindBasePython {
    $names = if ($IsWindows) { @('python.exe', 'python3.exe', 'py.exe') } else { @('python3', 'python') }
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $names) {
        foreach ($command in (Get-Command $name -CommandType Application -All -ErrorAction SilentlyContinue)) {
            $path = $command.Source
            if (!$path -or !$seen.Add($path)) { continue }
            if ($IsLinux -and $path -match '^/mnt/[a-z]/') { continue }
            if ($IsWindows -and $path -match '\\WindowsApps\\') { continue }
            $prefix = if ([IO.Path]::GetFileNameWithoutExtension($path) -ieq 'py') { @('-3') } else { @() }
            try {
                $versionText = (& $path @prefix -c 'import sys; print(".".join(map(str, sys.version_info[:3])))' 2>$null | Select-Object -First 1)
                if ($LASTEXITCODE -ne 0 -or $versionText -notmatch '^(\d+)\.(\d+)\.(\d+)$') { continue }
                $version = [version]$versionText
                if ($version -lt [version]'3.11' -or $version -ge [version]'3.15') { continue }
                & $path @prefix -c 'import venv, ensurepip' *> $null
                if ($LASTEXITCODE -ne 0) { continue }
                return [pscustomobject]@{ Path = $path; PrefixArguments = $prefix; Version = $versionText }
            } catch { continue }
        }
    }
    return $null
}
