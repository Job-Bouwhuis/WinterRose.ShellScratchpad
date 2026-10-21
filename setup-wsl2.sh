#!/usr/bin/env bash
set -Eeuo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CLONE_SETUP_SCRIPT="${PROJECT_DIR}/clone-and-setup.sh"
PROJECT_CHECKOUT="${HOME}/ShellScratchpad"
INSTALL_PREFIX="${HOME}/.local"
APP_LAUNCHER="${INSTALL_PREFIX}/bin/shell-scratchpad-launcher"
WSL_LAUNCHER="${INSTALL_PREFIX}/bin/shell-scratchpad-wsl"
WINDOWS_APP_DIR=""
WINDOWS_DESKTOP=""
WINDOWS_START_MENU=""
UNINSTALL=0

say() {
    printf '\n==> %s\n' "$*"
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'Usage: %s [--uninstall]\n' "$0"
}

for argument in "$@"; do
    case "${argument}" in
        --uninstall) UNINSTALL=1 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[[ -f "${CLONE_SETUP_SCRIPT}" ]] || fail "Keep clone-and-setup.sh next to setup-wsl2.sh."
[[ "${EUID}" -ne 0 ]] || fail "Run this script as your regular WSL user, not as root."
[[ -n "${WSL_DISTRO_NAME:-}" ]] || fail "This script must run inside a WSL distribution."
grep -qi 'microsoft.*wsl2\|wsl2.*microsoft' /proc/sys/kernel/osrelease \
    || fail "This script requires WSL 2."

command -v wsl.exe >/dev/null 2>&1 || fail "Windows interop is unavailable; make sure WSL interop is enabled."
command -v powershell.exe >/dev/null 2>&1 || fail "powershell.exe is unavailable through WSL interop."
command -v wslpath >/dev/null 2>&1 || fail "wslpath is unavailable."

windows_special_folder() {
    local folder_name="$1"
    local result
    result="$(powershell.exe -NoLogo -NoProfile -NonInteractive -Command \
        "[Environment]::GetFolderPath('${folder_name}')" 2>/dev/null | tr -d '\r')" \
        || fail "Could not locate the Windows ${folder_name} folder through PowerShell."
    [[ -n "${result}" ]] || fail "Windows returned an empty path for its ${folder_name} folder."
    printf '%s' "${result}"
}

WINDOWS_LOCAL_APP_DATA="$(windows_special_folder LocalApplicationData)"
WINDOWS_DESKTOP="$(windows_special_folder Desktop)"
WINDOWS_START_MENU="$(windows_special_folder StartMenu)"
WINDOWS_APP_DIR="$(wslpath -u "${WINDOWS_LOCAL_APP_DATA}")/Programs/Shell Scratchpad"

remove_windows_launchers() {
    local windows_app_dir="$1"
    local windows_desktop="$2"
    local windows_start_menu="$3"
    rm -f -- "${windows_desktop}/Shell Scratchpad.lnk" \
        "${windows_start_menu}/Programs/Shell Scratchpad.lnk"
    rm -rf -- "${windows_start_menu}/Programs/Shell Scratchpad"
    rm -rf -- "${windows_app_dir}"
}

if (( UNINSTALL )); then
    say "Removing Windows launchers and uninstalling Shell Scratchpad"
    if [[ -n "${WINDOWS_APP_DIR}" ]]; then
        remove_windows_launchers "${WINDOWS_APP_DIR}" "${WINDOWS_DESKTOP}" "${WINDOWS_START_MENU}"
    fi
    if [[ -x "${PROJECT_CHECKOUT}/setup.sh" ]]; then
        "${PROJECT_CHECKOUT}/setup.sh" --uninstall
    fi
    rm -f -- "${WSL_LAUNCHER}"
    say "WSL integration removed"
    exit 0
fi

if [[ -z "${WAYLAND_DISPLAY:-}" && -z "${DISPLAY:-}" ]]; then
    fail "No WSLg or X11 display is available. Update WSL from Windows with 'wsl --update', enable WSLg, restart this distribution, and rerun setup-wsl2.sh."
fi

say "Cloning the project and installing Shell Scratchpad in WSL"
bash "${CLONE_SETUP_SCRIPT}"
[[ -x "${APP_LAUNCHER}" ]] || fail "The normal setup completed without creating ${APP_LAUNCHER}."

install_xcb_runtime_dependency() {
    local os_id=""
    local id_like=""
    local run_as_root=()
    [[ -r /etc/os-release ]] || fail "Cannot identify this WSL distribution: /etc/os-release is missing."
    # shellcheck disable=SC1091
    source /etc/os-release
    os_id="${ID:-unknown}"
    os_id="${os_id,,}"
    id_like="${ID_LIKE:-}"
    id_like="${id_like,,}"
    if command -v sudo >/dev/null 2>&1; then
        run_as_root=(sudo)
    elif [[ "${EUID}" -ne 0 ]]; then
        fail "Installing the Qt X11 runtime dependency needs sudo. Install libxcb-cursor manually, then rerun this script."
    fi

    case "${os_id}" in
        ubuntu|debian|linuxmint|pop)
            "${run_as_root[@]}" apt-get update
            "${run_as_root[@]}" apt-get install -y libxcb-cursor0
            ;;
        fedora)
            "${run_as_root[@]}" dnf install -y xcb-util-cursor
            ;;
        arch|manjaro|endeavouros)
            "${run_as_root[@]}" pacman -S --needed --noconfirm xcb-util-cursor
            ;;
        opensuse-tumbleweed|opensuse-leap|sles)
            "${run_as_root[@]}" zypper --non-interactive install libxcb-cursor0
            ;;
        *)
            if [[ "${id_like}" == *debian* ]]; then
                "${run_as_root[@]}" apt-get update
                "${run_as_root[@]}" apt-get install -y libxcb-cursor0
            elif [[ "${id_like}" == *fedora* ]]; then
                "${run_as_root[@]}" dnf install -y xcb-util-cursor
            else
                fail "Unsupported WSL distribution '${ID:-unknown}'. Install the xcb-cursor runtime package with its package manager, then rerun this script."
            fi
            ;;
    esac
}

