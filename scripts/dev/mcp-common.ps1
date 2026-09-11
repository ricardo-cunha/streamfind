Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Start-StreamfindMcp {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$Catalogue
    )

    if (-not (Test-Path $Executable -PathType Leaf)) {
        throw "MCP executable not found: $Executable"
    }
    if (-not (Test-Path $Catalogue -PathType Leaf)) {
        throw "Catalogue not found: $Catalogue"
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not start MCP executable: $Executable"
    }
    $process | Add-Member -MemberType NoteProperty -Name McpErrorTask -Value $process.StandardError.ReadLineAsync()
    return $process
}

function Drain-McpDiagnostics {
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)
    while ($Process.McpErrorTask.IsCompleted) {
        $line = $Process.McpErrorTask.Result
        if ($null -eq $line) {
            return
        }
        if (-not [string]::IsNullOrWhiteSpace($line)) {
            [Console]::WriteLine("[native] $line")
        }
        $Process.McpErrorTask = $Process.StandardError.ReadLineAsync()
    }
}

function Send-McpRequest {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][hashtable]$Request
    )

    $json = $Request | ConvertTo-Json -Compress -Depth 50
    $Process.StandardInput.WriteLine($json)
    $responseTask = $Process.StandardOutput.ReadLineAsync()
    while (-not $responseTask.IsCompleted) {
        Drain-McpDiagnostics $Process
        Start-Sleep -Milliseconds 25
    }
    Drain-McpDiagnostics $Process
    $line = $responseTask.Result
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "MCP process returned no response."
    }
    try {
        return $line | ConvertFrom-Json
    } catch {
        throw "MCP returned invalid JSON: $line"
    }
}

function Invoke-McpTool {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$Id,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][hashtable]$Arguments
    )

    $response = Send-McpRequest $Process @{
        jsonrpc = '2.0'
        id = $Id
        method = 'tools/call'
        params = @{ name = $Name; arguments = $Arguments }
    }
    if ($response.PSObject.Properties.Name -contains 'error' -and $null -ne $response.error) {
        throw "MCP tool '$Name' failed: $($response.error.message)"
    }
    if (($response.result.PSObject.Properties.Name -contains 'isError') -and $response.result.isError -eq $true) {
        $message = ($response.result.content | ForEach-Object { $_.text }) -join '; '
        throw "MCP tool '$Name' returned an error: $message"
    }
    $text = @($response.result.content | Where-Object { $_.type -eq 'text' } | Select-Object -First 1).text
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $response.result
    }
    try {
        return $text | ConvertFrom-Json
    } catch {
        return $text
    }
}

function Initialize-Mcp {
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)
    $response = Send-McpRequest $Process @{
        jsonrpc = '2.0'
        id = 1
        method = 'initialize'
        params = @{}
    }
    if ($response.PSObject.Properties.Name -contains 'error' -and $null -ne $response.error) {
        throw "MCP initialize failed: $($response.error.message)"
    }
    return $response
}

function Stop-StreamfindMcp {
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)
    if (-not $Process.HasExited) {
        $Process.StandardInput.Close()
        if (-not $Process.WaitForExit(5000)) {
            $Process.Kill()
            $Process.WaitForExit()
        }
    }
    $Process.Dispose()
}

function Get-StreamfindDataRoot {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
    if ($env:STREAMFIND_EXAMPLE_DATA_ROOT) {
        $override = (Resolve-Path $env:STREAMFIND_EXAMPLE_DATA_ROOT -ErrorAction Stop).Path
        if (-not (Test-Path $override -PathType Container)) {
            throw "STREAMFIND_EXAMPLE_DATA_ROOT is not a directory: $override"
        }
        return $override
    }
    $dataRoot = Join-Path (Split-Path $RepositoryRoot -Parent) 'streamfind.data\data'
    if (-not (Test-Path $dataRoot -PathType Container)) {
        throw "Sibling streamfind.data repository not found at $dataRoot"
    }
    return $dataRoot
}

function Get-StreamfindVendorRoot {
    if ($env:STREAMFIND_VENDOR_DATA_ROOT) {
        $root = (Resolve-Path $env:STREAMFIND_VENDOR_DATA_ROOT -ErrorAction Stop).Path
    } else {
        $root = 'E:\example_files\raw_vendor_files'
    }
    if (-not (Test-Path $root -PathType Container)) {
        throw "Vendor fixture root not found: $root"
    }
    return $root
}

function Invoke-StreamfindRustBuild {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
    $buildScript = Join-Path $RepositoryRoot 'scripts\build\rust\build-rust.ps1'
    $commandLine = 'set tmp=&&set temp=&&set TMP=C:\Windows\Temp&&set TEMP=C:\Windows\Temp&&set TMPDIR=C:\Windows\Temp&&powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $buildScript + '" -Clean -Release'
    & $env:ComSpec /d /c $commandLine | Out-Host
    return [int]$LASTEXITCODE
}

function Get-BackendMcpExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][ValidateSet('Cpp', 'Rust')][string]$Backend
    )
    if ($Backend -eq 'Cpp') {
        return (Join-Path $RepositoryRoot 'tmp\build\core-default\streamfind_mcp.exe')
    }
    return (Join-Path $RepositoryRoot 'tmp\build\rust-target\release\streamfind-rust-mcp.exe')
}
