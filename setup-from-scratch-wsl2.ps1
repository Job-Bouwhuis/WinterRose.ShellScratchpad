param([switch]$SkipElevation)

$ErrorActionPreference = 'Stop'
$NewDistroName = 'shell-scratchpad-host'
$UbuntuRootfsBase = 'https://cloud-images.ubuntu.com/wsl/releases/noble/current'

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Fail {
    param([Parameter(Mandatory = $true)][string]$Message)
    throw $Message
}

function ConvertTo-BashSingleQuoted {
    param([Parameter(Mandatory = $true)][string]$Value)
    $DoubleQuote = [string][char]34
    $Replacement = "'" + $DoubleQuote + "'" + $DoubleQuote + "'"
    return "'" + $Value.Replace("'", $Replacement) + "'"
}

function Get-WslDistroNames {
    $Output = @(& $WslExe --list --quiet 2>&1)
    if ($LASTEXITCODE -ne 0) {
        return @()
    }
    return @($Output | ForEach-Object { ("$_" -replace "`0", '').Trim() } |
        Where-Object { $_ -and $_ -notmatch '^Windows Subsystem for Linux' -and $_ -notmatch '^There are no installed distributions' })
}

$WslExe = Join-Path $env:WINDIR 'System32\wsl.exe'
if (-not (Test-Path -LiteralPath $WslExe)) {
    Fail 'wsl.exe is not available. Install a supported Windows version and enable hardware virtualization, then rerun this script.'
}

$ScriptPath = $PSCommandPath
if (-not $ScriptPath) {
    $ScriptPath = $MyInvocation.MyCommand.Path
}
$ScriptDirectory = if ($ScriptPath) { Split-Path -Parent $ScriptPath } else { (Get-Location).Path }
$SetupWslScript = Join-Path $ScriptDirectory 'setup-wsl2.sh'
$CloneSetupScript = Join-Path $ScriptDirectory 'clone-and-setup.sh'
foreach ($Path in @($SetupWslScript, $CloneSetupScript)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "Required companion script is missing: $Path. Keep setup-from-scratch-wsl2.ps1, setup-wsl2.sh, and clone-and-setup.sh together."
    }
}

$Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
$IsAdministrator = $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $IsAdministrator -and -not $SkipElevation) {
    Write-Host 'Windows needs administrator approval to install or update WSL2. A UAC prompt will appear.'
    $Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"")
    $Elevated = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $Arguments -Wait -PassThru
    exit $Elevated.ExitCode
}
if (-not $IsAdministrator) {
    Fail 'Run this script as Administrator so it can install and configure WSL2.'
}

