#include "BashExecutionHandler.h"

#include <QProcess>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ShellScratchpad::Language
{
namespace
{
constexpr int SCRIPT_DESCRIPTOR = 198;
constexpr int HISTORY_DESCRIPTOR = 197;
constexpr int DEBUG_DESCRIPTOR = 196;

QSet<int> normalizeBreakpointsToLogicalCommandLines(const QString &source,
                                                    const QSet<int> &breakpoints,
                                                    int sourceLineOffset)
{
    if (breakpoints.isEmpty())
        return {};

    QSet<int> localBreakpoints;
    for (int line : breakpoints)
        localBreakpoints.insert(line - sourceLineOffset);

    QSet<int> normalizedLocal;
    int physicalLine = 1;
    int logicalCommandLine = 1;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool escaped = false;
    bool inComment = false;

    for (qsizetype index = 0; index <= source.size(); ++index) {
        const bool atEnd = index == source.size();
        const QChar character = atEnd ? QChar() : source.at(index);
        if (atEnd || character == QLatin1Char('\n')) {
            if (localBreakpoints.contains(physicalLine))
                normalizedLocal.insert(logicalCommandLine);

            const bool continued = escaped && !inSingleQuote;
            escaped = false;
            if (!continued && !inSingleQuote && !inDoubleQuote)
                logicalCommandLine = physicalLine + 1;
            if (!continued)
                inComment = false;
            ++physicalLine;
            continue;
        }

        if (inComment)
            continue;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == QLatin1Char('\\') && !inSingleQuote) {
            escaped = true;
            continue;
        }
        if (character == QLatin1Char(39) && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            continue;
        }
        if (character == QLatin1Char('"') && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            continue;
        }
        if (character == QLatin1Char('#') && !inSingleQuote && !inDoubleQuote) {
            const bool atTokenBoundary = index == 0 || source.at(index - 1).isSpace() ||
                QStringLiteral(";&|()<>{}").contains(source.at(index - 1));
            if (atTokenBoundary)
                inComment = true;
        }
    }
    QSet<int> normalized;
    for (int line : normalizedLocal)
        normalized.insert(line + sourceLineOffset);
    return normalized;
}
}

