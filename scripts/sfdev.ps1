#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$frontend = Join-Path $root 'frontend'
$platform = if ($IsWindows) { 'windows-core' } else { 'linux-core' }
$managedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
. (Join-Path $PSScriptRoot 'build/msys2-common.ps1')
. (Join-Path $PSScriptRoot 'build/python-common.ps1')
$ucrt = if ($IsWindows) { Get-StreamFindMSYS2Root $managedHome }
$msys2ManagedRoot = Join-Path $managedHome 'tools/msys2'
$bin = if ($IsWindows) { Join-Path $ucrt 'ucrt64/bin' }

function Resolve-Tool([string]$Name) {
    $commands = Get-Command $Name -CommandType Application -All -ErrorAction SilentlyContinue
    foreach ($command in $commands) {
        if ($IsLinux -and $command.Source -match '^/mnt/[a-z]/') { continue }
        return $command.Source
    }
    return $null
}

function New-Step([string]$Directory, [string]$Executable, [string[]]$Arguments) {
    if (!$Executable) { throw "Required executable is missing for: $($Arguments -join ' ')" }
    [ordered]@{ directory = $Directory; executable = $Executable; arguments = $Arguments }
}

function Get-DevelopmentScripts([string]$Directory, [string]$Prefix = '') {
    foreach ($item in Get-ChildItem -LiteralPath $Directory) {
        if ($item.Name.StartsWith('_') -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }
        $id = "$Prefix$($item.Name)"
        if ($item.PSIsContainer) {
            Get-DevelopmentScripts $item.FullName "$id/"
        } elseif ($item.Extension -in '.ps1', '.py', '.sh', '.cmd', '.bat') {
            [pscustomobject]@{ id = $id; path = $item.FullName; extension = $item.Extension }
        }
    }
}

function Show-Help([string]$Topic) {
    $help = [ordered]@{
        'env show' = @('env show [--json]', 'Show the checkout, default build directory, and resolved tool paths.', 'Use --json for machine-readable output.')
        'env shell' = @('env shell [--dry-run]', 'Open PowerShell 7 with a checkout-local sfdev command.', 'Starts in the repository root. Type exit to return to the parent shell.')
        'doctor' = @('doctor [--workflow native|frontend|scripts] [--json]', 'Check tool availability and version commands for a workflow.', 'Without --workflow, checks all listed tools. Returns a nonzero exit code on failure.')
        'setup' = @('setup --components LIST [--dry-run]', 'Install native, java, frontend, browser, or python prerequisites.', 'Python setup reuses a compatible system interpreter or provisions the pinned Python under .streamfind/tools, then creates the repository .venv.')
        'configure' = @('configure [--config Debug|Release] [--dry-run]', 'Generate a native CMake build tree under the platform build directory.', "Defaults to $platform, Debug, and all plugins. CMake discovers all bundled plugins.", 'Example: sfdev configure --config Release')
        'build backend' = @('build backend [--target NAME] [--jobs N] [--dry-run]', 'Compile an existing native build with CMake.', "Configure first. Defaults to $platform and the default CMake target.", 'Example: sfdev build backend --target streamfind_statistics_unit')
        'build frontend' = @('build frontend [--dry-run]', 'Run npm run build in frontend/ and write assets to frontend/dist/.', 'Run sfdev setup --components frontend when Node.js/npm are missing or frontend dependencies are not installed.')
        'build all' = @('build all [--jobs N] [--dry-run]', 'Build the configured native build, then build frontend assets.', "Defaults to $platform. Requires both native configuration and frontend dependencies.")
        'test backend' = @('test backend [--list] [--filter REGEX] [--label REGEX] [--jobs N] [--dry-run]', 'List or run CTest tests in a native build.', "Defaults to $platform. Build the selected tests first. Use --list to inspect tests without running them.", "Example: sfdev test backend --filter '^streamfind_statistics_unit$'")
        'test frontend' = @('test frontend [--watch] [--dry-run] [-- ARGUMENTS]', 'Run Vitest once, or keep it running with --watch.', 'Requires frontend dependencies. Run sfdev setup --components frontend first when needed.')
        'test browser' = @('test browser --url URL [--dry-run]', 'Run the Playwright smoke check against a running frontend.', 'Install frontend dependencies and Playwright Chromium first.')
        'check frontend' = @('check frontend [--dry-run]', 'Run the frontend build, lint, and formatting check in order.', 'Requires installed frontend dependencies. Stops at the first failed step.')
        'run backend' = @('run backend [--url URL] [--dry-run]', 'Start a development service or use one that is already ready.', "Defaults to $platform and http://127.0.0.1:8790. Build the service and worker first.")
        'run frontend' = @('run frontend [--url URL] [--dry-run]', 'Start Vite and connect it to an existing ready service.', 'Requires frontend dependencies. Run sfdev setup --components frontend first when needed. Defaults to http://127.0.0.1:8790.')
        'run app' = @('run app [--url URL] [--dry-run]', 'Start Vite and start or reuse a development service.', 'Press Ctrl+C to stop processes started by this command.')
        'package' = @('package [--frontend-only] [--dry-run]', 'Build the unpacked native-Windows test distribution under tmp/packaged_build/.', 'Run on the Windows development machine, never through WSL. The full workflow builds frontend and native targets, runs CTest, and stages the package. Use --frontend-only to refresh assets in an existing complete package.')
        'clean' = @('clean --scope build|all-temp [--dry-run]', 'Remove the platform build tree or the contents of repository tmp/.', 'Use --dry-run to preview deletions. all-temp includes logs, test projects, packages, and release archives.')
        'script list' = @('script list [--json]', 'List public scripts under scripts/dev/ and show interpreter availability.', 'An available interpreter does not guarantee that the script has its required data or arguments.')
        'script info' = @('script info PATH', 'Show the interpreter, working directory, and invocation for a script.', 'Use a path from script list. Example: sfdev script info validate_native_dependencies.py')
        'script run' = @('script run PATH [--dry-run] [-- ARGUMENTS]', 'Run a development script from the checkout root and log to tmp/logs/.', 'Pass the script arguments after --. Python scripts use the repository .venv.', 'Example: sfdev script run validate_native_dependencies.py -- --help')
        'version show' = @('version show', 'Print the canonical streamfind version from VERSION.')
        'version bump' = @('version bump patch|minor|major [--dry-run]', 'Advance the canonical VERSION.', 'Resets lower components and prints the previous and next version.')
        'sdk build' = @('sdk build [--dry-run]', 'Build the Windows kit and, when Ubuntu/WSL is available, the Linux kit.', 'Uses the canonical VERSION and runs each platform package validation.')
        'plugin list' = @('plugin list', 'List the bundled native plugin directories.')
        'plugin new' = @('plugin new PATH [--kind backend|frontend|full] [--domain ID] [--dry-run]', 'Create an external plugin project.')
        'plugin build' = @('plugin build PATH --sdk PATH [--dry-run]', 'Configure and compile an external native plugin against an installed developer kit.')
        'plugin test' = @('plugin test PATH [--dry-run]', 'Run the external native plugin CTest suite.')
        'plugin stage' = @('plugin stage PATH [--dry-run]', 'Install a built external native plugin under tmp/build/.')
        'plugin validate' = @('plugin validate PATH --sdk PATH [--dry-run]', 'Validate the staged native plugin with the developer kit validator.')
        'release prepare' = @('release prepare [--dry-run]', 'Build and validate the runtime release archive for this host.', 'Requires a clean committed checkout. Run on Windows and Linux to produce both archives.')
        'release verify' = @('release verify [--dry-run]', 'Verify checksums and smoke-test the extracted runtime for this host.', 'Uses the canonical VERSION and the repository Python environment.')
        'release publish' = @('release publish [--dry-run]', 'Publish checksum-verified runtime archives to GitHub.', 'Requires authenticated gh and an existing remote version tag.')
    }
    if (!$Topic) {
        'sfdev developer CLI'
        'Run sfdev help <command> for details. Example: sfdev help script run'
        ''
        foreach ($entry in $help.GetEnumerator()) {
            "  $($entry.Key.PadRight(17)) $($entry.Value[1])"
        }
        return
    }
    $matches = @($help.Keys | Where-Object { $_ -eq $Topic -or $_.StartsWith("$Topic ", [StringComparison]::OrdinalIgnoreCase) })
    if (!$matches) { throw "Unknown help topic: $Topic. Run sfdev help." }
    foreach ($key in $matches) {
        $usage, $summary, $details = $help[$key]
        "Usage: sfdev $usage"
        $summary
        $details
        ''
    }
}

