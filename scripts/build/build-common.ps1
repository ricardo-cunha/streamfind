<#
    build-common.ps1 — shared toolchain detection + logging for the streamfind
    build/test scripts (build-core.ps1, test-core.ps1, build-rust.ps1,
    test-rust.ps1).

    Design goals:
      - Machine independent: no hardcoded user or machine paths. Tools are
        resolved by the recommended standards:
          * Visual Studio   -> vswhere.exe (the official Microsoft installer
                               query tool), with $env:VSINSTALLDIR override.
          * cmake / ninja   -> $env:CMAKE / $env:NINJA override, else PATH via
                               Get-Command, else known VS-bundled locations.
          * cargo / rustc   -> $env:CARGO override, else PATH via Get-Command.
      - All transient outputs go under the repository-local tmp/ folder
        (AGENTS.md "Repository Scratch, Build, and Log Locations"): builds in
        tmp/build/, logs in tmp/logs/.

    Usage: dot-source this file from the other scripts:
        . "$PSScriptRoot\build-common.ps1"
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---- repository root --------------------------------------------------------

$Script:REPO_ROOT = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Script:TMP_DIR    = Join-Path $Script:REPO_ROOT 'tmp'
$Script:TMP_BUILD  = Join-Path $Script:TMP_DIR 'build'
$Script:TMP_LOGS   = Join-Path $Script:TMP_DIR 'logs'
$Script:TMP_SCRATCH = Join-Path $Script:TMP_DIR 'scratch'

function New-TmpDirs {
    foreach ($d in @($Script:TMP_BUILD, $Script:TMP_LOGS, $Script:TMP_SCRATCH)) {
        if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    }
}

function Set-RepositoryTemp {
    # MSVC and Cargo require valid Windows paths. Some agent shells export
    # colliding TMP/tmp or TEMP/temp values (often /tmp); use the managed
    # repository scratch directory for child tool processes instead.
    New-TmpDirs
    # Git Bash can export a second, lower-case `tmp` variable. Windows treats
    # environment names case-insensitively in most APIs, but cmd/rustc can
    # still receive both entries and interpret the MSYS value as C:\c\....
    [Environment]::SetEnvironmentVariable('tmp', $null, 'Process')
    [Environment]::SetEnvironmentVariable('temp', $null, 'Process')
    $nativeScratch = [System.IO.Path]::GetFullPath($Script:TMP_SCRATCH)
    [Environment]::SetEnvironmentVariable('TEMP', $nativeScratch, 'Process')
    [Environment]::SetEnvironmentVariable('TMP', $nativeScratch, 'Process')
    [Environment]::SetEnvironmentVariable('TMPDIR', $nativeScratch, 'Process')
}

# ---- logging ----------------------------------------------------------------

$script:LogFile = $null

function Start-ScriptLog {
    param([string]$Name)
    New-TmpDirs
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $script:LogFile = Join-Path $Script:TMP_LOGS "$Name-$stamp.log"
    "Script log: $script:LogFile"
}

function Write-Log {
    param([string]$Message)
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line
    if ($script:LogFile) { Add-Content -LiteralPath $script:LogFile -Value $line }
}

# ---- tool detection ---------------------------------------------------------