QByteArray BashExecutionHandler::buildRuntimeScript(const BashExecutionRequest &request,
                                                    int *userScriptStartLine)
{
    const QByteArray &userScript = request.source;
    const bool handleSudoWithDialog = request.guiSudoPrompts;
    const bool reuseSudoPassword = request.reuseSudoPassword;
    const bool captureHistory = request.captureHistory;
    const bool debuggingEnabled = request.debuggingEnabled;
    const QSet<int> &breakpoints = request.breakpoints;
    const int sourceLineOffset = request.sourceLineOffset;
    const bool debugStartPaused = request.startPaused;
    static const QByteArray RUNTIME_PRELUDE = R"SSPAD(
# Keep a separate duplicate of the app's input stream for prompts and sudo.
exec 199<&0
SSPAD_GUI_SUDO="__SSPAD_GUI_SUDO__"
SSPAD_REUSE_SUDO_PASSWORD="__SSPAD_REUSE_SUDO_PASSWORD__"
SSPAD_ORIGINAL_STDIN="$(readlink "/proc/${BASHPID}/fd/0" 2>/dev/null || true)"
if [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" ]]; then
    SSPAD_CHILD_SCRIPT_PATH="$(command readlink -f -- "$0" 2>/dev/null || builtin printf '%s' "$0")"
fi

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
    if [[ -z "${SSPAD_REAL_SUDO-}" || ! -x "$SSPAD_REAL_SUDO" ]]; then
        builtin printf 'Shell Scratchpad: sudo is not installed.\n' >&2
        return 127
    fi

    local argument=""
    for argument in "$@"; do
        if [[ "$argument" == "-n" || "$argument" == "--non-interactive" ]]; then
            "$SSPAD_REAL_SUDO" "$@"
            return $?
        fi
    done

    if [[ "$SSPAD_GUI_SUDO" != "1" ]]; then
        "$SSPAD_REAL_SUDO" "$@"
        return $?
    fi

    # Use the system credential timestamp whenever possible.
    if "$SSPAD_REAL_SUDO" -n -v >/dev/null 2>&1; then
        "$SSPAD_REAL_SUDO" "$@"
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
    if ! builtin printf '%s\n' "$password" | "$SSPAD_REAL_SUDO" -S -p '' -v >/dev/null 2>&1; then
        unset password
        if ((password_is_cached)); then unset SSPAD_SUDO_PASSWORD; fi
        if ((xtrace_enabled)); then set -x; fi
        return 1
    fi
    builtin printf '\036SSPAD_SUDO_AUTHENTICATED\037\n' >&2

    if [[ "$SSPAD_REUSE_SUDO_PASSWORD" == "1" ]]; then
        SSPAD_SUDO_PASSWORD="$password"
    fi
    local command_status=0
    if "$SSPAD_REAL_SUDO" "$@"; then
        command_status=0
    else
        command_status=$?
    fi
    if [[ "$SSPAD_REUSE_SUDO_PASSWORD" != "1" ]]; then
        "$SSPAD_REAL_SUDO" -k >/dev/null 2>&1 || true
    fi
    unset password
    if ((xtrace_enabled)); then set -x; fi
    return "$command_status"
}
)SSPAD";

    QByteArray runtimeScript = RUNTIME_PRELUDE;
    runtimeScript.replace("__SSPAD_GUI_SUDO__", handleSudoWithDialog ? "1" : "0");
    runtimeScript.replace("__SSPAD_REUSE_SUDO_PASSWORD__", reuseSudoPassword ? "1" : "0");
    runtimeScript.append('\n');
    if (captureHistory && !debuggingEnabled) {
        runtimeScript.append(R"SSPAD(
shell_scratchpad_record_sudo() {
    builtin printf 'S%s\0' "$1" >&197
}
__sspad_trace_command() {
    local previous_status=$?
    if [[ -z ${SSPAD_USER_SCRIPT_START_LINE+x} && "${SSPAD_BASH_ENV_CHILD-0}" != "1" ]]; then
        return 0
    fi
    if [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" && "${BASH_SOURCE[1]-}" == "${SSPAD_RUNTIME_FILE-}" ]]; then
        return 0
    fi
    local traced_command="${1-}"
    local source_line=${BASH_LINENO[0]:-0}
    local user_line=$source_line
    if [[ "${SSPAD_BASH_ENV_CHILD-0}" != "1" ]]; then
        user_line=$((source_line - SSPAD_USER_SCRIPT_START_LINE + 1))
    fi
    builtin printf 'C%s\t%s\0' "$user_line" "$traced_command" >&197
    return 0
}
trap '__sspad_trace_command "$BASH_COMMAND"' DEBUG
)SSPAD");
    }
    if (debuggingEnabled) {
        QString breakpointSet = QStringLiteral("|");
        const QSet<int> LOGICAL_BREAKPOINTS = normalizeBreakpointsToLogicalCommandLines(
            QString::fromUtf8(request.source), breakpoints, sourceLineOffset);
        QList<int> sortedBreakpoints = LOGICAL_BREAKPOINTS.values();
        std::sort(sortedBreakpoints.begin(), sortedBreakpoints.end());
        for (int line : sortedBreakpoints)
            breakpointSet += QString::number(line) + QLatin1Char('|');
        runtimeScript += QStringLiteral("__SSPAD_DEBUG_BREAKPOINTS='%1'\n")
            .arg(breakpointSet).toUtf8();
        runtimeScript += QStringLiteral("__SSPAD_DEBUG_LINE_OFFSET=%1\n").arg(sourceLineOffset).toUtf8();
        runtimeScript.append(R"SSPAD(
__SSPAD_DEBUG_MODE=__SSPAD_DEBUG_MODE_VALUE__
__SSPAD_DEBUG_TARGET_DEPTH=0
__SSPAD_DEBUG_SKIP_LINE=0
__SSPAD_DEBUG_PAUSE_REQUESTED=0
trap '__SSPAD_DEBUG_PAUSE_REQUESTED=1' USR1
set -T
__sspad_debug() {
    local __sspad_previous_status=$?
    if [[ -z ${SSPAD_USER_SCRIPT_START_LINE+x} && "${SSPAD_BASH_ENV_CHILD-0}" != "1" ]]; then
        return 0
    fi
    if [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" && "${BASH_SOURCE[1]-}" == "${SSPAD_RUNTIME_FILE-}" ]]; then
        return 0
    fi
    local __sspad_source_line=${BASH_LINENO[0]:-${LINENO}}
    if [[ "${SSPAD_BASH_ENV_CHILD-0}" != "1" ]] && ((__sspad_source_line < SSPAD_USER_SCRIPT_START_LINE)); then
        return 0
    fi
    local __sspad_source_file="${SSPAD_CHILD_SCRIPT_PATH-}"
    local __sspad_user_line
    if [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" ]]; then
        __sspad_user_line=$__sspad_source_line
    else
        __sspad_user_line=$((__sspad_source_line - SSPAD_USER_SCRIPT_START_LINE + 1 + __SSPAD_DEBUG_LINE_OFFSET))
        __sspad_source_file=""
    fi
    local __sspad_depth=${#FUNCNAME[@]}
    local __sspad_command="${1-}"
    local __sspad_pause=0
    if [[ "${SSPAD_CAPTURE_HISTORY-0}" == "1" ]]; then
        local __sspad_history_line=$__sspad_user_line
        [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" ]] || __sspad_history_line=$((__sspad_user_line - __SSPAD_DEBUG_LINE_OFFSET))
        builtin printf 'C%s\t%s\0' "$__sspad_history_line" "$__sspad_command" >&197
    fi
    case "$__SSPAD_DEBUG_MODE" in
        start)
            __sspad_pause=1
            ;;
        run)
            if ((__SSPAD_DEBUG_SKIP_LINE == __sspad_user_line)); then
                :
            else
                __SSPAD_DEBUG_SKIP_LINE=0
                [[ "$__SSPAD_DEBUG_BREAKPOINTS" == *"|${__sspad_user_line}|"* ]] && __sspad_pause=1
            fi
            ;;
        exitdebug)
            if ((__SSPAD_DEBUG_SKIP_LINE == __sspad_user_line)); then
                __SSPAD_DEBUG_SKIP_LINE=0
            elif [[ "$__SSPAD_DEBUG_BREAKPOINTS" == *"|${__sspad_user_line}|"* ]]; then
                __SSPAD_DEBUG_MODE=run
                __sspad_pause=1
            fi
            ;;
        step)
            ((__sspad_depth <= __SSPAD_DEBUG_TARGET_DEPTH)) && __sspad_pause=1
            ;;
        stepin)
            __sspad_pause=1
            ;;
        stepout)
            ((__sspad_depth < __SSPAD_DEBUG_TARGET_DEPTH)) && __sspad_pause=1
            ;;
    esac
    if ((__SSPAD_DEBUG_PAUSE_REQUESTED)); then
        __SSPAD_DEBUG_PAUSE_REQUESTED=0
        __sspad_pause=1
    fi
    ((__sspad_pause)) || return 0

    local __sspad_encoded_command
    __sspad_encoded_command="$(builtin printf '%s' "$__sspad_command" | command base64 -w0 2>/dev/null)"
    local __sspad_encoded_source_file=""
    if [[ -n "$__sspad_source_file" ]]; then
        __sspad_encoded_source_file="$(builtin printf '%s' "$__sspad_source_file" | command base64 -w0 2>/dev/null)"
    fi
    builtin printf 'P\t%s\t%s\t%s\t%s\n' "$__sspad_user_line" "$__sspad_depth" \
        "$__sspad_encoded_command" "$__sspad_encoded_source_file" >&196

    local __sspad_encoded_variables
    __sspad_encoded_variables="$(builtin declare -p 2>/dev/null | command base64 -w0 2>/dev/null)"
    builtin printf 'V\t%s\n' "$__sspad_encoded_variables" >&196
    builtin printf 'R\n' >&196

    local __sspad_action
    while IFS= read -r -u 196 __sspad_action; do
        case "$__sspad_action" in
            e:*)
                local __sspad_expression __sspad_result_file __sspad_result_status __sspad_encoded_result
                __sspad_expression="$(builtin printf '%s' "${__sspad_action#e:}" | command base64 -d 2>/dev/null)"
                __sspad_result_file="$(command mktemp 2>/dev/null)"
                if [[ -z "$__sspad_result_file" ]]; then
                    __sspad_result_status=125
                    __sspad_encoded_result="$(builtin printf '%s' 'Could not create a temporary result file' | command base64 -w0)"
                else
                    trap - DEBUG
                    if builtin eval "$__sspad_expression" >"$__sspad_result_file" 2>&1; then
                        __sspad_result_status=0
                    else
                        __sspad_result_status=$?
                    fi
                    trap '__sspad_debug "$BASH_COMMAND"' DEBUG
                    __sspad_encoded_result="$(command base64 -w0 <"$__sspad_result_file" 2>/dev/null)"
                    command rm -f -- "$__sspad_result_file"
                fi
                builtin printf 'E\t%s\t%s\n' "$__sspad_result_status" "$__sspad_encoded_result" >&196
                __sspad_encoded_variables="$(builtin declare -p 2>/dev/null | command base64 -w0 2>/dev/null)"
                builtin printf 'V\t%s\n' "$__sspad_encoded_variables" >&196
                builtin printf 'R\n' >&196
                ;;
            c)
                __SSPAD_DEBUG_MODE=run
                __SSPAD_DEBUG_SKIP_LINE=$__sspad_user_line
                break
                ;;
            x)
                __SSPAD_DEBUG_MODE=exitdebug
                __SSPAD_DEBUG_SKIP_LINE=$__sspad_user_line
                break
                ;;
            s)
                __SSPAD_DEBUG_MODE=step
                __SSPAD_DEBUG_TARGET_DEPTH=$__sspad_depth
                break
                ;;
            i)
                __SSPAD_DEBUG_MODE=stepin
                __SSPAD_DEBUG_TARGET_DEPTH=$__sspad_depth
                break
                ;;
            o)
                if ((__sspad_depth > 1)); then
                    __SSPAD_DEBUG_MODE=stepout
                    __SSPAD_DEBUG_TARGET_DEPTH=$__sspad_depth
                    break
                fi
                ;;
        esac
    done
    return 0
}
trap '__sspad_debug "$BASH_COMMAND"' DEBUG
)SSPAD");
        runtimeScript.replace("__SSPAD_DEBUG_MODE_VALUE__", debugStartPaused ? "start" : "run");
        runtimeScript += captureHistory
            ? QByteArray("SSPAD_CAPTURE_HISTORY=1\n") : QByteArray("SSPAD_CAPTURE_HISTORY=0\n");
    }
    runtimeScript.append(R"SSPAD(
bash() {
    if (($# > 0)) && [[ "$1" != -* && -f "$1" && -n "${SSPAD_RUNTIME_FILE-}" ]]; then
        BASH_ENV="$SSPAD_RUNTIME_FILE" SSPAD_BASH_ENV_CHILD=1 command bash "$@"
    else
        command bash "$@"
    fi
}
if [[ "${SSPAD_BASH_ENV_CHILD-0}" == "1" ]]; then
    return 0
fi
)SSPAD");
    runtimeScript.append("SSPAD_USER_SCRIPT_START_LINE=$((LINENO + 1))\n");
    if (userScriptStartLine)
        *userScriptStartLine = runtimeScript.count('\n') + 1;
    runtimeScript.append(userScript);
    if (!runtimeScript.endsWith('\n'))
        runtimeScript.append('\n');
    return runtimeScript;
}


void BashExecutionHandler::configureProcess(QProcess &process, const QString &runtimeScriptPath,
                                            int scriptDescriptor,
                                            int historyReadDescriptor, int historyWriteDescriptor,
                                            int debuggerChildSocketDescriptor,
                                            int debuggerSocketDescriptor)
{
    process.setProgram(QStringLiteral("/bin/bash"));
    process.setArguments({QStringLiteral("--noprofile"), QStringLiteral("--norc"),
                          runtimeScriptPath});
    process.setChildProcessModifier([scriptDescriptor, historyReadDescriptor, historyWriteDescriptor,
                                     debuggerChildSocketDescriptor, debuggerSocketDescriptor]() {
        if (::setsid() < 0)
            ::_exit(126);
        if (::dup2(scriptDescriptor, SCRIPT_DESCRIPTOR) < 0)
            ::_exit(126);
        const int SCRIPT_FLAGS = ::fcntl(SCRIPT_DESCRIPTOR, F_GETFD);
        if (SCRIPT_FLAGS < 0 || ::fcntl(SCRIPT_DESCRIPTOR, F_SETFD, SCRIPT_FLAGS & ~FD_CLOEXEC) < 0)
            ::_exit(126);
        if (historyWriteDescriptor >= 0) {
            if (::dup2(historyWriteDescriptor, HISTORY_DESCRIPTOR) < 0)
                ::_exit(126);
            const int HISTORY_FLAGS = ::fcntl(HISTORY_DESCRIPTOR, F_GETFD);
            if (HISTORY_FLAGS < 0 || ::fcntl(HISTORY_DESCRIPTOR, F_SETFD, HISTORY_FLAGS & ~FD_CLOEXEC) < 0)
                ::_exit(126);
            if (historyReadDescriptor >= 0)
                ::close(historyReadDescriptor);
            if (historyWriteDescriptor != HISTORY_DESCRIPTOR)
                ::close(historyWriteDescriptor);
        }
        if (debuggerChildSocketDescriptor >= 0) {
            if (::dup2(debuggerChildSocketDescriptor, DEBUG_DESCRIPTOR) < 0)
                ::_exit(126);
            const int DEBUG_FLAGS = ::fcntl(DEBUG_DESCRIPTOR, F_GETFD);
            if (DEBUG_FLAGS < 0 || ::fcntl(DEBUG_DESCRIPTOR, F_SETFD, DEBUG_FLAGS & ~FD_CLOEXEC) < 0)
                ::_exit(126);
            if (debuggerSocketDescriptor >= 0 && debuggerSocketDescriptor != DEBUG_DESCRIPTOR)
                ::close(debuggerSocketDescriptor);
            if (debuggerChildSocketDescriptor != DEBUG_DESCRIPTOR)
                ::close(debuggerChildSocketDescriptor);
        }
    });
}
}
