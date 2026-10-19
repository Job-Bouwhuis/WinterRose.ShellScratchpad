
# Keep a separate duplicate of the app's input stream for prompts and sudo.
exec 199<&0
SSPAD_GUI_SUDO="1"
SSPAD_REUSE_SUDO_PASSWORD="1"
SSPAD_ORIGINAL_STDIN="$(readlink "/proc/${BASHPID}/fd/0" 2>/dev/null || true)"

shell_scratchpad_record_sudo() { :; }

shell_scratchpad_emit_request() {
    local request_type="$1"
    local request_prompt="${2-}"
    local encoded_prompt
    encoded_prompt="$(builtin printf '%s' "$request_prompt" | command base64 -w0 2>/dev/null)"
    builtin printf '\036SSPAD_INPUT:%s:%s\037\n' "$request_type" "$encoded_prompt" >&2
}

read() {
    local prompt=""
    local hidden=0
    local can_use_dialog=1
    local current_stdin=""
    local option_body=""
    local option_flags=""
    local option_prompt=""
    local -a original_args=() parsed_args=()
    original_args=("$@")

    while (($#)); do
        case "$1" in
            -p)
                prompt="${2-}"
                if (($# > 1)); then shift 2; else shift; fi
                ;;
            -p*)
                prompt="${1#-p}"
                shift
                ;;
            -s)
                hidden=1
                shift
                ;;
            -d|-i|-n|-N|-t)
                # Dialogs return a complete line, not a character count or custom delimiter.
                if (($# > 1)); then shift 2; else shift; fi
                ;;
            -e)
                shift
                ;;
            -u)
                if (($# > 1)); then
                    if [[ "${2}" != "0" ]]; then can_use_dialog=0; fi
                    parsed_args+=("$1" "$2")
                    shift 2
                else
                    can_use_dialog=0
                    parsed_args+=("$1")
                    shift
                fi
                ;;
            -*)
                option_body="${1#-}"
                if [[ "$option_body" == *p* ]]; then
                    option_flags="${option_body%%p*}"
                    option_prompt="${option_body#*p}"
                    if [[ "$option_flags" == *s* ]]; then hidden=1; option_flags="${option_flags//s/}"; fi
                    if [[ -n "$option_flags" ]]; then parsed_args+=("-${option_flags}"); fi
                    if [[ -n "$option_prompt" ]]; then
                        prompt="$option_prompt"
                        shift
                    elif (($# > 1)); then
                        prompt="${2-}"
                        shift 2
                    else
                        prompt=""
                        shift
                    fi
                else
                    parsed_args+=("$1")
                    shift
                fi
                ;;
            *)
                parsed_args+=("$1")
                shift
                ;;
        esac
    done

    current_stdin="$(readlink "/proc/${BASHPID}/fd/0" 2>/dev/null || true)"
    if ((can_use_dialog)) && [[ -n "$SSPAD_ORIGINAL_STDIN" && "$current_stdin" == "$SSPAD_ORIGINAL_STDIN" ]]; then
        local request_type="read"
        if ((hidden)); then request_type="password"; fi
        shell_scratchpad_emit_request "$request_type" "$prompt"
        builtin read "${parsed_args[@]}"
    else
        builtin read "${original_args[@]}"
    fi
}

sudo() {
    shell_scratchpad_record_sudo "sudo $*"
    if ! command -v sudo >/dev/null 2>&1; then
        builtin printf 'Shell Scratchpad: sudo is not installed.\n' >&2
        return 127
    fi

    local argument=""
    for argument in "$@"; do
        if [[ "$argument" == "-n" || "$argument" == "--non-interactive" ]]; then
            command sudo "$@"
            return $?
        fi
    done

    if [[ "$SSPAD_GUI_SUDO" != "1" ]]; then
        command sudo "$@"
        return $?
    fi

    # Use the system credential timestamp whenever possible.
    if command sudo -n -v >/dev/null 2>&1; then
        command sudo "$@"
        return $?
    fi

    local password=""
    local password_is_cached=0
    local sudo_prompt="Authentication required for: sudo $*"
    local xtrace_enabled=0
    case "$-" in *x*) xtrace_enabled=1; set +x ;; esac

    if [[ "$SSPAD_REUSE_SUDO_PASSWORD" == "1" && -n "${SSPAD_SUDO_PASSWORD+x}" ]]; then
        password="$SSPAD_SUDO_PASSWORD"
        password_is_cached=1
    else
        shell_scratchpad_emit_request "sudo" "$sudo_prompt"
        local IFS=""
        if ! builtin read -r -s -u 199 password; then
            unset password
            if ((xtrace_enabled)); then set -x; fi
            return 1
        fi
    fi

    # Validate once, then retain it only in this Bash process so later calls
    # can refresh sudo's timestamp silently if necessary.
    if ! builtin printf '%s\n' "$password" | command sudo -S -p '' -v >/dev/null 2>&1; then
        unset password
        if ((password_is_cached)); then unset SSPAD_SUDO_PASSWORD; fi
        if ((xtrace_enabled)); then set -x; fi
        return 1
    fi

    if [[ "$SSPAD_REUSE_SUDO_PASSWORD" == "1" ]]; then
        SSPAD_SUDO_PASSWORD="$password"
    fi
    local command_status=0
    if command sudo "$@"; then
        command_status=0
    else
        command_status=$?
    fi
    if [[ "$SSPAD_REUSE_SUDO_PASSWORD" != "1" ]]; then
        command sudo -k >/dev/null 2>&1 || true
    fi
    unset password
    if ((xtrace_enabled)); then set -x; fi
    return "$command_status"
}


shell_scratchpad_record_sudo() {
    builtin printf 'S%s\0' "$1" >&197
}
__sspad_trace_command() {
    local previous_status=$?
    if [[ -z ${SSPAD_USER_SCRIPT_START_LINE+x} ]]; then
        return 0
    fi
    local traced_command="${1-}"
    local source_line=${BASH_LINENO[0]:-0}
    local user_line=$((source_line - SSPAD_USER_SCRIPT_START_LINE + 1))
    builtin printf 'C%s\t%s\0' "$user_line" "$traced_command" >&197
    return 0
}
trap '__sspad_trace_command "$BASH_COMMAND"' DEBUG
SSPAD_USER_SCRIPT_START_LINE=$((LINENO + 1))
git add .
git commit -m "slight overall improvements"
git push
