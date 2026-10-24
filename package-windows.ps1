param(
    [string]$QtRoot,
    [string]$BuildDirectory = 'build/windows-release',
    [string]$QtVersion = '6.12.0'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = $PSScriptRoot
$BuildPath = Join-Path $ProjectRoot $BuildDirectory
$QtArchitecture = 'win64_msvc2022_64'
$QtSdkBase = Join-Path $env:LOCALAPPDATA 'Programs/QtSdk'
if (-not $QtRoot) {
    $QtRoot = Join-Path $QtSdkBase "$QtVersion/$QtArchitecture"
}
$Executable = Join-Path $BuildPath 'shell-scratchpad-git.exe'
$Windeployqt = Join-Path $QtRoot 'bin/windeployqt.exe'

function Refresh-ProcessPath {
    $MachinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = @($env:Path, $MachinePath, $UserPath) -join [IO.Path]::PathSeparator
}

function Install-WingetPackage([string]$PackageId) {
    $Winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $Winget) {
        throw "winget is required to install $PackageId automatically. Install the Windows App Installer, or install the prerequisite manually and rerun this script."
    }
    Write-Host "Installing $PackageId with Windows Package Manager..."
    & $Winget.Source install --exact --id $PackageId --source winget
    if ($LASTEXITCODE -ne 0) { throw "winget failed to install $PackageId (exit code $LASTEXITCODE)." }
    Refresh-ProcessPath
}

function Find-GitCommand {
    $Git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($Git) { return $Git.Source }
    $Candidates = @(
        (Join-Path $env:ProgramFiles 'Git/cmd/git.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs/Git/cmd/git.exe')
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) { return $Candidate }
    }
    return $null
}

$GitCommand = Find-GitCommand
if (-not $GitCommand) {
    Install-WingetPackage 'Git.Git'
    $GitCommand = Find-GitCommand
    if (-not $GitCommand) { throw 'Git for Windows installed, but git.exe was not found. Restart PowerShell and rerun this script.' }
}
$GitCommandDirectory = Split-Path -Parent $GitCommand
if (($env:Path -split [IO.Path]::PathSeparator) -notcontains $GitCommandDirectory) {
    $env:Path = "$GitCommandDirectory$([IO.Path]::PathSeparator)$env:Path"
}

if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    Install-WingetPackage 'Kitware.CMake'
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) {
    throw 'CMake is not available on PATH after installation. Restart PowerShell and rerun this script.'
}

if (-not (Test-Path -LiteralPath $Windeployqt -PathType Leaf)) {
    if ($QtRoot) {
        # If the caller supplied a missing root, do not silently install to a different location.
        if ($PSBoundParameters.ContainsKey('QtRoot')) {
            throw "Qt was not found at the specified QtRoot: $QtRoot"
        }
    }

    $PythonLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
    $PythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($PythonLauncher) {
        $PythonExecutable = $PythonLauncher.Source
        $PythonPrefixArguments = @('-3')
    } elseif ($PythonCommand) {
        $PythonExecutable = $PythonCommand.Source
        $PythonPrefixArguments = @()
    } else {
        Install-WingetPackage 'Python.Python.3.12'
        $PythonLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
        $PythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($PythonLauncher) {
            $PythonExecutable = $PythonLauncher.Source
            $PythonPrefixArguments = @('-3')
        } elseif ($PythonCommand) {
            $PythonExecutable = $PythonCommand.Source
            $PythonPrefixArguments = @()
        } else {
            throw 'Python installed, but its launcher was not found. Restart PowerShell and rerun this script.'
        }
    }

    Write-Host "Installing Qt $QtVersion for MSVC 2022. This may take several minutes."
    & $PythonExecutable @PythonPrefixArguments -m pip install --user --upgrade aqtinstall
    if ($LASTEXITCODE -ne 0) { throw 'Could not install aqtinstall with pip.' }
    & $PythonExecutable @PythonPrefixArguments -m aqt install-qt windows desktop $QtVersion $QtArchitecture --outputdir $QtSdkBase
    if ($LASTEXITCODE -ne 0) { throw "Could not install Qt $QtVersion for $QtArchitecture." }
    if (-not (Test-Path -LiteralPath $Windeployqt -PathType Leaf)) {
        throw "Qt installation completed, but windeployqt.exe was not found at $Windeployqt"
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'The MSVC compiler is not available. Install Visual Studio 2022 or Build Tools with the C++ workload, then run this script from a Visual Studio Developer PowerShell.'
}

cmake -S $ProjectRoot -B $BuildPath "-DCMAKE_PREFIX_PATH=$QtRoot" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $BuildPath --config Release --target shell-scratchpad-git
if ($LASTEXITCODE -ne 0) { throw 'The native Windows Git client build failed.' }
& $Windeployqt --release --no-translations $Executable
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }

Write-Host "Packaged Git-only Windows app at: $BuildPath"
Write-Host "Git: $(& $GitCommand --version)"
Write-Host 'Git Credential Manager is recommended for HTTPS remotes.'
