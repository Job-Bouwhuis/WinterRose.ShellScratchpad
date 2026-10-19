#!/usr/bin/env bash
set -Eeuo pipefail

APP_NAME="Shell Scratchpad"
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
INSTALL_PREFIX="${HOME}/.local"
INSTALL_APP=1
IN_DISTROBOX=0
UNINSTALL_APP=0
QT_MIN_VERSION=6.12.0
QT_PREFIX=""

for argument in "$@"; do
    case "${argument}" in
        --build-only) INSTALL_APP=0 ;;
        --in-distrobox) IN_DISTROBOX=1 ;;
        --uninstall) UNINSTALL_APP=1 ;;
        *)
            printf 'Usage: %s [--build-only] [--in-distrobox] [--uninstall]\n' "$0" >&2
            exit 2
            ;;
    esac
done

say() {
    printf '\n==> %s\n' "$*"
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

uninstall_app() {
    say "Uninstalling ${APP_NAME} from ${INSTALL_PREFIX}"
    rm -f -- \
        "${INSTALL_PREFIX}/bin/shell-scratchpad" \
        "${INSTALL_PREFIX}/bin/shell-scratchpad-launcher" \
        "${INSTALL_PREFIX}/bin/shell-scratchpad-steamos" \
        "${INSTALL_PREFIX}/share/applications/org.winterrose.shellscratchpad.desktop" \
        "${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-servicemenu.desktop" \
        "${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-script-servicemenu.desktop" \
        "${INSTALL_PREFIX}/share/icons/hicolor/scalable/apps/shell-scratchpad.svg"
    if command -v update-desktop-database >/dev/null 2>&1 \
        && [[ -d "${INSTALL_PREFIX}/share/applications" ]]; then
        update-desktop-database "${INSTALL_PREFIX}/share/applications" >/dev/null 2>&1 || true
    fi
    rmdir --ignore-fail-on-non-empty \
        "${INSTALL_PREFIX}/share/applications" \
        "${INSTALL_PREFIX}/share/kio/servicemenus" \
        "${INSTALL_PREFIX}/share/kio" \
        "${INSTALL_PREFIX}/share/icons/hicolor/scalable/apps" \
        "${INSTALL_PREFIX}/share/icons/hicolor/scalable" \
        "${INSTALL_PREFIX}/share/icons/hicolor" \
        "${INSTALL_PREFIX}/bin" 2>/dev/null || true
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    fi
    say "Uninstall complete"
    printf 'Removed the application, launcher, icon, and Dolphin menu entries.\n'
    printf 'Build files, downloaded Qt tools, and any Distrobox container were left intact.\n'
}

if [[ ! -f "${PROJECT_DIR}/CMakeLists.txt" || ! -f "${PROJECT_DIR}/src/main.cpp" ]]; then
    fail "Run setup.sh from the extracted Shell Scratchpad project directory."
fi

if (( UNINSTALL_APP )); then
    # SteamOS stores the executable in the Distrobox runtime, while the desktop
    # launcher and service menus live in the shared home directory.
    if [[ "${IN_DISTROBOX}" -eq 0 && -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        source /etc/os-release
        if [[ "${ID:-}" == steamos ]] && command -v distrobox >/dev/null 2>&1 \
            && distrobox list 2>/dev/null | grep -q 'shell-scratchpad'; then
            distrobox enter --name shell-scratchpad -- bash -lc \
                "'${PROJECT_DIR}/setup.sh' --in-distrobox --uninstall" || true
        fi
    fi
    uninstall_app
    exit 0
fi

# SteamOS keeps its root filesystem read-only. Build in a user-owned Distrobox
# and invoke the installed binary through that container's runtime libraries.
if [[ "${IN_DISTROBOX}" -eq 0 && -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    source /etc/os-release
    if [[ "${ID:-}" == steamos ]]; then
        DISTROBOX_NAME="shell-scratchpad"
        command -v distrobox >/dev/null 2>&1 || fail "SteamOS requires Distrobox. Install/enable Distrobox from the SteamOS image or use a supported Linux distribution. The host is left unchanged."
        if ! command -v podman >/dev/null 2>&1 && ! command -v docker >/dev/null 2>&1; then
            fail "SteamOS requires Podman or Docker for Distrobox. Install/enable the container engine, then rerun setup.sh."
        fi
        if ! distrobox list 2>/dev/null | grep -q "${DISTROBOX_NAME}"; then
            say "Creating Fedora Distrobox for Qt 6.12 build and runtime dependencies"
            distrobox create --yes --name "${DISTROBOX_NAME}" --image fedora:latest
        fi
        say "Building inside Distrobox"
        if (( INSTALL_APP )); then
            distrobox enter --name "${DISTROBOX_NAME}" -- bash -lc "'${PROJECT_DIR}/setup.sh' --in-distrobox"
        else
            distrobox enter --name "${DISTROBOX_NAME}" -- bash -lc "'${PROJECT_DIR}/setup.sh' --in-distrobox --build-only"
            exit 0
        fi

        INSTALL_PREFIX="${HOME}/.local"
        APP_EXECUTABLE="${INSTALL_PREFIX}/bin/shell-scratchpad"
        RUNNER="${INSTALL_PREFIX}/bin/shell-scratchpad-steamos"
        mkdir -p "${INSTALL_PREFIX}/bin"
        cat > "${RUNNER}" <<EOF
#!/usr/bin/env bash
exec distrobox enter --name '${DISTROBOX_NAME}' -- '${APP_EXECUTABLE}' "\$@"
EOF
        chmod 0755 "${RUNNER}"
        # Point desktop and Dolphin actions at the host-side Distrobox wrapper.
        for desktop_file in "${INSTALL_PREFIX}/share/applications/org.winterrose.shellscratchpad.desktop" \
            "${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-servicemenu.desktop" \
            "${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-script-servicemenu.desktop"; do
            if [[ -f "${desktop_file}" ]]; then
                sed -i "s|${APP_EXECUTABLE}|${RUNNER}|g; s|${INSTALL_PREFIX}/bin/shell-scratchpad-launcher|${RUNNER}|g; s|Exec=shell-scratchpad |Exec=\"${RUNNER}\" |g" "${desktop_file}"
            fi
        done
        say "SteamOS setup complete"
        printf 'Run the app with: "%s"\n' "${RUNNER}"
        printf 'The build and Qt runtime are provided by the %s Distrobox.\n' "${DISTROBOX_NAME}"
        exit 0
    fi
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
            "${RUN_AS_ROOT[@]}" dnf install -y gcc-c++ cmake qt6-qtbase-devel ShellCheck
            ;;
        ubuntu|debian|linuxmint|pop)
            say "Installing build prerequisites with APT"
            "${RUN_AS_ROOT[@]}" apt-get update
            "${RUN_AS_ROOT[@]}" apt-get install -y build-essential cmake qt6-base-dev shellcheck
            ;;
        opensuse-tumbleweed|opensuse-leap|sles)
            say "Installing build prerequisites with Zypper"
            "${RUN_AS_ROOT[@]}" zypper --non-interactive install gcc-c++ cmake qt6-base-devel ShellCheck
            ;;
        arch|manjaro|endeavouros)
            say "Installing build prerequisites with Pacman"
            "${RUN_AS_ROOT[@]}" pacman -S --needed --noconfirm base-devel cmake qt6-base shellcheck
            ;;
        *)
            if [[ "${distro_like}" == *fedora* ]] && command -v dnf >/dev/null 2>&1; then
                say "Installing build prerequisites with DNF"
                "${RUN_AS_ROOT[@]}" dnf install -y gcc-c++ cmake qt6-qtbase-devel ShellCheck
            elif [[ "${distro_like}" == *debian* ]] && command -v apt-get >/dev/null 2>&1; then
                say "Installing build prerequisites with APT"
                "${RUN_AS_ROOT[@]}" apt-get update
                "${RUN_AS_ROOT[@]}" apt-get install -y build-essential cmake qt6-base-dev shellcheck
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

QT_TOOLS_DIR="${HOME}/.local/share/shell-scratchpad/qt-tools"
QT_INSTALL_ROOT="${HOME}/.local/opt/Qt"
QT_VERSION="$(qmake6 -query QT_VERSION 2>/dev/null || qtpaths6 --qt-version 2>/dev/null || true)"
QT6_CONFIG_DIR=""
QT_PREFIX=""

if [[ -n "${QT_VERSION}" && "$(printf '%s\n' "${QT_MIN_VERSION}" "${QT_VERSION}" | sort -V | head -n1)" == "${QT_MIN_VERSION}" ]]; then
    QT_PREFIX="$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null || true)"
fi

# Reuse a Qt SDK installed by an earlier setup run before downloading it again.
if [[ -z "${QT_PREFIX}" && -d "${QT_INSTALL_ROOT}" ]]; then
    while IFS= read -r config_file; do
        config_dir="$(dirname -- "${config_file}")"
        candidate_prefix="${config_file%/lib/cmake/Qt6/Qt6Config.cmake}"
        candidate_prefix="${candidate_prefix%/lib64/cmake/Qt6/Qt6Config.cmake}"
        candidate_qmake="${candidate_prefix}/bin/qmake"
        [[ -x "${candidate_qmake}" ]] || candidate_qmake="${candidate_prefix}/bin/qmake6"
        [[ -x "${candidate_qmake}" ]] || continue
        candidate_version="$("${candidate_qmake}" -query QT_VERSION 2>/dev/null || true)"
        if [[ -n "${candidate_version}" && "$(printf '%s\n' "${QT_MIN_VERSION}" "${candidate_version}" | sort -V | head -n1)" == "${QT_MIN_VERSION}" ]]; then
            QT_VERSION="${candidate_version}"
            QT_PREFIX="${candidate_prefix}"
            QT6_CONFIG_DIR="${config_dir}"
            break
        fi
    done < <(find "${QT_INSTALL_ROOT}" -type f \( -path '*/lib/cmake/Qt6/Qt6Config.cmake' -o -path '*/lib64/cmake/Qt6/Qt6Config.cmake' \) -print 2>/dev/null)
fi

if [[ -z "${QT_PREFIX}" ]]; then
    say "Qt ${QT_VERSION:-not found} is older than ${QT_MIN_VERSION}; installing Qt ${QT_MIN_VERSION} in the user profile"
    command -v python3 >/dev/null 2>&1 || fail "Python 3 is required to install Qt ${QT_MIN_VERSION} locally."
    python3 -m venv "${QT_TOOLS_DIR}/venv"
    # Qt's 6.12 repository metadata is newer than the PyPI aqtinstall release.
    # Install upstream main, which carries the metadata parser updates.
    "${QT_TOOLS_DIR}/venv/bin/python" -m pip install --disable-pip-version-check --upgrade "git+https://github.com/miurahr/aqtinstall.git"
    "${QT_TOOLS_DIR}/venv/bin/aqt" version
    "${QT_TOOLS_DIR}/venv/bin/aqt" install-qt linux desktop "${QT_MIN_VERSION}" linux_gcc_64 -O "${QT_INSTALL_ROOT}"
    QT6_CONFIG="$(find "${QT_INSTALL_ROOT}" -type f \( -path '*/lib/cmake/Qt6/Qt6Config.cmake' -o -path '*/lib64/cmake/Qt6/Qt6Config.cmake' \) -print -quit 2>/dev/null || true)"
    [[ -n "${QT6_CONFIG}" ]] || fail "aqtinstall completed, but Qt6Config.cmake was not found under ${QT_INSTALL_ROOT}. Check the Qt download output and rerun setup.sh."
    QT6_CONFIG_DIR="$(dirname -- "${QT6_CONFIG}")"
    QT_PREFIX="${QT6_CONFIG%/lib/cmake/Qt6/Qt6Config.cmake}"
    QT_PREFIX="${QT_PREFIX%/lib64/cmake/Qt6/Qt6Config.cmake}"
fi

say "Configuring ${APP_NAME}"
if [[ -n "${QT6_CONFIG_DIR:-}" ]]; then
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" -DQt6_DIR="${QT6_CONFIG_DIR}"
else
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" -DCMAKE_PREFIX_PATH="${QT_PREFIX}"
fi

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
APP_LAUNCHER="${INSTALL_PREFIX}/bin/shell-scratchpad-launcher"
DESKTOP_FILE="${INSTALL_PREFIX}/share/applications/org.winterrose.shellscratchpad.desktop"
SERVICE_MENU_FILE="${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-servicemenu.desktop"
SCRIPT_SERVICE_MENU_FILE="${INSTALL_PREFIX}/share/kio/servicemenus/org.winterrose.shellscratchpad-script-servicemenu.desktop"

if [[ -x "${APP_EXECUTABLE}" && -f "${DESKTOP_FILE}" ]]; then
    if [[ -n "${QT_PREFIX}" && "${QT_PREFIX}" == "${HOME}"* ]]; then
        cat > "${APP_LAUNCHER}" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${QT_PREFIX}/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${QT_PREFIX}/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="${QT_PREFIX}/plugins/platforms"
exec "${APP_EXECUTABLE}" "\$@"
EOF
    else
        cat > "${APP_LAUNCHER}" <<EOF
#!/usr/bin/env bash
exec "${APP_EXECUTABLE}" "\$@"
EOF
    fi
    chmod 0755 "${APP_LAUNCHER}"
    # Desktop Entry files do not expand shell variables. Store the concrete path
    # so the launcher works even when ~/.local/bin is not on the desktop session's PATH.
    DESKTOP_TEMP="$(mktemp)"
    awk -v exec_path="${APP_EXECUTABLE}" '
        BEGIN {
            gsub(/\\/, "\\\\", exec_path)
            gsub(/"/, "\\\"", exec_path)
        }
        /^Exec=/ { print "Exec=\"" exec_path "\" %f"; next }
        { print }
    ' "${DESKTOP_FILE}" > "${DESKTOP_TEMP}"
    sed -i "s|${APP_EXECUTABLE}|${APP_LAUNCHER}|g" "${DESKTOP_TEMP}"
    install -m 0644 "${DESKTOP_TEMP}" "${DESKTOP_FILE}"
    rm -f -- "${DESKTOP_TEMP}"
fi

if [[ -x "${APP_EXECUTABLE}" && -f "${SCRIPT_SERVICE_MENU_FILE}" ]]; then
    SCRIPT_SERVICE_MENU_TEMP="$(mktemp)"
    awk -v exec_path="${APP_LAUNCHER}" '
        BEGIN {
            gsub(/\\/, "\\\\", exec_path)
            gsub(/"/, "\\\"", exec_path)
        }
        /^Exec=shell-scratchpad %f$/ {
        print "Exec=\"" exec_path "\" %f"
            next
        }
        { print }
    ' "${SCRIPT_SERVICE_MENU_FILE}" > "${SCRIPT_SERVICE_MENU_TEMP}"
    install -m 0755 "${SCRIPT_SERVICE_MENU_TEMP}" "${SCRIPT_SERVICE_MENU_FILE}"
    rm -f -- "${SCRIPT_SERVICE_MENU_TEMP}"
fi

if [[ -x "${APP_EXECUTABLE}" && -f "${SERVICE_MENU_FILE}" ]]; then
    # KDE Plasma 6 authorizes user-installed servicemenus when the .desktop file
    # is executable. Use an absolute executable path because the desktop session
    # may not include ~/.local/bin in PATH. %f is supplied by KIO as the selected
    # local directory path and is forwarded to --working-directory.
    SERVICE_MENU_TEMP="$(mktemp)"
    awk -v exec_path="${APP_LAUNCHER}" '
        BEGIN {
            gsub(/\\/, "\\\\", exec_path)
            gsub(/"/, "\\\"", exec_path)
        }
        /^Exec=shell-scratchpad --working-directory %f$/ {
            print "Exec=\"" exec_path "\" --working-directory %f"
            next
        }
        /^Exec=shell-scratchpad --open-folder %f$/ {
            print "Exec=\"" exec_path "\" --open-folder %f"
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
printf 'Dolphin context menu: right-click a directory and choose "Open Shell Scratchpad" or "Open Folder".\n'
printf 'Open Shell Scratchpad pre-fills the working directory; Open Folder opens folder mode with the file browser.\n'
printf 'Dolphin script menu: right-click a .sh file and choose "Open in Shell Scratchpad".\n'
printf '\nBuild directory: %s\n' "${BUILD_DIR}"
printf 'Install prefix:  %s\n' "${INSTALL_PREFIX}"
printf '\nNote: Shell scripts run with your normal user permissions. Review commands before running them.\n'
