#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$managedHome = $env:STREAMFIND_HOME ?? (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.streamfind')
$javaRoot = Join-Path $managedHome 'tools/java'
$javaName = if ($IsWindows) { 'java.exe' } else { 'java' }
foreach ($jdk in Get-ChildItem -LiteralPath $javaRoot -Directory -Filter 'jdk-21*' -ErrorAction SilentlyContinue) {
    $java = Join-Path $jdk.FullName "bin/$javaName"
    if (Test-Path -LiteralPath $java -PathType Leaf) {
        $version = & $java -version 2>&1
        if ($LASTEXITCODE -eq 0 -and ($version -join "`n") -match 'version "21(?:[."]|$)') {
            Write-Host "Managed Java already available at $java"
            exit 0
        }
    }
}
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64' -or (!$IsWindows -and !$IsLinux)) {
    throw 'Managed Java setup supports Windows and Linux x64.'
}
$version = '21.0.13'
$build = '9'
$asset, $sha = if ($IsWindows) {
    "windows_hotspot_${version}_$build.zip", 'a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9'
} else {
    "linux_hotspot_${version}_$build.tar.gz", 'd7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c'
}
$jdkName = "jdk-$version+$build"
$destination = Join-Path $javaRoot $jdkName
if (Test-Path -LiteralPath $destination) { throw "Managed JDK destination already exists and failed validation: $destination" }
$scratch = Join-Path $root "tmp/scratch/java-$([guid]::NewGuid().ToString('N'))"
$archive = Join-Path $scratch "OpenJDK21U-jdk_x64_$asset"
$url = "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-$version%2B$build/$(Split-Path -Leaf $archive)"
[void][IO.Directory]::CreateDirectory($scratch)
try {
    Invoke-WebRequest -Uri $url -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha) { throw 'Temurin archive SHA-256 mismatch' }
    & tar -xf $archive -C $scratch
    if ($LASTEXITCODE -ne 0) { throw "Temurin extraction failed ($LASTEXITCODE)" }
    $extracted = Join-Path $scratch $jdkName
    $java = Join-Path $extracted "bin/$javaName"
    & $java -version
    if ($LASTEXITCODE -ne 0) { throw 'Extracted Temurin executable failed validation' }
    [void][IO.Directory]::CreateDirectory($javaRoot)
    Move-Item -LiteralPath $extracted -Destination $destination
    Write-Host "Installed managed Java at $destination"
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
