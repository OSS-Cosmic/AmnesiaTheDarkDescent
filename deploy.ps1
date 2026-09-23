# Stage a runnable Amnesia directory next to the Windows build output.

$ErrorActionPreference = 'Stop'

function Show-Usage {
    Write-Host @'
Usage: .\deploy.ps1 [options]

Options:
    -GameDir, --game-dir <path>      Installed Amnesia: The Dark Descent directory
    -Config, --config <value>        release | debug | all (default: all)
    -Resources, --resources <mode>   copy | merge | none (default: copy)
    -NoGameAssets, --no-game-assets  Skip the installed-game asset copy
    -Output, --output <dir>          Parent of the configuration directories
    -Overlay, --overlay <dir>        Redux resource overlay directory
    -Help, --help                    Show this help

Examples:
    .\deploy.ps1
    .\deploy.ps1 -Config release
    .\deploy.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Amnesia The Dark Descent"
    .\deploy.ps1 -Resources merge
    .\deploy.ps1 -Config debug -NoGameAssets
'@
}

function Get-AbsolutePath([string] $Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Get-RelativeFilePath([string] $BasePath, [string] $FilePath) {
    $separator = [System.IO.Path]::DirectorySeparatorChar
    $prefix = (Get-AbsolutePath $BasePath).TrimEnd('\', '/') + $separator
    $fullFile = Get-AbsolutePath $FilePath
    if (-not $fullFile.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "File is outside the deployment source: $fullFile"
    }
    return $fullFile.Substring($prefix.Length).Replace('\', '/')
}

function Get-SafeDestinationPath([string] $Destination, [string] $RelativePath) {
    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Invalid rooted path in overlay manifest: $RelativePath"
    }

    $separator = [System.IO.Path]::DirectorySeparatorChar
    $destinationRoot = (Get-AbsolutePath $Destination).TrimEnd('\', '/')
    $candidate = Get-AbsolutePath (Join-Path $destinationRoot ($RelativePath.Replace('/', $separator)))
    $prefix = $destinationRoot + $separator
    if (-not $candidate.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the deployment directory: $RelativePath"
    }
    return $candidate
}

function Copy-GameAssets([string] $Source, [string] $Destination) {
    $excludedExtensions = @('.rar', '.pdf', '.dll', '.exe')
    foreach ($file in Get-ChildItem -LiteralPath $Source -File -Recurse -Force) {
        if ($excludedExtensions -contains $file.Extension.ToLowerInvariant()) { continue }

        $relative = Get-RelativeFilePath $Source $file.FullName
        # Only the top-level retail files; assets like entities/bottle/amnesia/amnesia_bottle*.ent must be copied.
        if (-not $relative.Contains('/') -and $file.Name.StartsWith('Amnesia', [System.StringComparison]::OrdinalIgnoreCase)) { continue }
        $target = Get-SafeDestinationPath $Destination $relative
        $targetDirectory = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}

function Copy-DirectoryContents([string] $Source, [string] $Destination) {
    foreach ($directory in Get-ChildItem -LiteralPath $Source -Directory -Recurse -Force) {
        $relative = Get-RelativeFilePath $Source $directory.FullName
        $target = Get-SafeDestinationPath $Destination $relative
        if (-not (Test-Path -LiteralPath $target -PathType Container)) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $Source -File -Recurse -Force) {
        $relative = Get-RelativeFilePath $Source $file.FullName
        $target = Get-SafeDestinationPath $Destination $relative
        $targetDirectory = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}

function Restore-MergedOriginals([string] $Destination, [string] $BackupSuffix) {
    foreach ($backup in Get-ChildItem -LiteralPath $Destination -File -Recurse -Force |
            Where-Object { $_.Name.EndsWith($BackupSuffix, [System.StringComparison]::Ordinal) }) {
        $original = $backup.FullName.Substring(0, $backup.FullName.Length - $BackupSuffix.Length)
        Move-Item -LiteralPath $backup.FullName -Destination $original -Force
    }
}

function Invoke-MapDelta([string] $MapDelta, [string] $Destination, [string] $Overlay) {
    $python = Get-Command python -ErrorAction SilentlyContinue
    $pythonArgs = @()
    if (-not $python) {
        $python = Get-Command py -ErrorAction SilentlyContinue
        if ($python) { $pythonArgs = @('-3') }
    }
    if (-not $python) {
        $python = Get-Command python3 -ErrorAction SilentlyContinue
    }
    if (-not $python) {
        throw 'Python 3 is required for --resources merge, but python, py, and python3 were not found on PATH.'
    }

    & $python.Source @pythonArgs $MapDelta apply $Destination $Overlay --in-place --backup
    if ($LASTEXITCODE -ne 0) {
        throw "mapdelta.py failed with exit code $LASTEXITCODE"
    }
}

function Deploy-Resources(
    [string] $Destination,
    [string] $Overlay,
    [string] $Mode,
    [string] $MapDelta,
    [string] $ManifestName,
    [string] $BackupSuffix
) {
    $allFiles = @(Get-ChildItem -LiteralPath $Overlay -File -Recurse -Force |
        ForEach-Object { Get-RelativeFilePath $Overlay $_.FullName } |
        Sort-Object)

    $placed = @()
    if ($Mode -ne 'none') {
        $placed = @($allFiles | Where-Object {
            $isDelta = $_.EndsWith('.map_delta', [System.StringComparison]::OrdinalIgnoreCase) -or
                $_.EndsWith('.ent_delta', [System.StringComparison]::OrdinalIgnoreCase)
            $Mode -ne 'merge' -or -not $isDelta
        })
    }

    $manifest = Join-Path $Destination $ManifestName
    $placedSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    foreach ($relative in $placed) { [void] $placedSet.Add($relative) }

    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        foreach ($relative in Get-Content -LiteralPath $manifest) {
            if ([string]::IsNullOrWhiteSpace($relative) -or $placedSet.Contains($relative)) { continue }
            $stale = Get-SafeDestinationPath $Destination $relative
            if (Test-Path -LiteralPath $stale -PathType Leaf) {
                Remove-Item -LiteralPath $stale -Force
                Write-Host "    removed $relative"
            }
        }
    }

    Restore-MergedOriginals $Destination $BackupSuffix
    if ($Mode -eq 'merge') {
        Write-Host "    baking deltas into $Destination"
        Invoke-MapDelta $MapDelta $Destination $Overlay
    }

    foreach ($relative in $placed) {
        $source = Get-SafeDestinationPath $Overlay $relative
        $target = Get-SafeDestinationPath $Destination $relative
        $targetDirectory = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) {
            New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $source -Destination $target -Force
    }

    if ($placed.Count -gt 0) {
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllLines($manifest, [string[]] $placed, $utf8NoBom)
    } elseif (Test-Path -LiteralPath $manifest) {
        Remove-Item -LiteralPath $manifest -Force
    }
}

$root = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$root = Get-AbsolutePath $root
$overlay = Join-Path $root 'amnesia\resources'
$output = Join-Path $root 'build-premake\amnesia'
$editorResources = Join-Path $root 'HPL2\tools\resources'
$mapDelta = Join-Path $root 'scripts\mapdelta.py'
$manifestName = '.redux_overlay_manifest'
$backupSuffix = '.mapdelta-orig'
$gameDir = $null
$config = 'all'
$resources = 'copy'
$copyGameAssets = $true

$scriptArgs = @($args)
$index = 0
while ($index -lt $scriptArgs.Count) {
    $argument = [string] $scriptArgs[$index]
    if ($argument -ieq '-GameDir' -or $argument -ieq '--game-dir') {
        if ($index + 1 -ge $scriptArgs.Count) { throw "$argument requires a path argument." }
        $gameDir = [string] $scriptArgs[$index + 1]
        $index += 2
    } elseif ($argument.StartsWith('--game-dir=')) {
        $gameDir = $argument.Substring('--game-dir='.Length)
        $index++
    } elseif ($argument -ieq '-Config' -or $argument -ieq '--config') {
        if ($index + 1 -ge $scriptArgs.Count) { throw "$argument requires a value." }
        $config = ([string] $scriptArgs[$index + 1]).ToLowerInvariant()
        $index += 2
    } elseif ($argument.StartsWith('--config=')) {
        $config = $argument.Substring('--config='.Length).ToLowerInvariant()
        $index++
    } elseif ($argument -ieq '-Resources' -or $argument -ieq '--resources') {
        if ($index + 1 -ge $scriptArgs.Count) { throw "$argument requires a value." }
        $resources = ([string] $scriptArgs[$index + 1]).ToLowerInvariant()
        $index += 2
    } elseif ($argument.StartsWith('--resources=')) {
        $resources = $argument.Substring('--resources='.Length).ToLowerInvariant()
        $index++
    } elseif ($argument -ieq '-NoGameAssets' -or $argument -ieq '--no-game-assets') {
        $copyGameAssets = $false
        $index++
    } elseif ($argument -ieq '-Output' -or $argument -ieq '--output') {
        if ($index + 1 -ge $scriptArgs.Count) { throw "$argument requires a path argument." }
        $output = [string] $scriptArgs[$index + 1]
        $index += 2
    } elseif ($argument.StartsWith('--output=')) {
        $output = $argument.Substring('--output='.Length)
        $index++
    } elseif ($argument -ieq '-Overlay' -or $argument -ieq '--overlay') {
        if ($index + 1 -ge $scriptArgs.Count) { throw "$argument requires a path argument." }
        $overlay = [string] $scriptArgs[$index + 1]
        $index += 2
    } elseif ($argument.StartsWith('--overlay=')) {
        $overlay = $argument.Substring('--overlay='.Length)
        $index++
    } elseif ($argument -ieq '-Help' -or $argument -ieq '--help' -or $argument -ieq '-h' -or $argument -ieq '/?') {
        Show-Usage
        exit 0
    } else {
        Show-Usage
        throw "Unknown argument: $argument"
    }
}

if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
    throw 'deploy.ps1 must be run on Windows.'
}
if ($config -notin @('release', 'debug', 'all')) {
    throw '--config must be release, debug, or all.'
}
if ($resources -notin @('copy', 'merge', 'none')) {
    throw '--resources must be copy, merge, or none.'
}

$overlay = Get-AbsolutePath $overlay
$output = Get-AbsolutePath $output
if (-not (Test-Path -LiteralPath $overlay -PathType Container)) {
    throw "Overlay directory not found: $overlay"
}
if ($resources -eq 'merge' -and -not (Test-Path -LiteralPath $mapDelta -PathType Leaf)) {
    throw "Map-delta tool not found: $mapDelta"
}

if ($copyGameAssets) {
    if (-not $gameDir -and $env:AMNESIA_GAME_DIRECTORY) { $gameDir = $env:AMNESIA_GAME_DIRECTORY }
    if (-not $gameDir -and $env:ATDD_DIR) { $gameDir = $env:ATDD_DIR }
    if (-not $gameDir) {
        $programFilesX86 = ${env:ProgramFiles(x86)}
        if (-not $programFilesX86) { $programFilesX86 = $env:ProgramFiles }
        $gameDir = Join-Path $programFilesX86 'Steam\steamapps\common\Amnesia The Dark Descent'
    }
    $gameDir = Get-AbsolutePath $gameDir
    if (-not (Test-Path -LiteralPath $gameDir -PathType Container)) {
        throw "Game directory not found: $gameDir (pass -GameDir or set AMNESIA_GAME_DIRECTORY/ATDD_DIR)."
    }
}

$configurations = switch ($config) {
    'release' { @('Release') }
    'debug' { @('Debug') }
    default { @('Debug', 'Release') }
}

$deployed = 0
foreach ($configuration in $configurations) {
    $destination = Join-Path $output $configuration
    if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
        if ($config -eq 'all') { continue }
        throw "$destination does not exist; build $configuration first."
    }

    if ($copyGameAssets) {
        Write-Host "==> Deploying game assets from $gameDir to $destination"
        Copy-GameAssets $gameDir $destination
    }
    if (Test-Path -LiteralPath $editorResources -PathType Container) {
        Write-Host "==> Deploying editor resources to $destination"
        Copy-DirectoryContents $editorResources $destination
    }
    Write-Host "==> Redux resources ($resources) -> $destination"
    Deploy-Resources $destination $overlay $resources $mapDelta $manifestName $backupSuffix
    $deployed++
}

if ($deployed -eq 0) {
    throw "No $output\<Config> directory exists; build first."
}
