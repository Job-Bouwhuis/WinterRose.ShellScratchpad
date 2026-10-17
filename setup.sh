#!/usr/bin/env bash
set -Eeuo pipefail

APP_NAME="Shell Scratchpad"
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
INSTALL_PREFIX="${HOME}/.local"
INSTALL_APP=1

case "${1:-}" in
    "") ;;
    --build-only) INSTALL_APP=0 ;;
    *)
        printf 'Usage: %s [--build-only]\n' "$0" >&2
        exit 2
        ;;
esac

say() {
    printf '\n==> %s\n' "$*"
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

if [[ ! -f "${PROJECT_DIR}/CMakeLists.txt" || ! -f "${PROJECT_DIR}/src/main.cpp" ]]; then
    fail "Run setup.sh from the extracted Shell Scratchpad project directory."
fi

if [[ "${EUID}" -eq 0 ]]; then
    RUN_AS_ROOT=()
elif command -v sudo >/dev/null 2>&1; then
    RUN_AS_ROOT=(sudo)
else
    fail "Installing prerequisites needs root privileges. Install sudo or run this script as root (the app itself will still be installed to the invoking user's home only when run as that user)."
fi

install_prerequisites() {
    if [[ ! -r /etc/os-release ]]; then
        fail "Cannot identify this Linux distribution: /etc/os-release is missing."
    fi

    # shellcheck disable=SC1091
    source /etc/os-release
    local distro_id="${ID:-unknown}"
    local distro_like="${ID_LIKE:-}"
    distro_id="${distro_id,,}"
    distro_like="${distro_like,,}"

    case "${distro_id}" in
        fedora)
            say "Installing build prerequisites with DNF"
            "${RUN_AS_ROOT[@]}" dnf install -y gcc-c++ cmake qt6-qtbase-devel
            ;;
        ubuntu|debian|linuxmint|pop)
            say "Installing build prerequisites with APT"
            "${RUN_AS_ROOT[@]}" apt-get update
            "${RUN_AS_ROOT[@]}" apt-get install -y build-essential cmake qt6-base-dev
            ;;
        opensuse-tumbleweed|opensuse-leap|sles)
            say "Installing build prerequisites with Zypper"
            "${RUN_AS_ROOT[@]}" zypper --non-interactive install gcc-c++ cmake qt6-base-devel
            ;;
        arch|manjaro|endeavouros)
            say "Installing build prerequisites with Pacman"
            "${RUN_AS_ROOT[@]}" pacman -S --needed --noconfirm base-devel cmake qt6-base
            ;;
        *)
            if [[ "${distro_like}" == *fedora* ]] && command -v dnf >/dev/null 2>&1; then
                say "Installing build prerequisites with DNF"
                "${RUN_AS_ROOT[@]}" dnf install -y gcc-c++ cmake qt6-qtbase-devel
            elif [[ "${distro_like}" == *debian* ]] && command -v apt-get >/dev/null 2>&1; then
                say "Installing build prerequisites with APT"
                "${RUN_AS_ROOT[@]}" apt-get update
                "${RUN_AS_ROOT[@]}" apt-get install -y build-essential cmake qt6-base-dev
            else
                fail "Unsupported distribution '${ID:-unknown}'. Install a C++ compiler, CMake, and Qt 6 Widgets development files, then run this script again after adapting the package-install section."
            fi
            ;;
    esac
}

say "Checking and installing build prerequisites"
install_prerequisites

for command_name in cmake c++; do
    command -v "${command_name}" >/dev/null 2>&1 || fail "Required command '${command_name}' is still unavailable after installing prerequisites."
done

say "Configuring ${APP_NAME}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}"

say "Building ${APP_NAME}"
if command -v nproc >/dev/null 2>&1; then
    BUILD_JOBS="$(nproc)"
else
    BUILD_JOBS=2
fi
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

say "Running syntax tokenizer tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

if (( ! INSTALL_APP )); then
    say "Build and tests complete; existing installation was left untouched"
    printf 'Run this development build with: "%s"\n' "${BUILD_DIR}/shell-scratchpad"
    printf 'Future runs can reuse the build with: "cmake --build %s"\n' "${BUILD_DIR}"
    exit 0
fi

say "Installing ${APP_NAME} for the current user into ${INSTALL_PREFIX}"
cmake --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"

APP_EXECUTABLE="${INSTALL_PREFIX}/bin/shell-scratchpad"
DESKTOP_FILE="${INSTALL_PREFIX}/share/applications/org.winterrose.shellscratchpad.desktop"
SERVICE_MENU_FILE="${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-servicemenu.desktop"

if [[ -x "${APP_EXECUTABLE}" && -f "${DESKTOP_FILE}" ]]; then
    # Desktop Entry files do not expand shell variables. Store the concrete path
    # so the launcher works even when ~/.local/bin is not on the desktop session's PATH.
    DESKTOP_TEMP="$(mktemp)"
    awk -v exec_path="${APP_EXECUTABLE}" '
        BEGIN {
            gsub(/\\/, "\\\\", exec_path)
            gsub(/"/, "\\\"", exec_path)
        }
        /^Exec=/ { print "Exec=\"" exec_path "\""; next }
        { print }
    ' "${DESKTOP_FILE}" > "${DESKTOP_TEMP}"
    install -m 0644 "${DESKTOP_TEMP}" "${DESKTOP_FILE}"
    rm -f -- "${DESKTOP_TEMP}"
fi

if [[ -x "${APP_EXECUTABLE}" && -f "${SERVICE_MENU_FILE}" ]]; then
    # KDE Plasma 6 authorizes user-installed servicemenus when the .desktop file
    # is executable. Use an absolute executable path because the desktop session
    # may not include ~/.local/bin in PATH. %f is supplied by KIO as the selected
    # local directory path and is forwarded to --working-directory.
    SERVICE_MENU_TEMP="$(mktemp)"
    awk -v exec_path="${APP_EXECUTABLE}" '
        BEGIN {
            gsub(/\\/, "\\\\", exec_path)
            gsub(/"/, "\\\"", exec_path)
        }
        /^Exec=shell-scratchpad --working-directory %f$/ {
            print "Exec=\"" exec_path "\" --working-directory %f"
            next
        }
        { print }
    ' "${SERVICE_MENU_FILE}" > "${SERVICE_MENU_TEMP}"
    install -m 0755 "${SERVICE_MENU_TEMP}" "${SERVICE_MENU_FILE}"
    rm -f -- "${SERVICE_MENU_TEMP}"
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${INSTALL_PREFIX}/share/applications" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

say "Setup complete"
printf '%s is built and installed.\n' "${APP_NAME}"
printf 'Run it now with: "%s"\n' "${APP_EXECUTABLE}"
printf 'It should also appear in the KDE application launcher as: %s\n' "${APP_NAME}"
printf 'Dolphin context menu: right-click a directory and choose "Open Shell Scratchpad".\n'
printf 'The working directory will be pre-filled with the selected directory.\n'
printf '\nBuild directory: %s\n' "${BUILD_DIR}"
printf 'Install prefix:  %s\n' "${INSTALL_PREFIX}"
printf '\nNote: Shell scripts run with your normal user permissions. Review commands before running them.\n'
