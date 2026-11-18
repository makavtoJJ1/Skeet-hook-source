<#
.SYNOPSIS
Build the Windows x86 client and check every core definition at link time.
.DESCRIPTION
Requires CMake 3.21+, Ninja, Python 3.9+ and LLVM-MinGW with i686 tools.
Dependencies are built from the vendored sources; no downloads are performed.
Set SKEET_LLVM_MINGW_ROOT or pass -ToolchainRoot explicitly.
.EXAMPLE
.\build.ps1 -ToolchainRoot C:\Tools\llvm-mingw
.EXAMPLE
.\build.ps1 -ToolchainRoot C:\Tools\llvm-mingw -CMake C:\Tools\cmake.exe -Ninja C:\Tools\ninja.exe
#>
[CmdletBinding()]
param(
    [string]$ToolchainRoot = $env:SKEET_LLVM_MINGW_ROOT,
    [string]$CMake = 'cmake',
    [string]$Ninja = 'ninja',
    [string]$BuildDirectory,
    [ValidateRange(1, 256)]
    [int]$Jobs = 2,
    [ValidateSet('Idle', 'BelowNormal', 'Normal')]
    [string]$Priority = 'BelowNormal',
    [ValidateSet('skeet_core', 'skeet_client', 'skeet_core_linkcheck')]
    [string[]]$Targets = @('skeet_client')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot 'build\native-core-x86'
}
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    throw 'Pass -ToolchainRoot or set SKEET_LLVM_MINGW_ROOT to your LLVM-MinGW installation.'
}
$toolchainPath = (Resolve-Path -LiteralPath $ToolchainRoot).Path
$cmakePath = (Get-Command $CMake -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source
$ninjaPath = (Get-Command $Ninja -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source
$buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
if ($buildPath.TrimEnd('\', '/') -eq $PSScriptRoot.TrimEnd('\', '/')) {
    throw 'Use a separate build directory; generated files must not be written into the source root.'
}

$buildProcess = [System.Diagnostics.Process]::GetCurrentProcess()
$previousPriority = $buildProcess.PriorityClass
try {
    # Child CMake/Ninja/compiler processes inherit BelowNormal/Idle priority.
    # Keep one incremental build directory; never clean it between patches.
    $buildProcess.PriorityClass = $Priority
    & $cmakePath -S $PSScriptRoot -B $buildPath -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$PSScriptRoot/cmake/llvm-mingw-x86.cmake" `
    "-DSKEET_LLVM_MINGW_ROOT=$toolchainPath" `
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath" -DCMAKE_BUILD_TYPE=Release `
        "-DSKEET_COMPILE_JOBS=$Jobs" "-DSKEET_LINK_THREADS=$Jobs"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed (exit $LASTEXITCODE)."
    }

    & $cmakePath --build $buildPath --target @Targets --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Client build failed (exit $LASTEXITCODE)."
    }
} finally {
    $buildProcess.PriorityClass = $previousPriority
}
if ($Targets -contains 'skeet_client') {
    Write-Output (Join-Path $buildPath 'bin\skeet_client.dll')
}