function Resolve-Command {
    # Returns the path of a tool found on PATH, or $null if absent.
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-CMake {
    if ($env:CMAKE) {
        if (Test-Path $env:CMAKE) { return $env:CMAKE }
        throw "CMAKE override set but not found: $env:CMAKE"
    }
    $path = Resolve-Command 'cmake'
    if (-not $path) { throw 'cmake not found on PATH; set $env:CMAKE or install CMake (https://cmake.org/download/)' }
    return $path
}

function Get-CTest {
    # ctest ships alongside cmake; prefer the sibling of the resolved cmake so
    # both come from the same installation.
    $cmake = Get-CMake
    $sibling = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (Test-Path $sibling) { return $sibling }
    $path = Resolve-Command 'ctest'
    if (-not $path) { throw 'ctest not found alongside cmake or on PATH' }
    return $path
}

function Get-CPack {
    if ($env:CPACK) {
        if (Test-Path $env:CPACK) { return $env:CPACK }
        throw "CPACK override set but not found: $env:CPACK"
    }
    $cmake = Get-CMake
    $sibling = Join-Path (Split-Path -Parent $cmake) 'cpack.exe'
    if (Test-Path $sibling) { return $sibling }
    $path = Resolve-Command 'cpack'
    if (-not $path) { throw 'cpack not found alongside cmake or on PATH; set $env:CPACK' }
    return $path
}

function Get-Cargo {
    if ($env:CARGO) {
        if (Test-Path $env:CARGO) { return $env:CARGO }
        throw "CARGO override set but not found: $env:CARGO"
    }
    $path = Resolve-Command 'cargo'
    if (-not $path) { throw 'cargo not found on PATH; set $env:CARGO or install the Rust toolchain (https://rustup.rs)' }
    return $path
}

function Invoke-SemanticChecks {
    $python = Join-Path $Script:REPO_ROOT '.venv\Scripts\python.exe'
    if (-not (Test-Path $python)) {
        throw "Repository Python environment not found at $python"
    }

    $validate = Join-Path $Script:REPO_ROOT 'semantic\validate_semantic.py'
    $check = Join-Path $Script:REPO_ROOT 'semantic\generate_projection.py'
    foreach ($script in @($validate, $check)) {
        if (-not (Test-Path $script)) {
            throw "Semantic script not found: $script"
        }
    }

    Push-Location $Script:REPO_ROOT
    try {
        Write-Log 'Validating semantic catalogue...'
        & $python $validate
        if ($LASTEXITCODE -ne 0) { throw "Semantic validation failed ($LASTEXITCODE)" }

        Write-Log 'Checking generated semantic projection...'
        & $python $check '--check'
        if ($LASTEXITCODE -ne 0) { throw "Semantic projection check failed ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Write-Log 'Semantic checks passed.'
}

function Get-VisualStudioPath {
    if ($env:VSINSTALLDIR) {
        if (Test-Path $env:VSINSTALLDIR) { return $env:VSINSTALLDIR }
        throw "VSINSTALLDIR override set but not found: $env:VSINSTALLDIR"
    }
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found (Visual Studio Installer missing); set $env:VSINSTALLDIR'
    }
    $path = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
    if (-not $path) {
        throw 'No Visual Studio with the VC++ tools component found; install it or set $env:VSINSTALLDIR'
    }
    return $path
}

function Get-VcvarsAll {
    $vs = Get-VisualStudioPath
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvarsall.bat not found under: $vcvars"
    }
    return $vcvars
}

function Get-Ninja {
    if ($env:NINJA) {
        if (Test-Path $env:NINJA) { return $env:NINJA }
        throw "NINJA override set but not found: $env:NINJA"
    }
    $path = Resolve-Command 'ninja'
    if ($path) { return $path }
    # Ninja is bundled with Visual Studio's CMake tooling.
    $vs = Get-VisualStudioPath
    $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    if (Test-Path $bundled) { return $bundled }
    throw 'ninja not found on PATH, via $env:NINJA, or in the Visual Studio CMake tools'
}

<#
    Invoke-VcvarsAll: runs vcvarsall.bat for the given architecture and imports
    the resulting environment into the current PowerShell session.
#>
function Invoke-VcvarsAll {
    param([string]$Arch = 'x64')
    $vcvars = Get-VcvarsAll
    Write-Log "Initializing VS environment ($Arch): $vcvars"
    $env:VSCMD_SKIP_SENDTELEMETRY = '1'
    $cmd = "`"$vcvars`" $Arch >nul 2>&1 && set"
    $output = & $env:ComSpec /d /s /c $cmd
    if ($LASTEXITCODE -ne 0 -or -not $output) {
        throw "Failed to initialize the Visual Studio environment via vcvarsall.bat ($LASTEXITCODE)"
    }
    foreach ($line in $output) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) {
            $name = $line.Substring(0, $eq)
            $value = $line.Substring($eq + 1)
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
    Set-RepositoryTemp
}

Set-RepositoryTemp
New-TmpDirs