try {
    if ($args.Count -eq 0 -or $args[0] -in 'help', '--help') {
        Show-Help (($args | Select-Object -Skip 1) -join ' ')
        exit 0
    }
    $command = $args[0]
    $subject = ''
    $index = 1
    if ($command -in 'env', 'build', 'test', 'check', 'version', 'script', 'run', 'sdk', 'release', 'plugin') {
        if ($args.Count -lt 2) { throw "Missing subcommand for $command" }
        $subject = $args[1]
        $index = 2
    }
    $scriptId = ''
    $pluginPath = ''
    if ($command -eq 'plugin' -and $subject -in 'new', 'build', 'test', 'stage', 'validate') {
        if ($args.Count -lt 3 -or $args[2].StartsWith('--')) { throw 'Specify an external plugin path' }
        $pluginPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($args[2])
        $index = 3
    }
    $bump = ''
    if ("$command/$subject" -eq 'version/bump') {
        if ($args.Count -lt 3 -or $args[2] -notin 'patch', 'minor', 'major') { throw 'Choose patch, minor, or major' }
        $bump = $args[2]
        $index = 3
    }
    if ("$command/$subject" -in 'script/info', 'script/run') {
        if ($args.Count -lt 3) { throw 'Specify a script path from sfdev script list' }
        $scriptId = $args[2].Replace('\', '/')
        $index = 3
    }
    $allowed = switch ("$command/$subject") {
        'env/show' { 'json' }
        'env/shell' { 'dry-run' }
        'script/list' { 'json' }
        'script/info' { }
        'script/run' { 'dry-run' }
        'doctor/' { 'json', 'workflow' }
        'setup/' { 'components', 'dry-run' }
        'sdk/build' { 'dry-run' }
        'plugin/list' { }
        'plugin/new' { 'kind', 'domain', 'dry-run' }
        'plugin/build' { 'sdk', 'dry-run' }
        'plugin/test' { 'dry-run' }
        'plugin/stage' { 'dry-run' }
        'plugin/validate' { 'sdk', 'dry-run' }
        'release/prepare' { 'dry-run' }
        'release/verify' { 'dry-run' }
        'release/publish' { 'dry-run' }
        'configure/' { 'config', 'dry-run' }
        'build/backend' { 'target', 'jobs', 'dry-run' }
        'build/frontend' { 'dry-run' }
        'build/all' { 'jobs', 'dry-run' }
        'test/backend' { 'list', 'filter', 'label', 'jobs', 'dry-run' }
        'test/frontend' { 'watch', 'dry-run' }
        'test/browser' { 'url', 'dry-run' }
        'clean/' { 'scope', 'dry-run' }
        'check/frontend' { 'dry-run' }
        'run/backend' { 'url', 'dry-run' }
        'run/frontend' { 'url', 'dry-run' }
        'run/app' { 'url', 'dry-run' }
        'package/' { 'frontend-only', 'dry-run' }
        'version/show' { }
        'version/bump' { 'dry-run' }
        default { throw "Unknown command: $command $subject" }
    }
    $logExecution = $command -in 'configure', 'build', 'test', 'check', 'package', 'setup', 'sdk', 'release' -or
        ($command -eq 'plugin' -and $subject -ne 'list') -or
        "$command/$subject" -eq 'script/run'
    if ($logExecution) { $allowed = @($allowed) + 'json' }
    $options = @{}
    $forward = @()
    for (; $index -lt $args.Count; $index++) {
        $argument = $args[$index]
        if ($argument -eq '--' -and "$command/$subject" -in 'test/frontend', 'script/run') {
            $forward = @($args | Select-Object -Skip ($index + 1))
            break
        }
        $name = $argument -replace '^--', ''
        if (!$argument.StartsWith('--') -or $name -notin $allowed -or $options.ContainsKey($name)) {
            throw "Unexpected option: $argument"
        }
        if ($name -in 'json', 'list', 'watch', 'dry-run', 'frontend-only') { $options[$name] = $true; continue }
        if (++$index -ge $args.Count -or $args[$index].StartsWith('--')) { throw "Missing value for --$name" }
        $options[$name] = $args[$index]
    }
    $buildPlatform = if ($IsWindows) { 'windows/core' } else { 'linux/core' }
    $tree = Join-Path $root "tmp/build/$buildPlatform"
    $npmName = if ($IsWindows) { 'npm.cmd' } else { 'npm' }
    $pwsh = Join-Path $PSHOME $(if ($IsWindows) { 'pwsh.exe' } else { 'pwsh' })
    $nodeVersion = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build/node-version.txt') -Raw).Trim()
    $managedNodeDir = Join-Path $managedHome "tools/node/v$nodeVersion/$(if ($IsWindows) { 'win-x64' } else { 'linux-x64' })"
    $managedNodeBin = Join-Path $managedNodeDir 'bin'
    if ($IsWindows) { $managedNodeBin = $managedNodeDir }
    $nodeGlobal = Resolve-Tool node
    $npmGlobal = Resolve-Tool $npmName
    $nodeGlobalVersion = $null
    $globalNodeCompatible = $false
    if ($nodeGlobal -and $npmGlobal) {
        try {
            $nodeGlobalVersion = ((& $nodeGlobal --version 2>$null) -join '').Trim()
            if ($LASTEXITCODE -eq 0 -and $nodeGlobalVersion -match '^v(\d+)\.(\d+)\.(\d+)') {
                $nodeMajor = [int]$Matches[1]
                $nodeMinor = [int]$Matches[2]
                $globalNodeCompatible = ($nodeMajor -eq 22 -and $nodeMinor -ge 12) -or
                    $nodeMajor -eq 24 -or $nodeMajor -ge 26
            }
        } catch { $globalNodeCompatible = $false }
    }
    $nodeManaged = Join-Path $managedNodeBin $(if ($IsWindows) { 'node.exe' } else { 'node' })
    $npmManaged = Join-Path $managedNodeBin $npmName
    $useManagedNode = !$globalNodeCompatible
    $selectedNode = if ($useManagedNode -and (Test-Path -LiteralPath $nodeManaged -PathType Leaf) -and (Test-Path -LiteralPath $npmManaged -PathType Leaf)) {
        $nodeManaged
    } elseif (!$useManagedNode) { $nodeGlobal } else { $null }
    $selectedNpm = if ($selectedNode -eq $nodeManaged) { $npmManaged } elseif (!$useManagedNode) { $npmGlobal } else { $null }
    $tools = [ordered]@{
        pwsh = $pwsh
        git = Resolve-Tool git
        cmake = if ($IsWindows) { Join-Path $bin 'cmake.exe' } else { Resolve-Tool cmake }
        ctest = if ($IsWindows) { Join-Path $bin 'ctest.exe' } else { Resolve-Tool ctest }
        npm = $selectedNpm
        node = $selectedNode
        gcc = if ($IsWindows) { Join-Path $bin 'gcc.exe' } else { Resolve-Tool gcc }
        gxx = if ($IsWindows) { Join-Path $bin 'g++.exe' } else { Resolve-Tool 'g++' }
        ninja = if ($IsWindows) { Join-Path $bin 'ninja.exe' } else { Resolve-Tool ninja }
        python = Join-Path $root $(if ($IsWindows) { '.venv/Scripts/python.exe' } else { '.venv/bin/python' })
    }
    $basePython = Get-StreamFindBasePython
    $environment = [ordered]@{}
    if ($IsWindows) {
        $environment.STREAMFIND_MSYS2_ROOT = $ucrt
        $environment.MSYSTEM = 'UCRT64'
        $environment.PATH = "$bin;$env:PATH"
    } elseif ($IsLinux) {
        $environment.PATH = ($env:PATH.Split(':') | Where-Object { $_ -and $_ -notmatch '^/mnt/[a-z]/' }) -join ':'
    }
    if ($tools.node -eq $nodeManaged) {
        $environment.PATH = if ($IsWindows) { "$managedNodeBin;$($environment.PATH ?? $env:PATH)" }
            else { "$managedNodeBin`:$($environment.PATH ?? $env:PATH)" }
    }

    if ("$command/$subject" -in 'env/show', 'doctor/') {
        $workflow = $options.workflow ?? 'all'
        $required = switch ($workflow) {
            all { $tools.Keys }
            native { 'pwsh', 'cmake', 'ctest', 'gcc', 'gxx', 'ninja' }
            frontend { 'pwsh', 'node', 'npm' }
            scripts { 'pwsh', 'python' }
            default { throw "Unknown workflow: $workflow" }
        }
        $inventory = foreach ($entry in $tools.GetEnumerator()) {
            if ($entry.Key -notin $required) { continue }
            $available = [bool]($entry.Value -and (Test-Path -LiteralPath $entry.Value -PathType Leaf))
            $source = switch ($entry.Key) {
                pwsh { 'current PowerShell installation' }
                python { 'repository .venv' }
                { $IsWindows -and $_ -in 'cmake', 'ctest', 'gcc', 'gxx', 'ninja' } {
                    if ($ucrt.StartsWith($msys2ManagedRoot, [StringComparison]::OrdinalIgnoreCase)) { 'managed .streamfind tools' }
                    elseif ($env:STREAMFIND_MSYS2_ROOT) { 'STREAMFIND_MSYS2_ROOT' }
                    elseif ($ucrt -ieq 'C:\msys64') { 'system MSYS2 installation' }
                    else { 'MSYS2 UCRT64 found on PATH' }
                }
                { $_ -in 'node', 'npm' } {
                    if ($entry.Value -and $entry.Value.StartsWith($managedNodeDir, [StringComparison]::OrdinalIgnoreCase)) { 'managed .streamfind tools' }
                    else { 'PATH' }
                }
                default { 'PATH' }
            }
            $version = $null
            $diagnostic = $null
            if ($available) {
                try {
                    $versionLines = & $entry.Value --version 2>&1
                    if ($LASTEXITCODE -ne 0) { throw "Version command exited with code $LASTEXITCODE" }
                    $version = [string]($versionLines | Select-Object -First 1)
                    if (!$version) { throw 'Version command returned no output' }
                } catch { $diagnostic = $_.Exception.Message }
            } else {
                $diagnostic = switch ($entry.Key) {
                    python {
                        if ($basePython) { "Run sfdev setup --components python to create the repository .venv with Python $($basePython.Version)." }
                        else { 'Run sfdev setup --components python to install the pinned Python under .streamfind/tools and create the repository .venv.' }
                    }
                    { $IsWindows -and $_ -in 'cmake', 'ctest', 'gcc', 'gxx', 'ninja' } { 'Run sfdev setup --components native. Missing MSYS2 and UCRT64 packages are installed under .streamfind/tools.' }
                    { $_ -in 'node', 'npm' } {
                        if ($nodeGlobal -and !$globalNodeCompatible) { "PATH Node.js $nodeGlobalVersion is incompatible with the frontend dependencies; setup installs the pinned compatible version under .streamfind/tools." }
                        else { 'Run sfdev setup --components frontend to install the pinned Node.js/npm bundle under .streamfind/tools.' }
                    }
                    default { "Install $($entry.Key) and make it available on PATH." }
                }
            }
            [ordered]@{ name = $entry.Key; path = $entry.Value; source = $source; available = $available
                version = $version; diagnostic = $diagnostic }
        }
        $dataRoot = Join-Path (Split-Path -Parent $root) 'streamfind.data'
        $issues = @()
        if ($workflow -in 'all', 'native') {
            $cmakeVersion = ($inventory | Where-Object name -eq cmake).version
            if ($cmakeVersion -match '(\d+\.\d+\.\d+)' -and [version]$Matches[1] -lt [version]'3.25') {
                $issues += 'CMake 3.25 or later is required for the repository presets.'
            }
            if ($tools.gcc -and $tools.gxx -and (Test-Path $tools.gcc) -and (Test-Path $tools.gxx)) {
                $cTarget = & $tools.gcc -dumpmachine
                $cxxTarget = & $tools.gxx -dumpmachine
                $cVersion = & $tools.gcc -dumpfullversion
                $cxxVersion = & $tools.gxx -dumpfullversion
                if (!$cTarget -or !$cVersion -or $cTarget -ne $cxxTarget -or $cVersion -ne $cxxVersion) {
                    $issues += 'GCC and G++ must report matching targets and versions.'
                }
                $arrow = if ($IsWindows) { Join-Path $ucrt 'ucrt64/lib/cmake/Arrow/ArrowConfig.cmake' }
                    else { "/usr/lib/$cxxTarget/cmake/Arrow/ArrowConfig.cmake" }
                if (!(Test-Path -LiteralPath $arrow)) { $issues += "Arrow CMake package missing at $arrow. Run sfdev setup --components native." }
            }
            $javaName = if ($IsWindows) { 'java.exe' } else { 'java' }
            $javaReady = $false
            foreach ($jdk in Get-ChildItem -LiteralPath (Join-Path $managedHome 'tools/java') -Directory -Filter 'jdk-21.*' -ErrorAction SilentlyContinue) {
                $java = Join-Path $jdk.FullName "bin/$javaName"
                if (!(Test-Path -LiteralPath $java -PathType Leaf)) { continue }
                $javaVersion = & $java -version 2>&1
                if ($LASTEXITCODE -eq 0 -and ($javaVersion -join "`n") -match 'version "21(?:[."]|$)') { $javaReady = $true; break }
            }
            if (!$javaReady) { $issues += 'Managed Java 21 is unavailable. Run sfdev setup --components java.' }
        }
        $report = [ordered]@{ checkout = $root; build_directory = $tree; workflow = $workflow
            managed_home = $managedHome; default_data_root = $dataRoot
            default_data_available = (Test-Path -LiteralPath (Join-Path $dataRoot 'data') -PathType Container)
            tools = @($inventory); issues = $issues }
        if ($options.json) { $report | ConvertTo-Json -Depth 5 }
        else {
            "Checkout: $root"
            "Build directory: $tree"
            "Managed home: $managedHome"
            "Default data root: $dataRoot (available: $($report.default_data_available))"
            foreach ($tool in $inventory) {
                "$($tool.name): $($tool.version ?? 'unavailable') [$($tool.source)] $($tool.path)"
                if ($tool.diagnostic) { "  $($tool.diagnostic)" }
            }
            foreach ($issue in $issues) { "  $issue" }
        }
        if ($command -eq 'doctor' -and ($issues -or ($inventory | Where-Object { $_.diagnostic }))) { exit 1 }
        exit 0
    }
    if ($command -eq 'version') {
        $versionFile = Join-Path $root 'VERSION'
        $current = (Get-Content -LiteralPath $versionFile -Raw).Trim()
        if ($subject -eq 'show') { $current; exit 0 }
        if ($current -notmatch '^\d+\.\d+\.\d+$') { throw "Unsupported VERSION: $current" }
        $parsed = [version]$current
        $next = switch ($bump) {
            major { "$($parsed.Major + 1).0.0" }
            minor { "$($parsed.Major).$($parsed.Minor + 1).0" }
            patch { "$($parsed.Major).$($parsed.Minor).$($parsed.Build + 1)" }
        }
        if (!$options['dry-run']) { [IO.File]::WriteAllText($versionFile, "$next`n") }
        "$current -> $next"
        exit 0
    }
    if ($command -eq 'clean') {
        $scope = $options.scope
        if ($scope -notin 'build', 'all-temp') { throw 'Choose --scope build or --scope all-temp' }
        $tempRoot = Join-Path $root 'tmp'
        $buildRoot = Join-Path $tempRoot 'build'
        $parents = if ($scope -eq 'build') { @($tempRoot, $buildRoot) } else { @($tempRoot) }
        foreach ($parent in $parents) {
            $item = Get-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue
            if ($item -and (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
                throw "Cleanup requires a real directory: $parent"
            }
        }
        $targets = if ($scope -eq 'build') {
            Get-Item -LiteralPath $tree -Force -ErrorAction SilentlyContinue
        } elseif (Test-Path -LiteralPath $tempRoot) {
            Get-ChildItem -LiteralPath $tempRoot -Force
        }
        $links = @()
        foreach ($target in $targets) {
            if ($target.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                $links += $target
            } elseif ($target.PSIsContainer) {
                $links += @(Get-ChildItem -LiteralPath $target.FullName -Recurse -Force -Attributes ReparsePoint)
            }
            "Remove $($target.FullName)"
        }
        foreach ($link in $links) { "Remove link entry $($link.FullName)" }
        if (!$options['dry-run']) {
            foreach ($link in $links | Sort-Object { $_.FullName.Length } -Descending) {
                Remove-Item -LiteralPath $link.FullName -Force
            }
            foreach ($target in $targets) {
                if (Test-Path -LiteralPath $target.FullName) {
                    Remove-Item -LiteralPath $target.FullName -Recurse -Force
                }
            }
        }
        exit 0
    }
    $jobs = $options.jobs
    if ($jobs -and $jobs -notmatch '^[1-9][0-9]*$') { throw '--jobs must be a positive integer' }
    $steps = @()
    $releaseCommit = $null
    if ($command -eq 'plugin') {
        if ($subject -eq 'list') {
            Get-ChildItem -LiteralPath (Join-Path $root 'cpp/plugins') -Directory |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'CMakeLists.txt') } |
                Sort-Object Name | Select-Object -ExpandProperty Name
            exit 0
        }
        if ($subject -eq 'new') {
            $arguments = @((Join-Path $PSScriptRoot 'dev/plugin_generator.py'), 'init', $pluginPath)
            if ($options.kind) { $arguments += '--kind', $options.kind }
            if ($options.domain) { $arguments += '--domain', $options.domain }
            $steps += New-Step $root $tools.python $arguments
        } else {
            $source = Join-Path $pluginPath 'backend'
            if (!(Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) { $source = $pluginPath }
            if (!(Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) { throw "Native plugin CMakeLists.txt missing: $source" }
            $key = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($source))).Substring(0, 16)
            $pluginTree = Join-Path $root "tmp/build/external-plugins/$key"
            $stage = Join-Path $pluginTree 'stage'
            if ($subject -in 'build', 'validate') {
                if (!$options.sdk) { throw 'Specify the extracted developer kit with --sdk PATH' }
                $kit = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($options.sdk)
                $kitConfig = Join-Path $kit 'sdk/cmake/streamfind'
                if (!(Test-Path -LiteralPath (Join-Path $kitConfig 'streamfindConfig.cmake'))) { throw "Developer kit CMake package missing: $kitConfig" }
                $activePath = $environment.PATH ?? $env:PATH
                $environment.PATH = (Join-Path $kit 'bin') + [IO.Path]::PathSeparator + $activePath
            }
            switch ($subject) {
                build {
                    $steps += New-Step $root $tools.cmake @('-G', 'Ninja', '-S', $source, '-B', $pluginTree,
                        "-Dstreamfind_DIR=$kitConfig", "-DCMAKE_CXX_COMPILER=$($tools.gxx)",
                        "-DCMAKE_MAKE_PROGRAM=$($tools.ninja)", '-DCMAKE_BUILD_TYPE=Release')
                    $steps += New-Step $root $tools.cmake @('--build', $pluginTree)
                }
                test { $steps += New-Step $root $tools.ctest @('--test-dir', $pluginTree, '--output-on-failure', '--no-tests=error') }
                stage {
                    $step = New-Step $root $tools.cmake @('--install', $pluginTree, '--prefix', $stage)
                    $step['output'] = $stage
                    $steps += $step
                }
                validate {
                    $manifestFile = Join-Path $pluginTree 'plugin.json'
                    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
                    $platformKey = if ($IsWindows) { 'windows-x86_64' } else { 'linux-x86_64' }
                    if ($manifest.domain -notmatch '^[a-z][a-z0-9_]*$') { throw 'Invalid manifest domain' }
                    $validator = Join-Path $kit $(if ($IsWindows) { 'sdk/bin/streamfind_sdk_plugin_validator.exe' } else { 'sdk/bin/streamfind_sdk_plugin_validator' })
                    $steps += New-Step $root $validator @('--source-dir', $source,
                        '--output-dir', (Join-Path $stage "plugins/$($manifest.domain)"),
                        '--manifest', $manifestFile, '--domain', $manifest.domain, '--library', $manifest.library.$platformKey)
                }
            }
        }
    }
    if ($command -eq 'setup') {
        $components = @($options.components -split ',')
        if (!$options.components -or ($components | Where-Object { $_ -notin 'native', 'java', 'frontend', 'browser', 'python' })) {
            throw 'Choose components from native, java, frontend, browser, python'
        }
        if ('python' -in $components) {
            $steps += New-Step $root $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'build/setup-python.ps1'))
        }
        if ('native' -in $components) {
            if ($IsWindows) {
                $steps += New-Step $root $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'build/setup-msys2.ps1'), '-Root', $ucrt)
            } elseif ($IsLinux) {
                $steps += New-Step $root (Resolve-Tool sudo) @('apt-get', 'install', '--no-upgrade',
                    'build-essential', 'cmake', 'ninja-build', 'libarrow-dev', 'libabsl-dev', 'libbrotli-dev',
                    'libssl-dev', 'libprotobuf-dev', 'liblz4-dev', 'libre2-dev', 'libbz2-dev',
                    'libutf8proc-dev', 'libcurl4-openssl-dev', 'zlib1g-dev')
            } else { throw 'Native package setup supports Windows MSYS2 UCRT64 and Ubuntu/Debian APT.' }
        }
        if ('native' -in $components -or 'java' -in $components) {
            $steps += New-Step $root $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'build/setup-java.ps1'))
        }
        if (('frontend' -in $components -or 'browser' -in $components) -and !$tools.node) {
            $steps += New-Step $root $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'build/setup-node.ps1'))
            $tools.node = $nodeManaged
            $tools.npm = $npmManaged
            $environment.PATH = if ($IsWindows) { "$managedNodeBin;$($environment.PATH ?? $env:PATH)" }
                else { "$managedNodeBin`:$($environment.PATH ?? $env:PATH)" }
        }
        if ('frontend' -in $components) {
            $steps += New-Step $frontend $tools.npm @('ci', '--engine-strict')
        }
        if ('browser' -in $components) {
            $environment.PLAYWRIGHT_BROWSERS_PATH = Join-Path $managedHome 'tools/playwright'
            $playwright = Join-Path $frontend 'node_modules/playwright/cli.js'
            if ('frontend' -notin $components -and !$options['dry-run'] -and !(Test-Path -LiteralPath $playwright)) {
                throw 'Install frontend dependencies first with sfdev setup --components frontend'
            }
            $steps += New-Step $frontend $tools.node @($playwright, 'install', 'chromium')
        }
    }
    if ("$command/$subject" -eq 'test/browser') {
        $endpoint = $null
        if (![uri]::TryCreate($options.url, [UriKind]::Absolute, [ref]$endpoint) -or $endpoint.Scheme -notin 'http', 'https') {
            throw 'Specify an HTTP or HTTPS frontend URL with --url'
        }
        $environment.STREAMFIND_FRONTEND_URL = $endpoint.AbsoluteUri
        $environment.PLAYWRIGHT_BROWSERS_PATH = Join-Path $managedHome 'tools/playwright'
        $steps += New-Step $frontend $tools.npm @('run', 'browser:smoke')
    }
    if ($command -eq 'run') {
        $environment.STREAMFIND_DEV_BUILD_DIR = $tree
        $environment.STREAMFIND_SERVICE_URL = $options.url ?? $env:STREAMFIND_SERVICE_URL ?? 'http://127.0.0.1:8790'
        $arguments = @((Join-Path $frontend 'tools/dev/start-dev.mjs'))
        if ($subject -eq 'backend') { $arguments += '--backend-only' }
        if ($subject -eq 'frontend') { $arguments += '--frontend-only' }
        $steps += New-Step $frontend $tools.node $arguments
    }
    if ($command -eq 'release' -and $subject -in 'verify', 'publish') {
        $version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
        $arguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'release/publish-release.ps1'), '-Version', $version)
        if ($subject -eq 'verify') { $arguments += '-VerifyOnly' }
        else { $arguments += '-RequireProvenance' }
        $steps += New-Step $root $pwsh $arguments
    }
    if ($command -eq 'sdk' -or "$command/$subject" -eq 'release/prepare') {
        $version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
        if ($command -eq 'release' -and !$options['dry-run']) {
            $git = Resolve-Tool git
            if (!$git) { throw 'Git is required for release preparation' }
            $changes = & $git -C $root status --porcelain
            if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Release preparation requires a clean committed checkout' }
            $releaseCommit = & $git -C $root rev-parse HEAD
            if ($LASTEXITCODE -ne 0 -or $releaseCommit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot resolve the release source commit' }
        }
        $scriptName = if ($command -eq 'sdk') { 'package-plugin-devkit' } else { 'release-cpp' }
        $scriptDirectory = Join-Path $PSScriptRoot 'release/cpp'
        if ($command -eq 'sdk' -and $IsWindows) {
            $windowsSdkScript = Join-Path $scriptDirectory 'package-plugin-devkit.ps1'
            $steps += New-Step $root $pwsh @('-NoProfile', '-File', $windowsSdkScript, '-Version', $version)

            $wsl = Resolve-Tool 'wsl.exe'
            if ($wsl) {
                $wslListing = & $wsl --list --quiet 2>$null
                if ($LASTEXITCODE -eq 0) {
                    $distributionNames = (($wslListing -join "`n").Replace([string][char]0, '') -split '\r?\n' |
                        ForEach-Object { $_.Trim() } | Where-Object { $_ })
                    $ubuntu = $distributionNames | Where-Object { $_ -match '^Ubuntu(?:[-_ ].*)?$' } | Select-Object -First 1
                    if ($ubuntu) {
                        $linuxSdkScript = Join-Path $scriptDirectory 'package-plugin-devkit-linux.sh'
                        $linuxScriptPath = & $wsl --distribution $ubuntu --exec wslpath -a $linuxSdkScript
                        if ($LASTEXITCODE -eq 0 -and $linuxScriptPath) {
                            $linuxScriptPath = ([string]$linuxScriptPath).Replace([string][char]0, '').Trim()
                            $steps += New-Step $root $wsl @('--distribution', $ubuntu, '--exec', '/usr/bin/env',
                                'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
                                'STREAMFIND_SKIP_FRONTEND_BUILD=1', '/bin/bash', $linuxScriptPath, $version)
                        } else {
                            Write-Warning "Ubuntu WSL distribution '$ubuntu' is installed but could not resolve the Linux SDK build script path; building the Windows SDK only."
                        }
                    }
                }
            }
        } elseif ($IsWindows) {
            $steps += New-Step $root $pwsh @('-NoProfile', '-File',
                (Join-Path $scriptDirectory "$scriptName.ps1"), '-Version', $version)
        } elseif ($IsLinux) {
            $steps += New-Step $root (Resolve-Tool bash) @((Join-Path $scriptDirectory "$scriptName-linux.sh"), $version)
        } else { throw 'Release and SDK packaging support Windows and Linux' }
    }
    if ($command -eq 'package') {
        if (!$IsWindows) { throw 'The unpacked test package is available on Windows.' }
        $packageRoot = Join-Path $root 'tmp/packaged_build'
        if ($options['frontend-only']) {
            $packageScript = 'update-packaged-frontend.ps1'
            $packageStages = @('Build frontend', 'Refresh package app directory')
        } else {
            $packageScript = 'package-cpp.ps1'
            $packageStages = @('Build frontend', 'Configure native Release build', 'Build native targets',
                'Run CTest', 'Stage and validate unpacked package')
        }
        $packageStep = New-Step $root $pwsh @('-NoProfile', '-File',
            (Join-Path $PSScriptRoot "build/cpp/$packageScript"))
        $packageStep['stages'] = $packageStages
        $packageStep['output'] = $packageRoot
        $steps += $packageStep
    }
    if ("$command/$subject" -eq 'env/shell') {
        $environment.STREAMFIND_DEV_ENTRYPOINT = $PSCommandPath
        $steps += New-Step $root $pwsh @('-NoProfile', '-NoExit', '-Command',
            'function global:sfdev { & $env:STREAMFIND_DEV_ENTRYPOINT @args }; Write-Host "streamfind development shell. Run sfdev help."')
    }
    if ($command -eq 'script') {
        $devRoot = Get-Item -LiteralPath (Join-Path $PSScriptRoot 'dev')
        if ($devRoot.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The development script directory must be inside the checkout' }
        $scripts = @(Get-DevelopmentScripts $devRoot.FullName | Sort-Object -Property id)
        foreach ($script in $scripts) {
            $interpreter = switch ($script.extension) {
                '.ps1' { $pwsh }
                '.py' { $tools.python }
                '.sh' { Resolve-Tool bash }
                default { if ($IsWindows) { $script.path } }
            }
            $script | Add-Member -NotePropertyName interpreter -NotePropertyValue $interpreter
            $script | Add-Member -NotePropertyName available -NotePropertyValue (
                [bool]($interpreter -and (Test-Path -LiteralPath $interpreter -PathType Leaf)))
        }
        if ($subject -eq 'list') {
            if ($options.json) { ConvertTo-Json -InputObject $scripts -Depth 4 }
            else { $scripts | Format-Table id, available }
            exit 0
        }
        $script = $scripts | Where-Object { $_.id -ceq $scriptId }
        if (!$script) { throw "Unknown development script: $scriptId" }
        $arguments = switch ($script.extension) {
            '.ps1' { '-NoProfile'; '-File'; $script.path }
            { $_ -in '.sh', '.py' } { $script.path }
        }
        if ($subject -eq 'info') {
            [ordered]@{ script = $script; directory = $root; arguments = @($arguments) } | ConvertTo-Json -Depth 5
            exit 0
        }
        if (!$script.available) { throw "Interpreter unavailable for $scriptId. Run sfdev doctor." }
        $steps += New-Step $root $script.interpreter (@($arguments) + $forward)
    }
    if ($command -eq 'configure') {
        $config = $options.config ?? 'Debug'
        if ($config -cnotin 'Debug', 'Release') { throw 'Use Debug or Release for --config' }
        $cacheFile = Join-Path $tree 'CMakeCache.txt'
        if (Test-Path -LiteralPath $cacheFile -PathType Leaf) {
            if ($IsLinux -and !$options['dry-run']) {
                $cacheText = Get-Content -LiteralPath $cacheFile -Raw
                $mixedLinuxCache = $cacheText -match '(?im)msys64|(^|[=;])/(ucrt64|mingw64|clang64)(/|$)|(?:^|\n)(?:[^=\r\n]+_DIR|CMAKE_PREFIX_PATH|CMAKE_MODULE_PATH):[^=\r\n]*=[^\r\n]*[a-z]:[\\/]|CMAKE_(C|CXX)_COMPILER:[^=]*=[^\r\n]*\.exe|CMAKE_MAKE_PROGRAM:[^=]*=[^\r\n]*\.exe'
                if ($mixedLinuxCache) {
                    $managedBuildRoot = [IO.Path]::GetFullPath((Join-Path $root 'tmp/build'))
                    $managedBuildPrefix = $managedBuildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
                    $fullTree = [IO.Path]::GetFullPath($tree)
                    if (!$fullTree.StartsWith($managedBuildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                        throw "Refusing to remove a contaminated Linux build tree outside tmp/build: $fullTree"
                    }
                    Write-Warning "Removing Linux build tree with Windows/MSYS2 CMake paths: $fullTree"
                    Remove-Item -LiteralPath $fullTree -Recurse -Force
                }
            }
        }
        if (Test-Path -LiteralPath $cacheFile -PathType Leaf) {
            $cache = ConvertFrom-StringData ((Get-Content -LiteralPath $cacheFile |
                Where-Object { $_ -match '^(CMAKE_GENERATOR|CMAKE_HOME_DIRECTORY):[^=]+=' } |
                ForEach-Object { $_ -replace ':[^=]+=', '=' }) -join "`n")
            if ($cache.CMAKE_GENERATOR -cne 'Ninja' -or $cache.CMAKE_HOME_DIRECTORY -ne (Join-Path $root 'cpp').Replace('\', '/')) {
                throw 'Incompatible CMake build tree. Preview cleanup with sfdev clean --scope build --dry-run.'
            }
        }
        $steps += New-Step $root $tools.cmake @('--preset', $platform, '-S', (Join-Path $root 'cpp'),
            '-B', $tree, "-DCMAKE_BUILD_TYPE=$config", '-DSTREAMFIND_ENABLED_PLUGINS=ALL', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
    }
    if ($command -in 'build', 'test' -and $subject -in 'backend', 'all') {
        if (!$options['dry-run'] -and !(Test-Path -LiteralPath (Join-Path $tree 'CMakeCache.txt'))) {
            throw 'Native build is not configured. Run sfdev configure.'
        }
        if ($command -eq 'build') {
            $arguments = @('--build', $tree)
            if ($options.target) { $arguments += '--target', $options.target }
            if ($jobs) { $arguments += '--parallel', $jobs }
            $steps += New-Step $root $tools.cmake $arguments
        } else {
            $arguments = @('--test-dir', $tree, '--output-on-failure')
            if ($options.list) { $arguments += '-N' }
            if ($options.filter) { $arguments += '-R', $options.filter }
            if ($options.label) { $arguments += '-L', $options.label }
            if ($jobs) { $arguments += '-j', $jobs }
            $steps += New-Step $root $tools.ctest $arguments
        }
    }
    if ($command -in 'build', 'test', 'check' -and $subject -in 'frontend', 'all') {
        $tasks = switch ($command) {
            build { 'build' }
            check { 'build', 'lint', 'format:check' }
            test { if ($options.watch) { 'test' } else { 'test:run' } }
        }
        foreach ($task in $tasks) {
            $arguments = @('run', $task)
            if ($forward.Count) { $arguments += @('--') + $forward }
            $steps += New-Step $frontend $tools.npm $arguments
        }
    }
    if ($options['dry-run']) {
        [ordered]@{ environment = $environment; steps = $steps } | ConvertTo-Json -Depth 6
        exit 0
    }
    $previousEnvironment = @{}
    $log = $null
    $code = 0
    try {
        foreach ($entry in $environment.GetEnumerator()) {
            $previousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
        }
        if ($logExecution) {
            $logDirectory = Join-Path $root 'tmp/logs'
            [void][IO.Directory]::CreateDirectory($logDirectory)
            $log = Join-Path $logDirectory "sfdev-$command-$([guid]::NewGuid().ToString('N')).log"
            if (!$options.json) { Write-Host "Log: $log" }
        }
        foreach ($step in $steps) {
            $stepDescription = $step | ConvertTo-Json -Compress -Depth 4
            if (!$options.json) { Write-Host $stepDescription }
            Push-Location -LiteralPath $step.directory
            try {
                $arguments = $step.arguments
                if ($log) {
                    Add-Content -LiteralPath $log -Value $stepDescription -Encoding utf8
                    & $step.executable @arguments 2>&1 | ForEach-Object {
                        $line = [string]$_
                        Add-Content -LiteralPath $log -Value $line -Encoding utf8
                        if ($options.json) { [Console]::Error.WriteLine($line) }
                        else { $line }
                    }
                } else {
                    & $step.executable @arguments
                }
                $code = $LASTEXITCODE
            } finally { Pop-Location }
            if ($code -ne 0) { break }
        }
    } finally {
        foreach ($entry in $previousEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
        }
    }
    if ($code -eq 0 -and $releaseCommit) {
        $currentCommit = & $git -C $root rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $currentCommit -ne $releaseCommit) { throw 'Source commit changed during release preparation' }
        $changes = & $git -C $root status --porcelain
        if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Checkout changed during release preparation' }
        $archiveName = if ($IsWindows) { "streamfind-core-cpp-$version-Windows-x86_64.zip" }
            else { "streamfind-core-cpp-$version-Linux-x86_64.tgz" }
        $archive = Join-Path $root "tmp/release-output/$archiveName"
        [ordered]@{ version = $version; commit = $releaseCommit; archive = $archiveName
            sha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
            cmake = $tools.cmake; compiler = $tools.gxx; plugins = 'ALL' } |
            ConvertTo-Json | Set-Content -LiteralPath "$archive.provenance.json" -Encoding utf8
    }
    if ($options.json) {
        [ordered]@{ status = $(if ($code -eq 0) { 'ok' } else { 'failed' }); exit_code = $code; log = $log } |
            ConvertTo-Json
    }
    exit $code
} catch {
    [Console]::Error.WriteLine("sfdev: $($_.Exception.Message)")
    exit 1
}
