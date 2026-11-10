function Get-StreamfindManagedHome {
    if ($env:STREAMFIND_HOME) {
        return [IO.Path]::GetFullPath($env:STREAMFIND_HOME)
    }
    return Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind'
}

function Resolve-StreamfindExampleData {
    param(
        [switch]$Ensure,
        [string]$ManagedHome = (Get-StreamfindManagedHome)
    )

    $dataRoot = $null
    if ($env:STREAMFIND_EXAMPLE_DATA_ROOT) {
        $dataRoot = [IO.Path]::GetFullPath($env:STREAMFIND_EXAMPLE_DATA_ROOT)
        if (!(Test-Path -LiteralPath $dataRoot -PathType Container)) {
            throw "STREAMFIND_EXAMPLE_DATA_ROOT is not an existing data directory: $dataRoot"
        }
        return [pscustomobject]@{ RepositoryRoot = (Split-Path -Parent $dataRoot); DataRoot = $dataRoot; Source = 'override' }
    }

    $repositoryRoot = Join-Path ([IO.Path]::GetFullPath($ManagedHome)) 'streamfind.data'
    $dataRoot = Join-Path $repositoryRoot 'data'
    if (Test-Path -LiteralPath $dataRoot -PathType Container) {
        return [pscustomobject]@{ RepositoryRoot = $repositoryRoot; DataRoot = $dataRoot; Source = 'managed' }
    }

    if (!$Ensure) {
        return [pscustomobject]@{ RepositoryRoot = $repositoryRoot; DataRoot = $dataRoot; Source = 'missing' }
    }

    if (Test-Path -LiteralPath $repositoryRoot) {
        throw "The managed data path exists but has no data directory: $repositoryRoot. Move it aside or set STREAMFIND_EXAMPLE_DATA_ROOT to a valid checkout."
    }
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if (!$git) { throw 'Git is required to download the streamfind.data repository.' }

    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($repositoryRoot))
    $repositoryUrl = 'https://git.uni-due.de/odea-project/streamfind/streamfind.data'
    Write-Host "Downloading streamfind.data to $repositoryRoot"
    & $git.Source clone --depth 1 $repositoryUrl $repositoryRoot
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $repositoryRoot) { Remove-Item -LiteralPath $repositoryRoot -Recurse -Force }
        throw "Could not clone streamfind.data from $repositoryUrl (git exit $LASTEXITCODE)."
    }
    if (!(Test-Path -LiteralPath $dataRoot -PathType Container)) {
        throw "Downloaded streamfind.data repository is missing its data directory: $dataRoot"
    }
    return [pscustomobject]@{ RepositoryRoot = $repositoryRoot; DataRoot = $dataRoot; Source = 'managed' }
}