say "Ensuring Qt's WSLg X11 runtime dependency is installed"
install_xcb_runtime_dependency

say "Creating a Linux launcher that accepts Windows file paths"
mkdir -p -- "${INSTALL_PREFIX}/bin"
APP_LAUNCHER_QUOTED="$(printf '%q' "${APP_LAUNCHER}")"
cat > "${WSL_LAUNCHER}" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail
APP_LAUNCHER=${APP_LAUNCHER_QUOTED}
arguments=()
for argument in "\$@"; do
    case "\$argument" in
        --working-directory=*|--open-folder=*)
            option="\${argument%%=*}"
            path="\${argument#*=}"
            if [[ "\$path" =~ ^[A-Za-z]: ]] || [[ "\${path:0:2}" == '\\\\' ]]; then
                path="\$(wslpath -u "\$path")"
            fi
            arguments+=("\$option=\$path")
            ;;
        *)
            if [[ "\$argument" =~ ^[A-Za-z]: ]] || [[ "\${argument:0:2}" == '\\\\' ]]; then
                argument="\$(wslpath -u "\$argument")"
            fi
            arguments+=("\$argument")
            ;;
    esac
done

# WSLg exposes XWayland and usually provides DISPLAY. Qt's xcb backend works
# across WSLg versions; use Wayland directly only when X11 is unavailable.
if [[ -z "\${QT_QPA_PLATFORM:-}" ]]; then
    if [[ -n "\${DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=xcb
    elif [[ -n "\${WAYLAND_DISPLAY:-}" ]]; then
        export QT_QPA_PLATFORM=wayland
    fi
fi
exec "\$APP_LAUNCHER" "\${arguments[@]}"
EOF
chmod 0755 "${WSL_LAUNCHER}"

say "Creating the Windows launcher, Desktop shortcut, and Start menu entry"
mkdir -p -- "${WINDOWS_APP_DIR}"
printf '%s\n' "${WSL_DISTRO_NAME}" > "${WINDOWS_APP_DIR}/wsl-distro.txt"
printf '%s\n' "${WSL_LAUNCHER}" > "${WINDOWS_APP_DIR}/linux-launcher.txt"
cat > "${WINDOWS_APP_DIR}/shell-scratchpad.ps1" <<'POWERSHELL'
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$AppArgs)
$ErrorActionPreference = 'Stop'
$Distro = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'wsl-distro.txt') -Raw).Trim()
$LinuxLauncher = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'linux-launcher.txt') -Raw).Trim()
$WslExe = Join-Path $env:WINDIR 'System32\wsl.exe'
& $WslExe --distribution $Distro --exec $LinuxLauncher @AppArgs
exit $LASTEXITCODE
POWERSHELL
cat > "${WINDOWS_APP_DIR}/shell-scratchpad.cmd" <<'CMD'
@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0shell-scratchpad.ps1" %*
exit /b %ERRORLEVEL%
CMD
cat > "${WINDOWS_APP_DIR}/create-shortcuts.ps1" <<'POWERSHELL'
$ErrorActionPreference = 'Stop'
$Launcher = Join-Path $PSScriptRoot 'shell-scratchpad.cmd'
$Shell = New-Object -ComObject WScript.Shell
$DesktopShortcut = $Shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Shell Scratchpad.lnk'))
$DesktopShortcut.TargetPath = $Launcher
$DesktopShortcut.WorkingDirectory = $env:USERPROFILE
$DesktopShortcut.IconLocation = (Join-Path $env:WINDIR 'System32\wsl.exe') + ',0'
$DesktopShortcut.Description = 'Run Shell Scratchpad inside WSL'
$DesktopShortcut.Save()
$StartMenuDir = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\Shell Scratchpad'
New-Item -ItemType Directory -Path $StartMenuDir -Force | Out-Null
$StartMenuShortcut = $Shell.CreateShortcut((Join-Path $StartMenuDir 'Shell Scratchpad.lnk'))
$StartMenuShortcut.TargetPath = $Launcher
$StartMenuShortcut.WorkingDirectory = $env:USERPROFILE
$StartMenuShortcut.IconLocation = (Join-Path $env:WINDIR 'System32\wsl.exe') + ',0'
$StartMenuShortcut.Description = 'Run Shell Scratchpad inside WSL'
$StartMenuShortcut.Save()
POWERSHELL
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -File "${WINDOWS_APP_DIR}/create-shortcuts.ps1" \
    || fail "Could not create Windows shortcuts. The Windows launcher remains at ${WINDOWS_APP_DIR}/shell-scratchpad.cmd."
rm -f -- "${WINDOWS_APP_DIR}/create-shortcuts.ps1"

say "WSL2 setup complete"
printf 'Launch from Windows Desktop or Start menu: Shell Scratchpad\n'
printf 'Windows command launcher: %s\\shell-scratchpad.cmd\n' "${WINDOWS_APP_DIR}"
printf 'WSL command launcher: %s\n' "${WSL_LAUNCHER}"
printf 'The application and its scripts continue to run inside WSL distribution: %s\n' "${WSL_DISTRO_NAME}"
