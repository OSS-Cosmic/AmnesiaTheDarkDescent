# Windows build wrapper for Amnesia64 (Premake + MSBuild).
# This is a convenience path for command-line builds; developers can still open
# build-premake\Amnesia.sln directly after running `premake5 vs2026`.
# Requires premake5.exe on PATH and a VS 2026 install. MSBuild is auto-located
# via vswhere, so a regular PowerShell is enough.

$ErrorActionPreference = 'Stop'

function Show-Usage {
    Write-Host @'
Usage: .\build-windows.ps1 [release|debug] [options] [-- <extra premake args>]

Options:
    -Clean              Remove build-premake\ before generating
    -GameDir <path>     Path to your Amnesia: The Dark Descent install
                        (default: ATDD_DIR or AMNESIA_GAME_DIRECTORY)
    -Help               Show this help

Examples:
    .\build-windows.ps1
    .\build-windows.ps1 debug
    .\build-windows.ps1 release -Clean
    .\build-windows.ps1 release -GameDir "C:\Games\Amnesia The Dark Descent"
    .\build-windows.ps1 release -- --with-tools=no
'@
}

$config = 'release'
$clean = $false
$gameDir = $null
$extraArgs = @()

$scriptArgs = @($args)
$i = 0
while ($i -lt $scriptArgs.Count) {
    $arg = [string]$scriptArgs[$i]

    if ($arg -eq '--') {
        if ($i + 1 -lt $scriptArgs.Count) {
            $extraArgs = @($scriptArgs[($i + 1)..($scriptArgs.Count - 1)])
        }
        break
    } elseif ($arg -ieq 'release' -or $arg -ieq 'debug') {
        $config = $arg.ToLowerInvariant()
        $i++
    } elseif ($arg -ieq '-Clean' -or $arg -ieq '--clean') {
        $clean = $true
        $i++
    } elseif ($arg -ieq '-NoDeploy' -or $arg -ieq '--no-deploy') {
        Write-Host "==> -NoDeploy is obsolete: builds no longer stage assets"
        $i++
    } elseif ($arg -ieq '-GameDir' -or $arg -ieq '--game-dir') {
        if ($i + 1 -ge $scriptArgs.Count) {
            throw "$arg requires a path argument."
        }
        $gameDir = [string]$scriptArgs[$i + 1]
        $i += 2
    } elseif ($arg.StartsWith('--game-dir=')) {
        $gameDir = $arg.Substring('--game-dir='.Length)
        $i++
    } elseif ($arg -ieq '-Help' -or $arg -ieq '--help' -or $arg -ieq '-h' -or $arg -ieq '/?') {
        Show-Usage
        exit 0
    } else {
        Show-Usage
        throw "Unknown argument: $arg"
    }
}

if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
    throw "build-windows.ps1 must be run on Windows."
}

$root = $PSScriptRoot
if (-not $root) {
    $root = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$root = [System.IO.Path]::GetFullPath($root)
Set-Location $root

$cfgName = if ($config -eq 'release') { 'Release' } else { 'Debug' }
$buildDir = [System.IO.Path]::GetFullPath((Join-Path $root 'build-premake'))

if ($clean -and (Test-Path -LiteralPath $buildDir)) {
    $expected = [System.IO.Path]::GetFullPath((Join-Path $root 'build-premake'))
    if ($buildDir -ne $expected) {
        throw "Refusing to clean unexpected build directory: $buildDir"
    }
    Write-Host "==> Cleaning $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

if (-not (Test-Path -LiteralPath (Join-Path $root 'HPL2\extern\SDL\CMakeLists.txt'))) {
    Write-Host "==> Initialising git submodules"
    & git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "submodule init failed" }
}

if (-not $gameDir -and $env:ATDD_DIR) {
    $gameDir = $env:ATDD_DIR
} elseif (-not $gameDir -and $env:AMNESIA_GAME_DIRECTORY) {
    $gameDir = $env:AMNESIA_GAME_DIRECTORY
}

if ($gameDir) {
    $gameDir = [System.IO.Path]::GetFullPath($gameDir)
    if (-not (Test-Path -LiteralPath $gameDir -PathType Container)) {
        throw "Game directory not found: $gameDir"
    }

    # The generated VS projects use $(ATDD_DIR) for Debugging > Working Directory
    # and for copying freshly compiled Slang shaders into the game data folder.
    $env:ATDD_DIR = $gameDir
    Write-Host "==> ATDD_DIR=$gameDir"
}

$premake = (Get-Command premake5 -ErrorAction SilentlyContinue).Source
if (-not $premake) {
    throw "premake5.exe was not found on PATH."
}

$premakeArgs = @('vs2026')
if ($extraArgs) {
    $premakeArgs += $extraArgs
}

Write-Host "==> Generating Visual Studio 2026 solution"
& $premake @premakeArgs
if ($LASTEXITCODE -ne 0) { throw "premake5 vs2026 failed" }

$sln = Join-Path $buildDir 'Amnesia.sln'
if (-not (Test-Path -LiteralPath $sln)) {
    throw "Expected solution was not generated: $sln"
}

$msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue).Source
if (-not $msbuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "msbuild not on PATH and vswhere not found at $vswhere. Install VS 2026 or VS Build Tools."
    }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw "vswhere did not return an MSBuild.exe path." }
}

Write-Host "==> Building $cfgName with $msbuild"
& $msbuild $sln "/p:Configuration=$cfgName" '/p:Platform=x64' '/m' '/v:m'
if ($LASTEXITCODE -ne 0) { throw "msbuild failed" }

Write-Host "==> Build complete: build-premake\amnesia\$cfgName\"
