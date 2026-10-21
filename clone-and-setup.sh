#!/usr/bin/env bash
set -Eeuo pipefail

REPO_URL="https://github.com/Job-Bouwhuis/WinterRose.ShellScratchpad.git"
DEFAULT_DESTINATION="${HOME}/ShellScratchpad"
DESTINATION="${DEFAULT_DESTINATION}"
DESTINATION_SET=0
SETUP_ARGS=()

usage() {
    printf 'Usage: %s [clone-directory] [--build-only]\n' "$0"
}

for argument in "$@"; do
    case "${argument}" in
        --build-only)
            SETUP_ARGS+=("${argument}")
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        -* )
            usage >&2
            exit 2
            ;;
        *)
            if (( DESTINATION_SET )); then
                usage >&2
                exit 2
            fi
            DESTINATION="${argument}"
            DESTINATION_SET=1
            ;;
    esac
done

ensure_git() {
    command -v git >/dev/null 2>&1 && return

    if [[ ! -r /etc/os-release ]]; then
        printf 'Error: git is not installed and this distribution could not be identified. Install git manually, then rerun this script.\n' >&2
        exit 1
    fi

    # shellcheck disable=SC1091
    source /etc/os-release
    local distro_id="${ID:-unknown,,}"
    local distro_like="${ID_LIKE:-}"
    local root_options=""
    distro_id="${distro_id,,}"
    distro_like="${distro_like,,}"
    if command -v findmnt >/dev/null 2>&1; then
        root_options="$(findmnt -n -o OPTIONS / 2>/dev/null || true)"
    fi

    if [[ "${ID:-}" == steamos || -e /run/ostree-booted || "${TRANSACTIONAL_UPDATE:-}" == 1 || ",${root_options}," == *,ro,* ]]; then
        printf 'Error: git is not installed and this OS uses a read-only or transactional system. Please ensure git is installed manually, then rerun this script.\n' >&2
        exit 1
    fi

    local installer=()
    case "${distro_id}" in
        ubuntu|debian|linuxmint|pop|elementary|zorin)
            command -v apt-get >/dev/null 2>&1 || {
                printf 'Error: git is missing, but apt-get is unavailable. Install git manually.\n' >&2
                exit 1
            }
            if [[ "${EUID}" -eq 0 ]]; then installer=();
            elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
            else
                printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                exit 1
            fi
            "${installer[@]}" apt-get update
            "${installer[@]}" apt-get install -y git
            ;;
        fedora|rhel|centos|rocky|almalinux)
            command -v dnf >/dev/null 2>&1 || {
                printf 'Error: git is missing, but dnf is unavailable. Install git manually.\n' >&2
                exit 1
            }
            if [[ "${EUID}" -eq 0 ]]; then installer=();
            elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
            else
                printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                exit 1
            fi
            "${installer[@]}" dnf install -y git
            ;;
        arch|manjaro|endeavouros)
            command -v pacman >/dev/null 2>&1 || {
                printf 'Error: git is missing, but pacman is unavailable. Install git manually.\n' >&2
                exit 1
            }
            if [[ "${EUID}" -eq 0 ]]; then installer=();
            elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
            else
                printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                exit 1
            fi
            "${installer[@]}" pacman -S --needed --noconfirm git
            ;;
        *)
            if [[ "${distro_like}" == *debian* ]] && command -v apt-get >/dev/null 2>&1; then
                if [[ "${EUID}" -eq 0 ]]; then installer=();
                elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
                else
                    printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                    exit 1
                fi
                "${installer[@]}" apt-get update
                "${installer[@]}" apt-get install -y git
            elif [[ "${distro_like}" == *fedora* ]] && command -v dnf >/dev/null 2>&1; then
                if [[ "${EUID}" -eq 0 ]]; then installer=();
                elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
                else
                    printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                    exit 1
                fi
                "${installer[@]}" dnf install -y git
            elif [[ "${distro_like}" == *arch* ]] && command -v pacman >/dev/null 2>&1; then
                if [[ "${EUID}" -eq 0 ]]; then installer=();
                elif command -v sudo >/dev/null 2>&1; then installer=(sudo);
                else
                    printf 'Error: installing git requires sudo or root privileges. Install git manually, then rerun this script.\n' >&2
                    exit 1
                fi
                "${installer[@]}" pacman -S --needed --noconfirm git
            else
                printf 'Error: automatic git installation is unsupported for %s. Install git manually, then rerun this script.\n' "${ID:-unknown}" >&2
                exit 1
            fi
            ;;
    esac

    command -v git >/dev/null 2>&1 || {
        printf 'Error: git installation completed, but the git command is still unavailable.\n' >&2
        exit 1
    }
}

ensure_git

say() {
    printf '\n==> %s\n' "$*"
}

if [[ -d "${DESTINATION}/.git" ]]; then
    EXISTING_REMOTE="$(git -C "${DESTINATION}" remote get-url origin)"
    [[ "${EXISTING_REMOTE}" == "${REPO_URL}" ]] || {
        printf 'Error: destination uses a different Git remote: %s\n' "${DESTINATION}" >&2
        exit 1
    }
    say "Updating the existing Shell Scratchpad checkout"
    git -C "${DESTINATION}" pull --ff-only
elif [[ -e "${DESTINATION}" ]]; then
    printf 'Error: destination already exists and is not a Shell Scratchpad Git checkout: %s\n' "${DESTINATION}" >&2
    exit 1
else
    say "Cloning Shell Scratchpad from ${REPO_URL}"
    git clone --depth 1 "${REPO_URL}" "${DESTINATION}"
fi

say "Running the project setup script"
bash "${DESTINATION}/setup.sh" "${SETUP_ARGS[@]}"
