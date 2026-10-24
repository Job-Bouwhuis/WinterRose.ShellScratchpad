param(
    [Parameter(Mandatory = $true)]
    [string]$QtRoot,
    [string]$BuildDirectory = "build/windows-release"
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = $PSScriptRoot
$BuildPath = Join-Path $ProjectRoot $BuildDirectory
$Executable = Join-Path $BuildPath 'shell-scratchpad-git.exe'
$Windeployqt = Join-Path $QtRoot 'bin/windeployqt.exe'

if (-not (Test-Path -LiteralPath $Windeployqt -PathType Leaf)) {
    throw "Could not find windeployqt.exe under Qt root: $QtRoot"
}

cmake -S $ProjectRoot -B $BuildPath -DCMAKE_PREFIX_PATH=$QtRoot -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $BuildPath --config Release --target shell-scratchpad-git
if ($LASTEXITCODE -ne 0) { throw 'The native Windows Git client build failed.' }
& $Windeployqt --release --no-translations $Executable
if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }

Write-Host "Packaged Git-only Windows app at: $BuildPath"
Write-Host 'Git for Windows must be installed and available on PATH. Git Credential Manager is recommended for HTTPS remotes.'