try {
    Write-Step 'Ensuring WSL2 is installed and updated'
    $WslFeature = Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux
    $VmFeature = Get-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform
    if ($WslFeature.State -ne 'Enabled' -or $VmFeature.State -ne 'Enabled') {
        & $WslExe --install --no-distribution
        if ($LASTEXITCODE -ne 0) {
            Fail "WSL installation failed with exit code $LASTEXITCODE. If Windows requested a restart, restart and rerun this script."
        }
        $WslFeature = Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux
        $VmFeature = Get-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform
        if ($WslFeature.State -ne 'Enabled' -or $VmFeature.State -ne 'Enabled') {
            Fail 'Windows has marked WSL features for installation. Restart Windows, then rerun this script to continue.'
        }
    }
    & $WslExe --update
    if ($LASTEXITCODE -ne 0) {
        Fail "wsl --update failed with exit code $LASTEXITCODE. Check Windows Update/network access. If this was the first WSL installation, restart Windows and rerun this script."
    }
    & $WslExe --set-default-version 2
    if ($LASTEXITCODE -ne 0) {
        Fail "Could not set WSL2 as the default (exit code $LASTEXITCODE). Ensure virtualization is enabled and restart Windows if requested."
    }

    $Distros = @(Get-WslDistroNames)
    $Distro = $null
    if ($Distros.Count -gt 0) {
        Write-Host 'Choose the WSL distribution for Shell Scratchpad:'
        for ($Index = 0; $Index -lt $Distros.Count; $Index++) {
            Write-Host ("  {0}. {1}" -f ($Index + 1), $Distros[$Index])
        }
        if ($Distros -notcontains $NewDistroName) {
            Write-Host "  N. Create a new Ubuntu instance named $NewDistroName"
        }
        do {
            $Choice = (Read-Host 'Enter a distribution number, or N for a new Ubuntu instance').Trim()
            if ($Choice -match '^[Nn]$' -and $Distros -notcontains $NewDistroName) {
                $Distro = $NewDistroName
                break
            }
            if ($Choice -match '^\d+$' -and [int]$Choice -ge 1 -and [int]$Choice -le $Distros.Count) {
                $Distro = $Distros[[int]$Choice - 1]
                break
            }
            Write-Host 'That choice is not available. Select a listed number or N.' -ForegroundColor Yellow
        } while ($true)
    }
    else {
        Write-Host "No WSL distributions are installed. A new Ubuntu instance named $NewDistroName will be created."
        $Distro = $NewDistroName
    }

    $IsNewDistro = $Distro -eq $NewDistroName -and $Distros -notcontains $NewDistroName
    if ($IsNewDistro) {
        Write-Step 'Downloading the official Ubuntu 24.04 LTS WSL image'
        $Architecture = $env:PROCESSOR_ARCHITECTURE
        if ($env:PROCESSOR_ARCHITEW6432) {
            $Architecture = $env:PROCESSOR_ARCHITEW6432
        }
        $ImageArchitecture = if ($Architecture -match 'ARM64') { 'arm64' } else { 'amd64' }
        $ImageName = "ubuntu-noble-wsl-$ImageArchitecture-wsl.rootfs.tar.gz"
        $ImagePath = Join-Path $env:TEMP $ImageName
        $InstallDirectory = Join-Path $env:LOCALAPPDATA "WSL\$NewDistroName"
        if (Test-Path -LiteralPath $InstallDirectory) {
            Fail "The WSL install directory already exists: $InstallDirectory. Move it aside or choose another WSL distribution."
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $InstallDirectory) -Force | Out-Null
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri "$UbuntuRootfsBase/$ImageName" -OutFile $ImagePath
        try {
            Write-Step "Registering Ubuntu as the WSL2 instance $NewDistroName"
            & $WslExe --import $NewDistroName $InstallDirectory $ImagePath --version 2
            if ($LASTEXITCODE -ne 0) {
                Fail "Could not import the Ubuntu image as $NewDistroName (exit code $LASTEXITCODE). If WSL features were just enabled, restart Windows and rerun this script."
            }
        }
        finally {
            Remove-Item -LiteralPath $ImagePath -Force -ErrorAction SilentlyContinue
        }

        $DefaultLinuxUser = ($env:USERNAME -replace '[^A-Za-z0-9_-]', '').ToLowerInvariant()
        if (-not $DefaultLinuxUser -or $DefaultLinuxUser -notmatch '^[a-z_][a-z0-9_-]*$') {
            $DefaultLinuxUser = 'scratchpad'
        }
        $DefaultLinuxUser = (Read-Host "Linux username for $NewDistroName [$DefaultLinuxUser]").Trim().ToLowerInvariant()
        if (-not $DefaultLinuxUser) {
            $DefaultLinuxUser = ($env:USERNAME -replace '[^A-Za-z0-9_-]', '').ToLowerInvariant()
            if (-not $DefaultLinuxUser -or $DefaultLinuxUser -notmatch '^[a-z_][a-z0-9_-]*$') {
                $DefaultLinuxUser = 'scratchpad'
            }
        }
        if ($DefaultLinuxUser -notmatch '^[a-z_][a-z0-9_-]*$') {
            Fail 'Linux usernames must start with a lowercase letter or underscore and contain only lowercase letters, numbers, underscores, and hyphens.'
        }
        $UserQuoted = ConvertTo-BashSingleQuoted $DefaultLinuxUser
        $CreateUserCommand = @"
set -e
if ! id $UserQuoted >/dev/null 2>&1; then
    adduser --disabled-password --gecos '' $UserQuoted
fi
usermod -aG sudo $UserQuoted
if grep -q '^\[user\]' /etc/wsl.conf 2>/dev/null; then
    sed -i '/^\[user\]/,/^\[/{ /^default=/d; }' /etc/wsl.conf
    sed -i '/^\[user\]/a default=$DefaultLinuxUser' /etc/wsl.conf
else
    printf '\n[user]\ndefault=$DefaultLinuxUser\n' >> /etc/wsl.conf
fi
"@
        & $WslExe --distribution $Distro --user root --exec bash -lc $CreateUserCommand
        if ($LASTEXITCODE -ne 0) {
            Fail "Could not configure Linux user $DefaultLinuxUser in $Distro (exit code $LASTEXITCODE)."
        }
        Write-Host "Set a password for Linux user $DefaultLinuxUser when prompted. This password is used by sudo during installation."
        & $WslExe --distribution $Distro --user root --exec passwd $DefaultLinuxUser
        if ($LASTEXITCODE -ne 0) {
            Fail "Could not set the Linux password for $DefaultLinuxUser. Rerun and set it with: wsl --distribution $Distro --user root --exec passwd $DefaultLinuxUser"
        }
    }

    Write-Step "Preparing $Distro as a WSL2 instance"
    & $WslExe --set-version $Distro 2
    if ($LASTEXITCODE -ne 0) {
        Fail "Could not convert $Distro to WSL2. Check virtualization support and retry."
    }

    Write-Step 'Copying the WSL setup scripts into the selected distribution'
    $SetupScriptWsl = (& $WslExe --distribution $Distro --exec wslpath -u $SetupWslScript 2>$null | Out-String).Trim()
    $CloneScriptWsl = (& $WslExe --distribution $Distro --exec wslpath -u $CloneSetupScript 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $SetupScriptWsl -or -not $CloneScriptWsl) {
        Fail 'Could not access the setup scripts from WSL. Ensure Windows drives are mounted in WSL and rerun this script.'
    }
    $SetupScriptQuoted = ConvertTo-BashSingleQuoted $SetupScriptWsl
    $CloneScriptQuoted = ConvertTo-BashSingleQuoted $CloneScriptWsl
    $LinuxCommand = @"
set -e
target="`$HOME/.local/share/shell-scratchpad-bootstrap"
mkdir -p "`$target"
cp $SetupScriptQuoted "`$target/setup-wsl2.sh"
cp $CloneScriptQuoted "`$target/clone-and-setup.sh"
sed -i 's/\r$//' "`$target/setup-wsl2.sh" "`$target/clone-and-setup.sh"
chmod +x "`$target/setup-wsl2.sh" "`$target/clone-and-setup.sh"
cd "`$target"
./setup-wsl2.sh
"@
    & $WslExe --distribution $Distro --exec bash -lc $LinuxCommand
    if ($LASTEXITCODE -ne 0) {
        Fail "The setup failed with exit code $LASTEXITCODE. The scripts remain in the selected distro at ~/.local/share/shell-scratchpad-bootstrap; rerun setup-wsl2.sh there after resolving the error."
    }

    Write-Step 'Setup complete'
    Write-Host "Shell Scratchpad is installed in the WSL2 distribution '$Distro'. Windows Desktop and Start menu shortcuts were created."
}
catch {
    Write-Error $_
    exit 1
}
