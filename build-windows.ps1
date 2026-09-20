# Windows build wrapper for Amnesia64 (Premake + MSBuild).
# This is a convenience path for command-line builds; developers can still open
# build-premake\Amnesia.sln directly after running `premake5 vs2026`.
# Uses the repository's pinned Premake release, downloading it when necessary.
# MSBuild is auto-located via vswhere, so a regular PowerShell is enough.

$ErrorActionPreference = 'Stop'

function Show-Usage {
    Write-Host @'
Usage: .\build-windows.ps1 [release|debug] [options] [--<premake-opt>=<value> ...]

Options:
    -Clean              Remove build-premake\ before generating
    -WithTest           Build and run the unit tests (disabled by default)
    -GameDir <path>     Path to your Amnesia: The Dark Descent install
                        (default: ATDD_DIR or AMNESIA_GAME_DIRECTORY)
    -Help               Show this help

Any additional --foo / --foo=bar arguments are forwarded to premake5.

Examples:
    .\build-windows.ps1
    .\build-windows.ps1 debug
    .\build-windows.ps1 debug -WithTest
    .\build-windows.ps1 release -Clean
    .\build-windows.ps1 release -GameDir "C:\Games\Amnesia The Dark Descent"
    .\build-windows.ps1 release --with-tools=no
'@
}

$config = 'release'
$clean = $false
$withTest = $false
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
    } elseif ($arg -ieq '-WithTest' -or $arg -ieq '-with-test' -or $arg -ieq '--with-test') {
        $withTest = $true
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
    } elseif ($arg.StartsWith('--')) {
        # Pass through unknown --foo / --foo=bar to premake. PowerShell strips a
        # bare '--' before it reaches this script, so any extra premake args must
        # be recognised individually rather than after a terminator.
        $extraArgs += $arg
        $i++
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

$premakeVersion = '5.0.0-beta8'
$premakeSha256 = 'e64ce2ed8778e0098f63674cca61fe33941b5f0c8d9a4afd651152bdea3758ab'
$premakeCommand = Get-Command premake5 -ErrorAction SilentlyContinue
$premake = $null

if ($premakeCommand) {
    $previousLocation = Get-Location
    try {
        Set-Location ([System.IO.Path]::GetTempPath())
        $detectedVersion = (& $premakeCommand.Source --version 2>&1 | Out-String).Trim()
        $versionExitCode = $LASTEXITCODE
    } finally {
        Set-Location $previousLocation
    }

    if ($versionExitCode -eq 0 -and $detectedVersion -match [regex]::Escape($premakeVersion)) {
        $premake = $premakeCommand.Source
    } else {
        Write-Host "==> Ignoring incompatible Premake on PATH: $detectedVersion"
    }
}

if (-not $premake) {
    $premakeDir = Join-Path $buildDir "_deps\premake\$premakeVersion"
    $premake = Join-Path $premakeDir 'premake5.exe'

    if (-not (Test-Path -LiteralPath $premake -PathType Leaf)) {
        $premakeArchive = Join-Path $premakeDir "premake-$premakeVersion-windows.zip"
        $premakeUrl = "https://github.com/premake/premake-core/releases/download/v$premakeVersion/premake-$premakeVersion-windows.zip"

        Write-Host "==> Downloading Premake $premakeVersion"
        New-Item -ItemType Directory -Path $premakeDir -Force | Out-Null
        Invoke-WebRequest -UseBasicParsing -Uri $premakeUrl -OutFile $premakeArchive

        $actualHash = (Get-FileHash -LiteralPath $premakeArchive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $premakeSha256) {
            throw "Premake archive checksum mismatch: expected $premakeSha256, got $actualHash"
        }

        Expand-Archive -LiteralPath $premakeArchive -DestinationPath $premakeDir -Force
        if (-not (Test-Path -LiteralPath $premake -PathType Leaf)) {
            throw "premake5.exe was not found after extracting $premakeArchive"
        }
    }
}

Write-Host "==> Using Premake $premakeVersion from $premake"

$testOption = if ($withTest) { 'yes' } else { 'no' }
$premakeArgs = @(
    'vs2026',
    "--with-tests=$testOption",
    "--with-python-tests=$testOption"
)
if ($extraArgs) {
    $premakeArgs += $extraArgs
}

Write-Host "==> Generating Visual Studio 2026 solution"
& $premake @premakeArgs
if ($LASTEXITCODE -ne 0) { throw "premake5 vs2026 failed" }

$solutionCandidates = @(
    (Join-Path $buildDir 'Amnesia.slnx'),
    (Join-Path $buildDir 'Amnesia.sln')
)
$sln = $solutionCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $sln) {
    throw "Expected solution was not generated: $($solutionCandidates -join ' or ')"
}

$msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue).Source
if (-not $msbuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "msbuild not on PATH and vswhere not found at $vswhere. Install VS 2022 or VS Build Tools."
    }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw "vswhere did not return an MSBuild.exe path." }
}

Write-Host "==> Building $cfgName with $msbuild"
& $msbuild $sln "/p:Configuration=$cfgName" '/p:Platform=x64' '/m:4' '/v:m'
if ($LASTEXITCODE -ne 0) { throw "msbuild failed" }

Write-Host "==> Build complete: build-premake\amnesia\$cfgName\"
