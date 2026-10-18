#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QKeySequence>
#include <QKeyCombination>
#include <QLabel>
#include <QKeyEvent>
#include <QList>
#include <QListWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QPainter>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSocketNotifier>
#include <QRegularExpression>
#include <QSplitter>
#include <QStatusBar>
#include <QSet>
#include <QSyntaxHighlighter>
#include <QStringList>
#include <QStringView>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QThreadPool>
#include <QRunnable>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <atomic>
#include <algorithm>
#include <memory>

#include "syntax/SyntaxHighlighter.h"
#include "syntax/SyntaxTokenizer.h"
#include "syntax/SyntaxDebugDialog.h"
#include "syntax/SyntaxColorDialog.h"
#include "editor/CodeEditor.h"
#include "completion/CompletionController.h"
#include "snippets/SnippetManagerDialog.h"
#include "snippets/SnippetCreateDialog.h"
#include "snippets/SnippetStore.h"
#include "util/FuzzyMatcher.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
constexpr int SCRIPT_DESCRIPTOR = 198;
constexpr int HISTORY_DESCRIPTOR = 197;
constexpr int DEBUG_DESCRIPTOR = 196;
const QByteArray INPUT_MARKER_PREFIX = QByteArray(1, '\x1e') + "SSPAD_INPUT:";
constexpr char INPUT_MARKER_END = '\x1f';

QByteArray buildRuntimeScript(const QByteArray &userScript, bool handleSudoWithDialog,
                              bool reuseSudoPassword, bool captureHistory,
                              bool debuggingEnabled, const QSet<int> &breakpoints,
                              int sourceLineOffset, bool debugStartPaused)
{
    static const QByteArray RUNTIME_PRELUDE = R"SSPAD(
# Keep a separate duplicate of the app's input stream for prompts and sudo.
exec 199<&0
SSPAD_GUI_SUDO="__SSPAD_GUI_SUDO__"
SSPAD_REUSE_SUDO_PASSWORD="__SSPAD_REUSE_SUDO_PASSWORD__"
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
    if [[ -z ${SSPAD_USER_SCRIPT_START_LINE+x} ]]; then
        return "$previous_status"
    fi
    local traced_command="${1-}"
    local source_line=${BASH_LINENO[0]:-0}
    local user_line=$((source_line - SSPAD_USER_SCRIPT_START_LINE + 1))
    builtin printf 'C%s\t%s\0' "$user_line" "$traced_command" >&197
    return "$previous_status"
}
trap '__sspad_trace_command "$BASH_COMMAND"' DEBUG
)SSPAD");
    }
    if (debuggingEnabled) {
        QString breakpointSet = QStringLiteral("|");
        QList<int> sortedBreakpoints = breakpoints.values();
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
set -T
__sspad_debug() {
    local __sspad_previous_status=$?
    if [[ -z ${SSPAD_USER_SCRIPT_START_LINE+x} ]]; then
        return "$__sspad_previous_status"
    fi
    local __sspad_source_line=${BASH_LINENO[0]:-${LINENO}}
    if ((__sspad_source_line < SSPAD_USER_SCRIPT_START_LINE)); then
        return "$__sspad_previous_status"
    fi
    local __sspad_user_line=$((__sspad_source_line - SSPAD_USER_SCRIPT_START_LINE + 1 + __SSPAD_DEBUG_LINE_OFFSET))
    local __sspad_depth=${#FUNCNAME[@]}
    local __sspad_command="${1-}"
    local __sspad_pause=0
    if [[ "${SSPAD_CAPTURE_HISTORY-0}" == "1" ]]; then
        builtin printf 'C%s\t%s\0' "$((__sspad_user_line - __SSPAD_DEBUG_LINE_OFFSET))" "$__sspad_command" >&197
    fi
    case "$__SSPAD_DEBUG_MODE" in
        run)
            if ((__SSPAD_DEBUG_SKIP_LINE == __sspad_user_line)); then
                :
            else
                __SSPAD_DEBUG_SKIP_LINE=0
                [[ "$__SSPAD_DEBUG_BREAKPOINTS" == *"|${__sspad_user_line}|"* ]] && __sspad_pause=1
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
    ((__sspad_pause)) || return "$__sspad_previous_status"

    local __sspad_encoded_command
    __sspad_encoded_command="$(builtin printf '%s' "$__sspad_command" | command base64 -w0 2>/dev/null)"
    builtin printf 'P\t%s\t%s\t%s\n' "$__sspad_user_line" "$__sspad_depth" "$__sspad_encoded_command" >&196

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
    return "$__sspad_previous_status"
}
trap '__sspad_debug "$BASH_COMMAND"' DEBUG
)SSPAD");
        runtimeScript.replace("__SSPAD_DEBUG_MODE_VALUE__", debugStartPaused ? "start" : "run");
        runtimeScript += captureHistory
            ? QByteArray("SSPAD_CAPTURE_HISTORY=1\n") : QByteArray("SSPAD_CAPTURE_HISTORY=0\n");
        runtimeScript.append("SSPAD_USER_SCRIPT_START_LINE=$((LINENO + 1))\n");
    } else {
        runtimeScript.append("SSPAD_USER_SCRIPT_START_LINE=$((LINENO + 1))\n");
    }
    runtimeScript.append(userScript);
    if (!runtimeScript.endsWith('\n'))
        runtimeScript.append('\n');
    return runtimeScript;
}

int createAnonymousScriptFile(const QByteArray &scriptContents)
{
    const int FILE_DESCRIPTOR = ::memfd_create("shell-scratchpad-script", MFD_CLOEXEC);
    if (FILE_DESCRIPTOR < 0)
        return -1;

    qsizetype totalWritten = 0;
    while (totalWritten < scriptContents.size()) {
        const ssize_t WRITTEN = ::write(FILE_DESCRIPTOR,
                                        scriptContents.constData() + totalWritten,
                                        static_cast<size_t>(scriptContents.size() - totalWritten));
        if (WRITTEN < 0) {
            if (errno == EINTR)
                continue;
            ::close(FILE_DESCRIPTOR);
            return -1;
        }
        if (WRITTEN == 0) {
            ::close(FILE_DESCRIPTOR);
            errno = EIO;
            return -1;
        }
        totalWritten += WRITTEN;
    }

    if (::lseek(FILE_DESCRIPTOR, 0, SEEK_SET) < 0) {
        ::close(FILE_DESCRIPTOR);
        return -1;
    }

    return FILE_DESCRIPTOR;
}
}

class ShellScratchpadWindow;

namespace
{
QList<ShellScratchpadWindow *> &openScratchpadWindows()
{
    static QList<ShellScratchpadWindow *> windows;
    return windows;
}

class KeyChordDispatcher final : public QObject
{
public:
    explicit KeyChordDispatcher(QMainWindow *window)
        : QObject(window), owner(window)
    {
        timeout.setSingleShot(true);
        timeout.setInterval(1800);
        connect(&timeout, &QTimer::timeout, this, [this]() { clearPending(); });
    }

    void registerChord(const QKeySequence &sequence, std::function<void()> callback)
    {
        if (sequence.count() != 2)
            return;
        const int FIRST = sequence[0].toCombined();
        const int SECOND = sequence[1].toCombined();
        bindings[FIRST].insert(SECOND, std::move(callback));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) ||
            !owner || QApplication::activeWindow() != owner)
            return QObject::eventFilter(watched, event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || widget->window() != owner)
            return QObject::eventFilter(watched, event);

        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int KEY = QKeyCombination(keyEvent->modifiers(),
                                        static_cast<Qt::Key>(keyEvent->key())).toCombined();
        if (event->type() == QEvent::ShortcutOverride) {
            if (pendingPrefix != 0 || bindings.contains(KEY))
                keyEvent->accept();
            return QObject::eventFilter(watched, event);
        }
        if (keyEvent->isAutoRepeat())
            return QObject::eventFilter(watched, event);
        if (pendingPrefix != 0) {
            const auto FOLLOWUPS = bindings.constFind(pendingPrefix);
            if (FOLLOWUPS != bindings.cend()) {
                const auto MATCH = FOLLOWUPS->constFind(KEY);
                if (MATCH != FOLLOWUPS->cend()) {
                    const std::function<void()> CALLBACK = MATCH.value();
                    clearPending();
                    CALLBACK();
                    return true;
                }
            }
            clearPending();
            return true;
        }

        const auto PREFIX = bindings.constFind(KEY);
        if (PREFIX == bindings.cend())
            return QObject::eventFilter(watched, event);

        pendingPrefix = KEY;
        QStringList FOLLOWUP_NAMES;
        for (int followup : PREFIX->keys())
            FOLLOWUP_NAMES.append(QKeySequence(followup).toString(QKeySequence::NativeText));
        owner->statusBar()->showMessage(
            QStringLiteral("Waiting for follow-up key after %1 (%2)…")
                .arg(QKeySequence(KEY).toString(QKeySequence::NativeText), FOLLOWUP_NAMES.join(QStringLiteral(", "))));
        timeout.start();
        return true;
    }

private:
    void clearPending()
    {
        timeout.stop();
        pendingPrefix = 0;
        if (owner)
            owner->statusBar()->clearMessage();
    }

    QMainWindow *owner = nullptr;
    QHash<int, QHash<int, std::function<void()>>> bindings;
    QTimer timeout;
    int pendingPrefix = 0;
};

class OutputMatchHighlighter final : public QSyntaxHighlighter
{
public:
    explicit OutputMatchHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        matchFormat.setBackground(QColor(QStringLiteral("#806d25")));
        matchFormat.setForeground(QColor(QStringLiteral("#fff4c2")));
    }

    void setQuery(const QString &newQuery, bool caseSensitive)
    {
        if (query == newQuery && this->caseSensitive == caseSensitive)
            return;
        query = newQuery;
        this->caseSensitive = caseSensitive;
        rehighlight();
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (query.isEmpty())
            return;
        qsizetype start = 0;
        while (start < text.size()) {
            const qsizetype MATCH = text.indexOf(query, start,
                caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
            if (MATCH < 0)
                break;
            setFormat(static_cast<int>(MATCH), query.size(), matchFormat);
            start = MATCH + qMax(query.size(), 1);
        }
    }

private:
    QString query;
    bool caseSensitive = false;
    QTextCharFormat matchFormat;
};
}

class ShellScratchpadWindow final : public QMainWindow
{
public:
    explicit ShellScratchpadWindow(const QString &initialWorkingDirectory,
                                   const QString &initialScriptPath = QString(),
                                   bool offerRecovery = false)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QStringLiteral("Shell Scratchpad"));
        setWindowIcon(QIcon::fromTheme(QStringLiteral("shell-scratchpad")));
        resize(980, 760);
        setMinimumSize(640, 480);
        openScratchpadWindows().append(this);

        auto *central = new QWidget(this);
        auto *pageLayout = new QVBoxLayout(central);
        pageLayout->setContentsMargins(18, 16, 18, 14);
        pageLayout->setSpacing(10);

        auto *directoryRow = new QHBoxLayout();
        directoryRow->setSpacing(8);
        auto *directoryLabel = new QLabel(QStringLiteral("Working directory"), central);
        directoryEdit = new QLineEdit(
            initialWorkingDirectory.isEmpty() ? QDir::homePath() : initialWorkingDirectory, central);
        directoryEdit->setClearButtonEnabled(true);
        defaultWorkingDirectory = directoryEdit->text();
        directoryEdit->setPlaceholderText(QStringLiteral("Directory used by the script, e.g. ~/Projects"));
        runProfileCombo = new QComboBox(central);
        runProfileCombo->setMinimumWidth(140);
        runProfileCombo->setToolTip(QStringLiteral("Select a saved working-directory and environment profile"));
        auto *manageProfilesButton = new QPushButton(QStringLiteral("Profiles…"), central);
        connect(manageProfilesButton, &QPushButton::clicked, this, [this]() { manageRunProfiles(); });
        auto *browseButton = new QPushButton(QStringLiteral("Browse…"), central);
        connect(browseButton, &QPushButton::clicked, this, [this]() {
            const QString SELECTED_DIRECTORY = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Choose working directory"), expandedWorkingDirectory());
            if (!SELECTED_DIRECTORY.isEmpty())
                directoryEdit->setText(QDir::cleanPath(SELECTED_DIRECTORY));
        });
        directoryRow->addWidget(directoryLabel);
        directoryRow->addWidget(directoryEdit, 1);
        directoryRow->addWidget(browseButton);
        directoryRow->addWidget(runProfileCombo);
        directoryRow->addWidget(manageProfilesButton);
        pageLayout->addLayout(directoryRow);
        loadRunProfiles();
        connect(runProfileCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (index == 0) {
                QSettings().setValue(QStringLiteral("runProfiles/active"), QString());
                directoryEdit->setText(defaultWorkingDirectory);
                refreshCompletionProfileVariables();
                if (!applyingScriptRunProfile)
                    saveScriptRunProfile(QString());
                return;
            }
            if (index < 0)
                return;
            const QString NAME = runProfileCombo->itemText(index);
            for (const QJsonValue &value : storedRunProfiles()) {
                const QJsonObject PROFILE = value.toObject();
                if (PROFILE.value(QStringLiteral("name")).toString() == NAME) {
                    const QString PATH = PROFILE.value(QStringLiteral("directory")).toString();
                    if (!PATH.isEmpty())
                        directoryEdit->setText(PATH);
                    QSettings().setValue(QStringLiteral("runProfiles/active"), NAME);
                    if (!applyingScriptRunProfile)
                        saveScriptRunProfile(NAME);
                    break;
                }
            }
            refreshCompletionProfileVariables();
        });

        splitter = new QSplitter(Qt::Vertical, central);
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(7);

        scriptSideSplitter = new QSplitter(Qt::Horizontal, splitter);
        scriptSideSplitter->setChildrenCollapsible(false);
        auto *scriptPanel = new QWidget(scriptSideSplitter);
        auto *scriptLayout = new QVBoxLayout(scriptPanel);
        scriptLayout->setContentsMargins(0, 0, 0, 0);
        scriptLayout->setSpacing(6);
        auto *scriptHeading = new QLabel(QStringLiteral("Bash script"), scriptPanel);
        debuggerBanner = new QLabel(scriptPanel);
        debuggerBanner->setVisible(false);
        debuggerBanner->setStyleSheet(QStringLiteral("QLabel { background: #8f2525; color: white; font-weight: bold; padding: 6px; border-radius: 3px; }"));
        scriptEdit = new ShellScratchpad::CodeEditor(scriptPanel);
        scriptEdit->setObjectName(QStringLiteral("scriptEdit"));
        scriptEdit->setPlaceholderText(
            QStringLiteral("#!/usr/bin/env bash\nset -euo pipefail\n\n# Paste or write your script here…"));
        scriptEdit->setTabChangesFocus(false);
        scriptEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        scriptEdit->setContextMenuPolicy(Qt::CustomContextMenu);
        scriptEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        scriptEdit->setPlainText(QStringLiteral(
            "echo \"Hello, World!\"\n"));
        scriptEdit->document()->setModified(false);
        new ShellScratchpad::Syntax::SyntaxHighlighter(scriptEdit);
        completionController = new ShellScratchpad::Completion::CompletionController(scriptEdit, scriptEdit);
        connect(scriptEdit, &QPlainTextEdit::textChanged, this, [this]() { refreshNavigationSymbols(); });
        connect(scriptEdit, &ShellScratchpad::CodeEditor::symbolNavigationRequested, this,
                [this](const QString &name) {
            const QJsonObject profile = selectedRunProfile();
            if (profile.value(QStringLiteral("environment")).toObject().contains(name)) {
                manageRunProfiles(profile.value(QStringLiteral("name")).toString(), name);
                return;
            }
            const auto &symbols = completionController->scriptSymbols();
            const auto symbol = std::find_if(symbols.cbegin(), symbols.cend(),
                [&name](const auto &candidate) { return candidate.name == name; });
            if (symbol == symbols.cend())
                return;
            scriptEdit->revealPosition(symbol->declarationOffset);
            QTextCursor declaration(scriptEdit->document());
            declaration.setPosition(static_cast<int>(symbol->declarationOffset));
            declaration.setPosition(static_cast<int>(symbol->declarationOffset + name.size()), QTextCursor::KeepAnchor);
            scriptEdit->setTextCursor(declaration);
            scriptEdit->centerCursor();
            scriptEdit->setFocus();
        });
        refreshCompletionProfileVariables();
        scriptLayout->addWidget(scriptHeading);
        scriptLayout->addWidget(debuggerBanner);
        scriptSearchRow = new QWidget(scriptPanel);
        auto *scriptSearchLayout = new QHBoxLayout(scriptSearchRow);
        scriptSearchLayout->setContentsMargins(0, 0, 0, 0);
        scriptSearchLayout->setSpacing(5);
        scriptFindEdit = new QLineEdit(scriptSearchRow);
        scriptFindEdit->setClearButtonEnabled(true);
        scriptFindEdit->setPlaceholderText(QStringLiteral("Find in script"));
        scriptFindEdit->setToolTip(QStringLiteral("Search the script. Enter finds the next match; Shift+Enter finds the previous match."));
        auto *findPreviousButton = new QPushButton(QStringLiteral("↑"), scriptSearchRow);
        findPreviousButton->setToolTip(QStringLiteral("Find previous (Shift+Enter)"));
        auto *findNextButton = new QPushButton(QStringLiteral("↓"), scriptSearchRow);
        findNextButton->setToolTip(QStringLiteral("Find next (Enter)"));
        auto *findOptionsButton = new QToolButton(scriptSearchRow);
        findOptionsButton->setText(QStringLiteral("Options"));
        findOptionsButton->setPopupMode(QToolButton::InstantPopup);
        auto *findOptionsMenu = new QMenu(findOptionsButton);
        scriptFindCaseSensitiveAction = findOptionsMenu->addAction(QStringLiteral("Case sensitive"));
        scriptFindCaseSensitiveAction->setCheckable(true);
        findOptionsButton->setMenu(findOptionsMenu);
        auto *closeFindButton = new QPushButton(QStringLiteral("×"), scriptSearchRow);
        closeFindButton->setToolTip(QStringLiteral("Close search (Esc)"));
        scriptSearchLayout->addWidget(scriptFindEdit, 1);
        scriptSearchLayout->addWidget(findPreviousButton);
        scriptSearchLayout->addWidget(findNextButton);
        scriptSearchLayout->addWidget(findOptionsButton);
        scriptSearchLayout->addWidget(closeFindButton);
        scriptLayout->addWidget(scriptSearchRow);
        scriptSearchRow->hide();
        connect(findNextButton, &QPushButton::clicked, this, [this]() { findInScript(false); });
        connect(findPreviousButton, &QPushButton::clicked, this, [this]() { findInScript(true); });
        connect(scriptFindEdit, &QLineEdit::textChanged, this, [this]() {
            if (!scriptFindEdit->text().isEmpty())
                findInScript(false, true);
            else {
                QTextCursor cursor = scriptEdit->textCursor();
                cursor.clearSelection();
                scriptEdit->setTextCursor(cursor);
            }
        });
        connect(scriptFindCaseSensitiveAction, &QAction::toggled, this, [this]() {
            if (!scriptFindEdit->text().isEmpty())
                findInScript(false, true);
        });
        connect(closeFindButton, &QPushButton::clicked, this, [this]() {
            scriptSearchRow->hide();
            scriptEdit->setFocus();
        });
        scriptFindEdit->installEventFilter(this);
        scriptLayout->addWidget(scriptEdit, 1);

        variablesPanel = new QWidget(scriptSideSplitter);
        auto *variablesLayout = new QVBoxLayout(variablesPanel);
        variablesLayout->setContentsMargins(5, 0, 0, 0);
        auto *variablesHeading = new QLabel(QStringLiteral("Paused Bash variables"), variablesPanel);
        variablesFilter = new QLineEdit(variablesPanel);
        variablesFilter->setPlaceholderText(QStringLiteral("Filter variables"));
        variablesTree = new QTreeWidget(variablesPanel);
        variablesTree->setColumnCount(3);
        variablesTree->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Flags"), QStringLiteral("Value / declaration")});
        variablesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        variablesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        variablesTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        variablesTree->setRootIsDecorated(false);
        variablesTree->setAlternatingRowColors(true);
        variablesTree->setToolTip(QStringLiteral("Variables captured from the current Bash execution context. Values use Bash's declaration representation."));
        variablesLayout->addWidget(variablesHeading);
        variablesLayout->addWidget(variablesFilter);
        variablesLayout->addWidget(variablesTree, 1);
        variablesPanel->hide();
        connect(variablesFilter, &QLineEdit::textChanged, this, [this]() { refreshDebuggerVariableTree(); });
        scriptSideSplitter->addWidget(scriptPanel);
        scriptSideSplitter->addWidget(variablesPanel);
        scriptSideSplitter->setStretchFactor(0, 3);
        scriptSideSplitter->setStretchFactor(1, 1);
        scriptSideSplitter->setSizes({760, 280});

        outputSideSplitter = new QSplitter(Qt::Horizontal, splitter);
        outputSideSplitter->setChildrenCollapsible(false);
        outputPanel = new QWidget(outputSideSplitter);
        auto *outputLayout = new QVBoxLayout(outputPanel);
        outputLayout->setContentsMargins(0, 0, 0, 0);
        outputLayout->setSpacing(6);
        auto *outputHeadingRow = new QHBoxLayout();
        auto *outputHeading = new QLabel(QStringLiteral("Output"), outputPanel);
        auto *copyButton = new QPushButton(QStringLiteral("Copy output"), outputPanel);
        copyButton->setToolTip(QStringLiteral("Copy selected output, or all output when nothing is selected"));
        connect(copyButton, &QPushButton::clicked, this, [this]() { copyOutput(); });
        auto *clearButton = new QPushButton(QStringLiteral("Clear output"), outputPanel);
        connect(clearButton, &QPushButton::clicked, this, [this]() {
            clearOutput();
        });
        auto *bookmarkButton = new QPushButton(QStringLiteral("Bookmark selection"), outputPanel);
        bookmarkButton->setToolTip(QStringLiteral("Save selected output text as a bookmark, or bookmark the current line when there is no selection"));
        connect(bookmarkButton, &QPushButton::clicked, this, [this]() { addOutputBookmark(); });
        outputHeadingRow->addWidget(outputHeading);
        outputHeadingRow->addStretch(1);
        outputHeadingRow->addWidget(copyButton);
        outputHeadingRow->addWidget(clearButton);
        outputHeadingRow->addWidget(bookmarkButton);

        outputSearchRow = new QWidget(outputPanel);
        auto *outputSearchLayout = new QHBoxLayout(outputSearchRow);
        outputSearchLayout->setContentsMargins(0, 0, 0, 0);
        outputSearchLayout->setSpacing(5);
        outputFindEdit = new QLineEdit(outputPanel);
        outputFindEdit->setClearButtonEnabled(true);
        outputFindEdit->setPlaceholderText(QStringLiteral("Type to show matching lines…"));
        outputFindEdit->setToolTip(QStringLiteral("Filter output to lines containing this text; matching characters are highlighted."));
        auto *outputPreviousButton = new QPushButton(QStringLiteral("↑"), outputSearchRow);
        outputPreviousButton->setToolTip(QStringLiteral("Previous matching line (Shift+Enter)"));
        auto *outputNextButton = new QPushButton(QStringLiteral("↓"), outputSearchRow);
        outputNextButton->setToolTip(QStringLiteral("Next matching line (Enter)"));
        auto *outputOptionsButton = new QToolButton(outputSearchRow);
        outputOptionsButton->setText(QStringLiteral("Options"));
        outputOptionsButton->setPopupMode(QToolButton::InstantPopup);
        auto *outputOptionsMenu = new QMenu(outputOptionsButton);
        outputFindCaseSensitiveAction = outputOptionsMenu->addAction(QStringLiteral("Case sensitive"));
        outputFindCaseSensitiveAction->setCheckable(true);
        outputOptionsButton->setMenu(outputOptionsMenu);
        auto *closeOutputFindButton = new QPushButton(QStringLiteral("×"), outputSearchRow);
        closeOutputFindButton->setToolTip(QStringLiteral("Close search (Esc)"));
        outputSearchLayout->addWidget(outputFindEdit, 1);
        outputSearchLayout->addWidget(outputPreviousButton);
        outputSearchLayout->addWidget(outputNextButton);
        outputSearchLayout->addWidget(outputOptionsButton);
        outputSearchLayout->addWidget(closeOutputFindButton);
        outputSearchRow->hide();
        connect(outputPreviousButton, &QPushButton::clicked, this, [this]() { navigateOutputMatches(true); });
        connect(outputNextButton, &QPushButton::clicked, this, [this]() { navigateOutputMatches(false); });
        connect(closeOutputFindButton, &QPushButton::clicked, this, [this]() {
            outputFindEdit->clear();
            outputSearchRow->hide();
            outputEdit->setFocus();
        });
        outputFindEdit->installEventFilter(this);

        outputEdit = new QPlainTextEdit(outputPanel);
        outputEdit->setReadOnly(true);
        outputEdit->setObjectName(QStringLiteral("outputEdit"));
        outputEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        outputEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        outputEdit->setPlaceholderText(QStringLiteral("Script output and errors will appear here."));
        outputMatchHighlighter = new OutputMatchHighlighter(outputEdit->document());
        outputLayout->addLayout(outputHeadingRow);
        outputLayout->addWidget(outputSearchRow);
        outputLayout->addWidget(outputEdit, 1);

        immediatePanel = new QWidget(outputSideSplitter);
        auto *immediateLayout = new QVBoxLayout(immediatePanel);
        immediateLayout->setContentsMargins(5, 0, 0, 0);
        auto *immediateHeading = new QLabel(QStringLiteral("Immediate"), immediatePanel);
        immediateHeading->setToolTip(QStringLiteral("Evaluate a Bash expression in the paused script process. Assignments and other side effects persist."));
        immediateOutputEdit = new QPlainTextEdit(immediatePanel);
        immediateOutputEdit->setReadOnly(true);
        immediateOutputEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        immediateOutputEdit->setPlaceholderText(QStringLiteral("Results from expressions run in the paused Bash process appear here."));
        auto *immediateEntryRow = new QHBoxLayout();
        immediateInputEdit = new QLineEdit(immediatePanel);
        immediateInputEdit->setPlaceholderText(QStringLiteral("Bash expression or command…"));
        immediateInputEdit->setToolTip(QStringLiteral("Runs in the live paused shell; assignments and side effects persist."));
        immediateRunButton = new QPushButton(QStringLiteral("Run"), immediatePanel);
        immediateEntryRow->addWidget(immediateInputEdit, 1);
        immediateEntryRow->addWidget(immediateRunButton);
        immediateLayout->addWidget(immediateHeading);
        immediateLayout->addWidget(immediateOutputEdit, 1);
        immediateLayout->addLayout(immediateEntryRow);
        immediatePanel->hide();
        connect(immediateRunButton, &QPushButton::clicked, this, [this]() { evaluateImmediate(); });
        connect(immediateInputEdit, &QLineEdit::returnPressed, this, [this]() { evaluateImmediate(); });
        outputSideSplitter->addWidget(outputPanel);
        outputSideSplitter->addWidget(immediatePanel);
        outputSideSplitter->setStretchFactor(0, 2);
        outputSideSplitter->setStretchFactor(1, 1);
        outputSideSplitter->setSizes({650, 350});

        splitter->addWidget(scriptSideSplitter);
        splitter->addWidget(outputSideSplitter);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        pageLayout->addWidget(splitter, 1);

        auto *buttonRow = new QHBoxLayout();
        buttonRow->setSpacing(8);
        runButton = new QPushButton(QStringLiteral("▶  Run script"), central);
        runButton->setObjectName(QStringLiteral("runButton"));
        runButton->setDefault(true);
        stopButton = new QPushButton(QStringLiteral("■  Stop"), central);
        stopButton->setEnabled(false);
        runStatusLabel = new QLabel(QStringLiteral("Ready"), central);
        runDurationLabel = new QLabel(QStringLiteral(""), central);
        auto *saveButton = new QPushButton(QStringLiteral("Save script"), central);
        saveButton->setToolTip(QStringLiteral("Save the script and make it executable."));
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveScript(); });
        auto *newButton = new QPushButton(QStringLiteral("New script"), central);
        connect(newButton, &QPushButton::clicked, this, [this]() { createNewScript(); });
        buttonRow->addWidget(runButton);
        buttonRow->addWidget(stopButton);
        buttonRow->addStretch(1);
        buttonRow->addWidget(saveButton);
        buttonRow->addWidget(newButton);
        pageLayout->addLayout(buttonRow);

        setCentralWidget(central);

        outputFilterPool.setMaxThreadCount(1);
        outputFilterTimer.setSingleShot(true);
        outputFilterTimer.setInterval(90);
        connect(&outputFilterTimer, &QTimer::timeout, this, [this]() { filterOutputInBackground(); });
        connect(scriptEdit->document(), &QTextDocument::modificationChanged, this,
                [this](bool) { updateWindowTitle(); });
        connect(scriptEdit, &QPlainTextEdit::textChanged, this, [this]() {
            updateWindowTitle();
            scriptEdit->setTextDecorations({});
            if (recoveryEnabled)
                recoveryTimer.start();
            latestDraftTimer.start();
            ++shellCheckGeneration;
            shellCheckCurrent = false;
            shellCheckTimer.start();
        });
        recoveryTimer.setSingleShot(true);
        recoveryTimer.setInterval(800);
        connect(&recoveryTimer, &QTimer::timeout, this, [this]() { writeRecoveryDraft(); });
        latestDraftTimer.setSingleShot(true);
        latestDraftTimer.setInterval(750);
        connect(&latestDraftTimer, &QTimer::timeout, this, [this]() { writeLatestDraft(); });
        shellCheckTimer.setSingleShot(true);
        shellCheckTimer.setInterval(250);
        connect(&shellCheckTimer, &QTimer::timeout, this, [this]() { startShellCheck(); });
        configureShellCheckProcess();
        diagnosticsDock = new QDockWidget(QStringLiteral("Script Diagnostics"), this);
        diagnosticsDock->setObjectName(QStringLiteral("scriptDiagnosticsDock"));
        diagnosticsList = new QListWidget(diagnosticsDock);
        diagnosticsList->setAlternatingRowColors(true);
        diagnosticsDock->setWidget(diagnosticsList);
        addDockWidget(Qt::BottomDockWidgetArea, diagnosticsDock);
        diagnosticsDock->hide();
        connect(diagnosticsList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            const int FIX_START = item->data(Qt::UserRole + 5).toInt();
            const int FIX_LENGTH = item->data(Qt::UserRole + 6).toInt();
            const QString FIX_REPLACEMENT = item->data(Qt::UserRole + 7).toString();
            const QString EXPECTED_TEXT = item->data(Qt::UserRole + 8).toString();
            if (FIX_LENGTH > 0 && !FIX_REPLACEMENT.isEmpty() && FIX_START >= 0 &&
                scriptEdit->toPlainText().mid(FIX_START, FIX_LENGTH) == EXPECTED_TEXT) {
                QTextCursor fix(scriptEdit->document());
                fix.setPosition(FIX_START);
                fix.setPosition(FIX_START + FIX_LENGTH, QTextCursor::KeepAnchor);
                fix.insertText(FIX_REPLACEMENT);
                fix.setPosition(FIX_START);
                fix.setPosition(FIX_START + FIX_REPLACEMENT.size(), QTextCursor::KeepAnchor);
                scriptEdit->setTextCursor(fix);
                scriptEdit->setFocus();
                return;
            }
            const int LINE = item->data(Qt::UserRole).toInt();
            if (LINE > 0) {
                QTextCursor cursor(scriptEdit->document()->findBlockByNumber(LINE - 1));
                scriptEdit->setTextCursor(cursor);
                scriptEdit->centerCursor();
                scriptEdit->setFocus();
            }
        });
        recoveryEnabled = offerRecovery;

        if (!initialScriptPath.isEmpty()) {
            openScriptPath(initialScriptPath, false);
        } else if (offerRecovery) {
            restoreRecoveryDraft();
        }

        keyChordDispatcher = new KeyChordDispatcher(this);
        qApp->installEventFilter(keyChordDispatcher);
        createMenus();
        statusBar()->showMessage(QStringLiteral("Ready  •  Alt+Enter to run"));
        statusBar()->addPermanentWidget(runStatusLabel);
        statusBar()->addPermanentWidget(runDurationLabel);

        process.setProcessChannelMode(QProcess::MergedChannels);
        connect(&process, &QProcess::started, this, [this]() { closeRunHistoryWriteEnd(); });
        connect(runButton, &QPushButton::clicked, this, [this]() { runScript(); });
        connect(scriptEdit, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            const bool HAS_SELECTION = scriptEdit->textCursor().hasSelection();
            QMenu contextMenu(scriptEdit);

            // Resolve the clicked token independently of the current caret/selection.
            const QString SOURCE = scriptEdit->toPlainText();
            const QTextCursor CLICK_CURSOR = scriptEdit->cursorForPosition(position);
            qsizetype CLICK_OFFSET = CLICK_CURSOR.position();
            if (CLICK_OFFSET == SOURCE.size() && CLICK_OFFSET > 0)
                --CLICK_OFFSET;
            const QVector<ShellScratchpad::Syntax::SyntaxToken> TOKENS =
                ShellScratchpad::Syntax::SyntaxTokenizer::tokenize(SOURCE);
            const ShellScratchpad::Syntax::SyntaxToken *clickedToken = nullptr;
            for (const auto &token : TOKENS) {
                if (CLICK_OFFSET >= token.startOffset &&
                    CLICK_OFFSET < token.startOffset + token.length) {
                    clickedToken = &token;
                    break;
                }
            }
            QString commandContext;
            bool commandContextBuiltin = false;
            if (clickedToken) {
                for (const auto &token : TOKENS) {
                    if (token.startOffset > clickedToken->startOffset)
                        break;
                    if (token.kind == ShellScratchpad::Syntax::TokenKind::Operator) {
                        const QString OPERATOR = SOURCE.mid(token.startOffset, token.length);
                        if (OPERATOR == QStringLiteral(";") || OPERATOR == QStringLiteral("&&") ||
                            OPERATOR == QStringLiteral("||") || OPERATOR == QStringLiteral("|") ||
                            OPERATOR == QStringLiteral("|&")) {
                            commandContext.clear();
                            commandContextBuiltin = false;
                        }
                    } else if (token.kind == ShellScratchpad::Syntax::TokenKind::Command ||
                               token.kind == ShellScratchpad::Syntax::TokenKind::Builtin) {
                        commandContext = SOURCE.mid(token.startOffset, token.length);
                        commandContextBuiltin = token.kind == ShellScratchpad::Syntax::TokenKind::Builtin;
                    }
                }
                const QString tokenText = SOURCE.mid(clickedToken->startOffset, clickedToken->length);
                QString symbolName = tokenText;
                if (clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Variable) {
                    static const QRegularExpression VARIABLE_NAME(QStringLiteral(R"(^\$\{?([A-Za-z_][A-Za-z0-9_]*))"));
                    const auto MATCH = VARIABLE_NAME.match(tokenText);
                    symbolName = MATCH.hasMatch() ? MATCH.captured(1) : QString();
                }
                bool hasContextAction = false;
                const bool SYMBOL_TOKEN = clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Variable ||
                    clickedToken->kind == ShellScratchpad::Syntax::TokenKind::AssignmentName ||
                    clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Command ||
                    clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Builtin ||
                    clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Argument ||
                    clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Word;
                const QJsonObject PROFILE = selectedRunProfile();
                const QJsonObject PROFILE_OVERRIDES = PROFILE.value(QStringLiteral("environment")).toObject();
                if (SYMBOL_TOKEN && !symbolName.isEmpty() && PROFILE_OVERRIDES.contains(symbolName)) {
                    QAction *profileDeclaration = contextMenu.addAction(
                        QStringLiteral("Go to execution profile declaration of %1").arg(symbolName));
                    const QString PROFILE_NAME = PROFILE.value(QStringLiteral("name")).toString();
                    connect(profileDeclaration, &QAction::triggered, this,
                            [this, PROFILE_NAME, symbolName]() { manageRunProfiles(PROFILE_NAME, symbolName); });
                    hasContextAction = true;
                } else if (SYMBOL_TOKEN && completionController && !symbolName.isEmpty()) {
                    const auto &SYMBOLS = completionController->scriptSymbols();
                    const auto SYMBOL = std::find_if(SYMBOLS.cbegin(), SYMBOLS.cend(),
                        [&symbolName](const auto &entry) { return entry.name == symbolName; });
                    if (SYMBOL != SYMBOLS.cend()) {
                        QAction *declarationAction = contextMenu.addAction(
                            QStringLiteral("Go to declaration of %1").arg(symbolName));
                        const qsizetype DECLARATION_OFFSET = SYMBOL->declarationOffset;
                        const qsizetype DECLARATION_LENGTH = symbolName.size();
                        connect(declarationAction, &QAction::triggered, this,
                                [this, DECLARATION_OFFSET, DECLARATION_LENGTH]() {
                            scriptEdit->revealPosition(DECLARATION_OFFSET);
                            QTextCursor declaration(scriptEdit->document());
                            declaration.setPosition(static_cast<int>(DECLARATION_OFFSET));
                            declaration.setPosition(static_cast<int>(DECLARATION_OFFSET + DECLARATION_LENGTH),
                                                    QTextCursor::KeepAnchor);
                            scriptEdit->setTextCursor(declaration);
                            scriptEdit->centerCursor();
                            scriptEdit->setFocus();
                        });
                        hasContextAction = true;
                    }
                }
                if (!commandContext.isEmpty()) {
                    const bool DOCS_ELIGIBLE = clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Command ||
                        clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Builtin ||
                        clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Option ||
                        clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Argument ||
                        clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Path ||
                        clickedToken->kind == ShellScratchpad::Syntax::TokenKind::Number;
                    if (DOCS_ELIGIBLE) {
                        const QString ACTION_LABEL = commandContextBuiltin
                            ? QStringLiteral("Open Bash manual for %1").arg(commandContext)
                            : QStringLiteral("Open man page for %1").arg(commandContext);
                        QAction *manPage = contextMenu.addAction(ACTION_LABEL);
                        const QString SEARCH_TERM = commandContextBuiltin ? commandContext : tokenText;
                        connect(manPage, &QAction::triggered, this,
                                [this, commandContext, commandContextBuiltin, SEARCH_TERM]() {
                            openManPage(commandContext, commandContextBuiltin, SEARCH_TERM);
                        });
                        hasContextAction = true;
                    }
                }
                if (hasContextAction)
                    contextMenu.addSeparator();
            }

            const QTextBlock RIGHT_CLICKED_BLOCK = scriptEdit->cursorForPosition(position).block();
            const int RIGHT_CLICKED_LINE = RIGHT_CLICKED_BLOCK.blockNumber() + 1;
            QStringList lineErrors;
            for (int row = 0; row < diagnosticsList->count(); ++row) {
                const QListWidgetItem *diagnostic = diagnosticsList->item(row);
                if (diagnostic->data(Qt::UserRole).toInt() != RIGHT_CLICKED_LINE)
                    continue;
                lineErrors.append(QStringLiteral("%1 SC%2 at column %3: %4")
                    .arg(diagnostic->data(Qt::UserRole + 3).toString().toUpper())
                    .arg(diagnostic->data(Qt::UserRole + 1).toInt(), 4, 10, QLatin1Char('0'))
                    .arg(diagnostic->data(Qt::UserRole + 2).toInt())
                    .arg(diagnostic->data(Qt::UserRole + 4).toString()));
            }
            if (!lineErrors.isEmpty()) {
                QAction *copyLineAndErrors = contextMenu.addAction(QStringLiteral("Copy Line and Diagnostics"));
                const QString LINE_TEXT = RIGHT_CLICKED_BLOCK.text();
                connect(copyLineAndErrors, &QAction::triggered, this, [RIGHT_CLICKED_LINE, LINE_TEXT, lineErrors]() {
                    QApplication::clipboard()->setText(QStringLiteral("Line %1: %2\nDiagnostics:\n%3")
                        .arg(RIGHT_CLICKED_LINE).arg(LINE_TEXT, lineErrors.join(QLatin1Char('\n'))));
                });
                contextMenu.addSeparator();
            }

            QAction *paste = contextMenu.addAction(QStringLiteral("Paste"));
            paste->setEnabled(QApplication::clipboard()->mimeData()->hasText());
            connect(paste, &QAction::triggered, scriptEdit, &QPlainTextEdit::paste);

            QAction *execute = contextMenu.addAction(QStringLiteral("Execute"));
            connect(execute, &QAction::triggered, this, [this]() { runScript(); });

            if (HAS_SELECTION) {
                contextMenu.addSeparator();
                QAction *executeSelection = contextMenu.addAction(QStringLiteral("Execute Selection"));
                connect(executeSelection, &QAction::triggered, this, [this]() { runScript(true); });
                QAction *copy = contextMenu.addAction(QStringLiteral("Copy"));
                connect(copy, &QAction::triggered, scriptEdit, &QPlainTextEdit::copy);
                QAction *createSnippet = contextMenu.addAction(QStringLiteral("Create Snippet"));
                connect(createSnippet, &QAction::triggered, this, [this]() {
                    QString code = scriptEdit->textCursor().selectedText();
                    code.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
                    ShellScratchpad::Snippets::SnippetCreateDialog dialog(code, this);
                    if (dialog.exec() != QDialog::Accepted)
                        return;
                    if (completionController)
                        completionController->refreshSnippets();
                    statusBar()->showMessage(QStringLiteral("Snippet saved"), 3000);
                });
            }

            contextMenu.addSeparator();
            QAction *undo = contextMenu.addAction(QStringLiteral("Undo"));
            undo->setEnabled(scriptEdit->document()->isUndoAvailable());
            connect(undo, &QAction::triggered, scriptEdit, &QPlainTextEdit::undo);
            QAction *redo = contextMenu.addAction(QStringLiteral("Redo"));
            redo->setEnabled(scriptEdit->document()->isRedoAvailable());
            connect(redo, &QAction::triggered, scriptEdit, &QPlainTextEdit::redo);
            QAction *selectAll = contextMenu.addAction(QStringLiteral("Select All"));
            connect(selectAll, &QAction::triggered, scriptEdit, &QPlainTextEdit::selectAll);

            contextMenu.exec(scriptEdit->viewport()->mapToGlobal(position));
        });
        connect(stopButton, &QPushButton::clicked, this, [this]() { stopScript(); });
        connect(outputFindEdit, &QLineEdit::textChanged, this, [this]() { refreshOutputView(); });
        connect(outputFindCaseSensitiveAction, &QAction::toggled, this, [this]() { refreshOutputView(); });
        scriptEdit->installEventFilter(this);

        connect(&process, &QProcess::readyReadStandardOutput, this, [this]() {
            processOutputAvailable();
        });
        connect(&process, &QProcess::started, this, [this]() { closeDebuggerChildSocket(); });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
            processOutputAvailable();
            readDebuggerSocket();
            readRunHistoryPipe();
            closeRunHistoryPipe();
            closeDebuggerSocket();
            if (!pendingOutput.isEmpty()) {
                appendOutput(QString::fromLocal8Bit(pendingOutput));
                pendingOutput.clear();
            }

            promptTimer.stop();
            killTimer.stop();
            runButton->setEnabled(true);
            stopButton->setEnabled(false);
            directoryEdit->setEnabled(true);
            scriptEdit->setReadOnly(false);
            scriptEdit->setDebugLine(0);
            debuggerEnabled = false;
            debuggerPaused = false;
            debuggerVariables.clear();
            pendingImmediateExpression.clear();
            debuggerBanner->hide();
            variablesPanel->hide();
            immediatePanel->hide();
            if (completionController)
                completionController->setRuntimeVariables({});
            updateDebuggerActions();
            if (toggleBreakpointAction)
                toggleBreakpointAction->setEnabled(true);
            closeScriptFileDescriptor();

            const QString DURATION = QStringLiteral("%1 s").arg(runElapsed.elapsed() / 1000.0, 0, 'f', 1);
            runDurationLabel->setText(DURATION);
            if (stopRequested) {
                runStatusLabel->setText(QStringLiteral("Stopped"));
                statusBar()->showMessage(QStringLiteral("Script stopped"));
            } else if (exitStatus == QProcess::NormalExit) {
                runStatusLabel->setText(exitCode == 0 ? QStringLiteral("Succeeded")
                                                       : QStringLiteral("Failed (%1)").arg(exitCode));
                statusBar()->showMessage(exitCode == 0 ? QStringLiteral("Script completed successfully")
                                                        : QStringLiteral("Script failed with exit code %1").arg(exitCode));
            } else {
                runStatusLabel->setText(QStringLiteral("Terminated"));
                statusBar()->showMessage(QStringLiteral("Script process terminated"));
            }
            stopRequested = false;
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                promptTimer.stop();
                killTimer.stop();
                runButton->setEnabled(true);
                stopButton->setEnabled(false);
                directoryEdit->setEnabled(true);
                closeScriptFileDescriptor();
                closeRunHistoryPipe();
                closeDebuggerSocket();
                debuggerEnabled = false;
                debuggerPaused = false;
                pendingImmediateExpression.clear();
                debuggerBanner->hide();
                variablesPanel->hide();
                immediatePanel->hide();
                scriptEdit->setReadOnly(false);
                scriptEdit->setDebugLine(0);
                updateDebuggerActions();
                if (toggleBreakpointAction)
                    toggleBreakpointAction->setEnabled(true);
                runStatusLabel->setText(QStringLiteral("Could not start"));
                statusBar()->showMessage(QStringLiteral("Could not start script"));
            }
        });
        connect(&killTimer, &QTimer::timeout, this, [this]() {
            if (process.state() != QProcess::NotRunning) {
                killProcessGroup(SIGKILL);
                process.kill();
            }
        });
        promptTimer.setSingleShot(true);
        promptTimer.setInterval(700);
        connect(&promptTimer, &QTimer::timeout, this, [this]() { detectExternalCommandPrompt(); });

        setStyleSheet(QStringLiteral(
            "QPushButton { padding: 7px 13px; }"
            "QPushButton#runButton { font-weight: 600; }"
            "QPlainTextEdit#scriptEdit { "
            "border: 1px solid palette(mid); border-radius: 6px; padding: 7px; "
            "background-color: #2b2b2b; color: #e5e7eb; "
            "selection-background-color: #46566b; }"
            "QPlainTextEdit#scriptEdit:focus { border: 1px solid palette(highlight); }"
            "QPlainTextEdit#outputEdit { "
            "border: 1px solid palette(mid); border-radius: 6px; padding: 6px; "
            "background: palette(base); }"));
        placeCaretAtEndOfFirstLine();
    }

    ~ShellScratchpadWindow() override
    {
        outputFilterTimer.stop();
        outputFilterGeneration->fetch_add(1, std::memory_order_relaxed);
        shellCheckTimer.stop();
        latestDraftTimer.stop();
        shellCheckProcess.kill();
        closeRunHistoryPipe();
        closeDebuggerSocket();
        openScratchpadWindows().removeAll(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == outputFindEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                outputFindEdit->clear();
                outputSearchRow->hide();
                outputEdit->setFocus();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                navigateOutputMatches(keyEvent->modifiers().testFlag(Qt::ShiftModifier));
                return true;
            }
        }
        if (watched == scriptFindEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                scriptSearchRow->hide();
                scriptEdit->setFocus();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                findInScript(keyEvent->modifiers().testFlag(Qt::ShiftModifier));
                return true;
            }
        }
        if (watched == scriptEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const Qt::KeyboardModifiers MODIFIERS = keyEvent->modifiers();
            if (keyEvent->key() == Qt::Key_F9 && MODIFIERS == Qt::NoModifier) {
                if (process.state() == QProcess::NotRunning)
                    scriptEdit->toggleBreakpoint(scriptEdit->textCursor().blockNumber() + 1);
                return true;
            }
            if (debuggerPaused && keyEvent->key() == Qt::Key_F10 && MODIFIERS == Qt::NoModifier) {
                sendDebuggerCommand('s');
                return true;
            }
            if (debuggerPaused && keyEvent->key() == Qt::Key_F11) {
                if (MODIFIERS == Qt::NoModifier)
                    sendDebuggerCommand('i');
                else if (MODIFIERS == Qt::ShiftModifier)
                    sendDebuggerCommand('o');
                else
                    return QMainWindow::eventFilter(watched, event);
                return true;
            }
            const bool IS_ENTER = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
            if (IS_ENTER && (MODIFIERS == Qt::ControlModifier || MODIFIERS == Qt::ShiftModifier)) {
                QTextCursor cursor = scriptEdit->textCursor();
                const bool INSERT_ABOVE = MODIFIERS == Qt::ControlModifier;
                cursor.movePosition(INSERT_ABOVE ? QTextCursor::StartOfBlock : QTextCursor::EndOfBlock);
                const QString CURRENT_LINE = cursor.block().text();
                qsizetype indentationLength = 0;
                while (indentationLength < CURRENT_LINE.size() &&
                       (CURRENT_LINE.at(indentationLength) == QLatin1Char(' ') ||
                        CURRENT_LINE.at(indentationLength) == QLatin1Char('\t')))
                    ++indentationLength;
                const QString INDENTATION = CURRENT_LINE.left(indentationLength);
                cursor.insertBlock();
                if (INSERT_ABOVE) {
                    const QTextBlock NEW_BLOCK = cursor.block().previous();
                    QTextCursor newLineCursor(NEW_BLOCK);
                    newLineCursor.insertText(INDENTATION);
                    scriptEdit->setTextCursor(newLineCursor);
                } else {
                    cursor.insertText(INDENTATION);
                    scriptEdit->setTextCursor(cursor);
                }
                return true;
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent *event) override
    {
        if (process.state() != QProcess::NotRunning) {
            const auto ANSWER = QMessageBox::question(
                this, QStringLiteral("Script is still running"),
                QStringLiteral("A script is still running. Stop it and close the scratchpad?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ANSWER != QMessageBox::Yes) {
                event->ignore();
                return;
            }
            terminateProcessGroup();
            process.kill();
            process.waitForFinished(1500);
        }
        if (!confirmReplaceDocument()) {
            event->ignore();
            return;
        }
        recoveryTimer.stop();
        latestDraftTimer.stop();
        writeLatestDraft();
        clearRecoveryDraft();
        closeScriptFileDescriptor();
        event->accept();
    }

private:
    ShellScratchpad::CodeEditor *scriptEdit = nullptr;
    QWidget *scriptSearchRow = nullptr;
    QLineEdit *scriptFindEdit = nullptr;
    QAction *scriptFindCaseSensitiveAction = nullptr;
    QPlainTextEdit *outputEdit = nullptr;
    QLineEdit *outputFindEdit = nullptr;
    QWidget *outputSearchRow = nullptr;
    QAction *outputFindCaseSensitiveAction = nullptr;
    QWidget *outputPanel = nullptr;
    QSplitter *splitter = nullptr;
    QSplitter *scriptSideSplitter = nullptr;
    QSplitter *outputSideSplitter = nullptr;
    QWidget *variablesPanel = nullptr;
    QLabel *debuggerBanner = nullptr;
    QLineEdit *variablesFilter = nullptr;
    QTreeWidget *variablesTree = nullptr;
    QWidget *immediatePanel = nullptr;
    QPlainTextEdit *immediateOutputEdit = nullptr;
    QLineEdit *immediateInputEdit = nullptr;
    QPushButton *immediateRunButton = nullptr;
    QLineEdit *directoryEdit = nullptr;
    QString defaultWorkingDirectory;
    QComboBox *runProfileCombo = nullptr;
    QAction *clearOutputEachRunAction = nullptr;
    QAction *guiSudoPromptsAction = nullptr;
    QAction *reuseSudoPasswordAction = nullptr;
    QAction *debugContinueAction = nullptr;
    QAction *debugStepOverAction = nullptr;
    QAction *debugStepInAction = nullptr;
    QAction *debugStepOutAction = nullptr;
    QAction *toggleBreakpointAction = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *stopButton = nullptr;
    QLabel *runStatusLabel = nullptr;
    QLabel *runDurationLabel = nullptr;
    QProcess process;
    QPointer<ShellScratchpad::Syntax::SyntaxDebugDialog> syntaxDebugDialog;
    QPointer<ShellScratchpad::Completion::CompletionController> completionController;
    QPointer<QAction> showOutputAction;
    KeyChordDispatcher *keyChordDispatcher = nullptr;
    QTimer killTimer;
    QTimer promptTimer;
    QByteArray pendingOutput;
    QString outputContent;
    QElapsedTimer runElapsed;
    OutputMatchHighlighter *outputMatchHighlighter = nullptr;
    qsizetype outputFindNavigationPosition = 0;
    QThreadPool outputFilterPool;
    QTimer outputFilterTimer;
    std::shared_ptr<std::atomic<quint64>> outputFilterGeneration =
        std::make_shared<std::atomic<quint64>>(0);
    int scriptFileDescriptor = -1;
    bool promptDialogOpen = false;
    bool stopRequested = false;
    QString currentScriptPath;
    QTimer recoveryTimer;
    QTimer latestDraftTimer;
    QTimer shellCheckTimer;
    QProcess shellCheckProcess;
    QPointer<QDialog> runHistoryDialog;
    QListWidget *runHistoryList = nullptr;
    QSocketNotifier *runHistoryNotifier = nullptr;
    QSocketNotifier *debuggerNotifier = nullptr;
    int debuggerSocketDescriptor = -1;
    int debuggerChildSocketDescriptor = -1;
    QByteArray debuggerSocketBuffer;
    bool debuggerEnabled = false;
    bool debuggerPaused = false;
    int debuggerCurrentDepth = 0;
    QHash<QString, QString> debuggerVariables;
    QString pendingImmediateExpression;
    QStringList runHistoryEntries;
    QList<int> runHistorySourceLines;
    QByteArray runHistoryBuffer;
    int runHistoryReadDescriptor = -1;
    int runHistoryWriteDescriptor = -1;
    bool runHistoryTruncated = false;
    QDockWidget *diagnosticsDock = nullptr;
    QListWidget *diagnosticsList = nullptr;
    QString activeShellCheckScript;
    quint64 shellCheckGeneration = 0;
    quint64 activeShellCheckGeneration = 0;
    bool shellCheckPending = false;
    bool shellCheckCurrent = false;
    bool runAfterShellCheck = false;
    bool runSelectionAfterShellCheck = false;
    bool runForceDebugAfterShellCheck = false;
    bool applyingScriptRunProfile = false;
    int currentHistoryLineOffset = 0;
    QString latestDraftSnapshot;
    bool recoveryEnabled = false;
    bool recoveryDraftOwned = false;
    int zoomLevel = 0;

    void openManPage(const QString &command, bool isShellBuiltin, const QString &searchTerm)
    {
        const QString MAN_PROGRAM = QStandardPaths::findExecutable(QStringLiteral("man"));
        if (MAN_PROGRAM.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Manual pages unavailable"),
                QStringLiteral("The 'man' program is not installed or could not be found."));
            return;
        }
        const QString TOPIC = isShellBuiltin ? QStringLiteral("bash") : QFileInfo(command).fileName();
        if (TOPIC.isEmpty())
            return;
        auto *manProcess = new QProcess(this);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MANPAGER"), QStringLiteral("cat"));
        environment.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
        environment.insert(QStringLiteral("GROFF_NO_SGR"), QStringLiteral("1"));
        environment.insert(QStringLiteral("MANWIDTH"), QStringLiteral("100"));
        manProcess->setProcessEnvironment(environment);
        connect(manProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, manProcess, command, isShellBuiltin, searchTerm, TOPIC](int exitCode, QProcess::ExitStatus) {
            const QString OUTPUT = QString::fromLocal8Bit(manProcess->readAllStandardOutput()).replace(QLatin1Char('\r'), QLatin1Char('\n'));
            const QString ERROR = QString::fromLocal8Bit(manProcess->readAllStandardError()).trimmed();
            manProcess->deleteLater();
            if (exitCode != 0 || OUTPUT.trimmed().isEmpty()) {
                const QString MESSAGE = ERROR.isEmpty()
                    ? QStringLiteral("No manual page was found for %1.").arg(command)
                    : ERROR;
                QMessageBox::information(this, QStringLiteral("Manual page unavailable"), MESSAGE);
                return;
            }
            QDialog dialog(this);
            dialog.setWindowTitle(isShellBuiltin
                ? QStringLiteral("Bash manual — %1").arg(command)
                : QStringLiteral("Man page — %1").arg(TOPIC));
            dialog.resize(860, 640);
            auto *layout = new QVBoxLayout(&dialog);
            auto *viewer = new QPlainTextEdit(&dialog);
            viewer->setReadOnly(true);
            viewer->setLineWrapMode(QPlainTextEdit::NoWrap);
            viewer->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            viewer->setPlainText(OUTPUT);
            if (!searchTerm.isEmpty()) {
                QTextCursor cursor = viewer->document()->find(searchTerm, 0, QTextDocument::FindWholeWords);
                if (!cursor.isNull()) {
                    viewer->setTextCursor(cursor);
                    viewer->centerCursor();
                }
            }
            layout->addWidget(viewer, 1);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog.exec();
        });
        connect(manProcess, &QProcess::errorOccurred, this,
                [this, manProcess](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            const QString REASON = manProcess->errorString();
            manProcess->deleteLater();
            QMessageBox::warning(this, QStringLiteral("Could not open manual page"), REASON);
        });
        manProcess->start(MAN_PROGRAM, {QStringLiteral("-P"), QStringLiteral("cat"),
                                        QStringLiteral("--"), TOPIC});
    }

    void createMenus()
    {
        QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        QAction *newScriptAction = fileMenu->addAction(QStringLiteral("New Script"));
        newScriptAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
        connect(newScriptAction, &QAction::triggered, this, [this]() { createNewScript(); });

        QAction *newWindowAction = new QAction(QStringLiteral("New Window"), this);
        newWindowAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
        connect(newWindowAction, &QAction::triggered, this, [this]() {
            auto *window = new ShellScratchpadWindow(expandedWorkingDirectory());
            window->show();
            window->raise();
            window->activateWindow();
        });
        fileMenu->addAction(newWindowAction);

        QAction *openAction = fileMenu->addAction(QStringLiteral("Open Script…"));
        openAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
        connect(openAction, &QAction::triggered, this, [this]() { openScript(); });
        QAction *saveAction = fileMenu->addAction(QStringLiteral("Save Script…"));
        saveAction->setShortcut(QKeySequence::Save);
        connect(saveAction, &QAction::triggered, this, [this]() {
            if (!currentScriptPath.isEmpty())
                saveScript();
        });
        fileMenu->addSeparator();
        QAction *closeAction = fileMenu->addAction(QStringLiteral("Close Window"));
        closeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
        connect(closeAction, &QAction::triggered, this, &QWidget::close);

        QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        QAction *undoAction = editMenu->addAction(QStringLiteral("Undo"));
        undoAction->setShortcut(QKeySequence::Undo);
        connect(undoAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::undo);
        QAction *redoAction = editMenu->addAction(QStringLiteral("Redo"));
        redoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Y")));
        connect(redoAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::redo);
        editMenu->addSeparator();
        QAction *cutAction = editMenu->addAction(QStringLiteral("Cut"));
        cutAction->setShortcut(QKeySequence::Cut);
        connect(cutAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::cut);
        QAction *copyAction = editMenu->addAction(QStringLiteral("Copy"));
        copyAction->setShortcut(QKeySequence::Copy);
        connect(copyAction, &QAction::triggered, this, [this]() {
            if (outputFindEdit->hasFocus()) {
                outputFindEdit->copy();
                return;
            }
            if (outputEdit->hasFocus()) {
                copyOutput();
                return;
            }
            scriptEdit->copy();
        });
        QAction *pasteAction = editMenu->addAction(QStringLiteral("Paste"));
        pasteAction->setShortcut(QKeySequence::Paste);
        connect(pasteAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::paste);
        QAction *selectAllAction = editMenu->addAction(QStringLiteral("Select All"));
        selectAllAction->setShortcut(QKeySequence::SelectAll);
        connect(selectAllAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::selectAll);
        editMenu->addSeparator();
        QAction *findAction = editMenu->addAction(QStringLiteral("Find…"));
        findAction->setShortcut(QKeySequence::Find);
        connect(findAction, &QAction::triggered, this, [this]() {
            if (outputEdit->hasFocus() || outputFindEdit->hasFocus()) {
                outputSearchRow->show();
                outputFindEdit->setFocus();
                outputFindEdit->selectAll();
            } else {
                showScriptFind();
            }
        });

        QMenu *scriptMenu = menuBar()->addMenu(QStringLiteral("&Script"));
        QAction *runAction = scriptMenu->addAction(QStringLiteral("Run Script"));
        runAction->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
        connect(runAction, &QAction::triggered, this, [this]() { runScript(); });
        QAction *runSelectionAction = scriptMenu->addAction(QStringLiteral("Run Selection"));
        runSelectionAction->setEnabled(scriptEdit->textCursor().hasSelection());
        connect(scriptEdit, &QPlainTextEdit::copyAvailable, runSelectionAction, &QAction::setEnabled);
        connect(runSelectionAction, &QAction::triggered, this, [this]() { runScript(true); });
        scriptMenu->addSeparator();
        QAction *debugStartAction = scriptMenu->addAction(QStringLiteral("Start Debugging"));
        connect(debugStartAction, &QAction::triggered, this, [this]() { runScript(false, true); });
        toggleBreakpointAction = scriptMenu->addAction(QStringLiteral("Toggle Breakpoint"));
        toggleBreakpointAction->setShortcut(QKeySequence(Qt::Key_F9));
        connect(toggleBreakpointAction, &QAction::triggered, this, [this]() {
            if (process.state() != QProcess::NotRunning)
                return;
            scriptEdit->toggleBreakpoint(scriptEdit->textCursor().blockNumber() + 1);
        });
        debugContinueAction = scriptMenu->addAction(QStringLiteral("Continue Debugging"));
        debugContinueAction->setShortcut(QKeySequence(Qt::Key_F5));
        connect(debugContinueAction, &QAction::triggered, this, [this]() { sendDebuggerCommand('c'); });
        debugStepOverAction = scriptMenu->addAction(QStringLiteral("Step Over"));
        debugStepOverAction->setShortcut(QKeySequence(Qt::Key_F10));
        connect(debugStepOverAction, &QAction::triggered, this, [this]() { sendDebuggerCommand('s'); });
        debugStepInAction = scriptMenu->addAction(QStringLiteral("Step Into"));
        debugStepInAction->setShortcut(QKeySequence(Qt::Key_F11));
        connect(debugStepInAction, &QAction::triggered, this, [this]() { sendDebuggerCommand('i'); });
        debugStepOutAction = scriptMenu->addAction(QStringLiteral("Step Out"));
        debugStepOutAction->setShortcut(QKeySequence(QStringLiteral("Shift+F11")));
        connect(debugStepOutAction, &QAction::triggered, this, [this]() { sendDebuggerCommand('o'); });
        updateDebuggerActions();
        QAction *stopAction = scriptMenu->addAction(QStringLiteral("Stop Script"));
        stopAction->setShortcut(QKeySequence(QStringLiteral("Shift+Alt+Return")));
        connect(stopAction, &QAction::triggered, this, [this]() { stopScript(); });
        scriptMenu->addSeparator();
        guiSudoPromptsAction = scriptMenu->addAction(QStringLiteral("GUI Sudo Prompts"));
        guiSudoPromptsAction->setCheckable(true);
        guiSudoPromptsAction->setChecked(true);
        guiSudoPromptsAction->setToolTip(QStringLiteral("Show a masked dialog when the script needs sudo authentication."));
        reuseSudoPasswordAction = scriptMenu->addAction(QStringLiteral("Reuse sudo password for this run"));
        reuseSudoPasswordAction->setCheckable(true);
        reuseSudoPasswordAction->setChecked(true);
        reuseSudoPasswordAction->setToolTip(QStringLiteral("Keep the password only in the running script process so sudo is prompted once per run."));
        connect(guiSudoPromptsAction, &QAction::toggled, reuseSudoPasswordAction, &QAction::setEnabled);
        scriptMenu->addSeparator();
        clearOutputEachRunAction = scriptMenu->addAction(QStringLiteral("Clear output each run"));
        clearOutputEachRunAction->setCheckable(true);
        clearOutputEachRunAction->setChecked(true);
        QAction *clearOutputAction = scriptMenu->addAction(QStringLiteral("Clear Output"));
        connect(clearOutputAction, &QAction::triggered, this, [this]() {
            clearOutput();
        });
        QAction *clearScriptAction = scriptMenu->addAction(QStringLiteral("Clear Script"));
        connect(clearScriptAction, &QAction::triggered, this, [this]() { createNewScript(); });
        QAction *setWorkingDirectoryAction = scriptMenu->addAction(QStringLiteral("Set Working Directory…"));
        connect(setWorkingDirectoryAction, &QAction::triggered, this, [this]() {
            const QString DIRECTORY = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Choose working directory"), expandedWorkingDirectory());
            if (!DIRECTORY.isEmpty())
                directoryEdit->setText(QDir::cleanPath(DIRECTORY));
        });
        scriptMenu->addSeparator();
        QAction *quickSavedScriptsAction = scriptMenu->addAction(QStringLiteral("Quick Saved Scripts…"));
        quickSavedScriptsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
        connect(quickSavedScriptsAction, &QAction::triggered, this, [this]() { showQuickSavedScripts(); });
        QAction *saveQuickScriptAction = scriptMenu->addAction(QStringLiteral("Save Quick Script…"));
        connect(saveQuickScriptAction, &QAction::triggered, this, [this]() { saveQuickScript(); });

        QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        showOutputAction = viewMenu->addAction(QStringLiteral("Show/Hide Output"));
        showOutputAction->setCheckable(true);
        showOutputAction->setChecked(true);
        connect(showOutputAction, &QAction::toggled, outputPanel, &QWidget::setVisible);
        viewMenu->addSeparator();
        QAction *resetLayoutAction = viewMenu->addAction(QStringLiteral("Reset Layout"));
        connect(resetLayoutAction, &QAction::triggered, this, [this]() {
            outputPanel->show();
            showOutputAction->setChecked(true);
            const int HEIGHT = qMax(splitter->height(), 1);
            splitter->setSizes({HEIGHT * 3 / 5, HEIGHT * 2 / 5});
        });
        viewMenu->addSeparator();
        const QKeySequence COLLAPSE_ALL_SEQUENCE = QKeySequence::fromString(
            QStringLiteral("Ctrl+M, Ctrl+O"), QKeySequence::PortableText);
        QAction *collapseAllAction = viewMenu->addAction(
            QStringLiteral("Collapse All Sections\tCtrl+M, Ctrl+O"));
        connect(collapseAllAction, &QAction::triggered, scriptEdit, &ShellScratchpad::CodeEditor::collapseAllSections);
        keyChordDispatcher->registerChord(COLLAPSE_ALL_SEQUENCE, [this]() { scriptEdit->collapseAllSections(); });

        const QKeySequence EXPAND_ALL_SEQUENCE = QKeySequence::fromString(
            QStringLiteral("Ctrl+M, Ctrl+P"), QKeySequence::PortableText);
        QAction *expandAllAction = viewMenu->addAction(
            QStringLiteral("Expand All Sections\tCtrl+M, Ctrl+P"));
        connect(expandAllAction, &QAction::triggered, scriptEdit, &ShellScratchpad::CodeEditor::expandAllSections);
        keyChordDispatcher->registerChord(EXPAND_ALL_SEQUENCE, [this]() { scriptEdit->expandAllSections(); });
        viewMenu->addSeparator();
        QAction *zoomInAction = viewMenu->addAction(QStringLiteral("Zoom In"));
        zoomInAction->setShortcut(QKeySequence(QStringLiteral("Ctrl++")));
        connect(zoomInAction, &QAction::triggered, this, [this]() { changeZoom(1); });
        QAction *zoomOutAction = viewMenu->addAction(QStringLiteral("Zoom Out"));
        zoomOutAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
        connect(zoomOutAction, &QAction::triggered, this, [this]() { changeZoom(-1); });
        QAction *resetZoomAction = viewMenu->addAction(QStringLiteral("Reset Zoom"));
        resetZoomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
        connect(resetZoomAction, &QAction::triggered, this, [this]() { resetZoom(); });
        viewMenu->addSeparator();
        viewMenu->addAction(diagnosticsDock->toggleViewAction());

        QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
        QAction *snippetsAction = toolsMenu->addAction(QStringLiteral("Snippets…"));
        connect(snippetsAction, &QAction::triggered, this, [this]() {
            ShellScratchpad::Snippets::SnippetManagerDialog dialog(this);
            dialog.exec();
            if (completionController)
                completionController->refreshSnippets();
        });
        toolsMenu->addSeparator();
        QAction *codeStyleAction = toolsMenu->addAction(QStringLiteral("Code Style…"));
        connect(codeStyleAction, &QAction::triggered, this, [this]() {
            ShellScratchpad::Syntax::SyntaxColorDialog dialog(this);
            dialog.exec();
        });
        QAction *debuggerAction = toolsMenu->addAction(QStringLiteral("Syntax Debugger…"));
        connect(debuggerAction, &QAction::triggered, this, [this]() {
            if (!syntaxDebugDialog)
                syntaxDebugDialog = new ShellScratchpad::Syntax::SyntaxDebugDialog(scriptEdit, this);
            syntaxDebugDialog->show();
            syntaxDebugDialog->raise();
            syntaxDebugDialog->activateWindow();
        });
        QAction *refreshCommandCacheAction = toolsMenu->addAction(QStringLiteral("Refresh Command Cache"));
        connect(refreshCommandCacheAction, &QAction::triggered, this, [this]() {
            if (completionController)
                completionController->refreshCommandCache();
            statusBar()->showMessage(QStringLiteral("Command suggestions refreshed from PATH"), 3000);
        });
        QAction *runHistoryAction = toolsMenu->addAction(QStringLiteral("Run History…"));
        connect(runHistoryAction, &QAction::triggered, this, [this]() { showRunHistory(); });
        QAction *outputBookmarksAction = toolsMenu->addAction(QStringLiteral("Output Bookmarks…"));
        connect(outputBookmarksAction, &QAction::triggered, this, [this]() { showOutputBookmarks(); });

        QMenu *windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
        windowMenu->addAction(newWindowAction);
        QAction *nextWindowAction = windowMenu->addAction(QStringLiteral("Next Window"));
        connect(nextWindowAction, &QAction::triggered, this, [this]() { activateRelativeWindow(1); });
        QAction *previousWindowAction = windowMenu->addAction(QStringLiteral("Previous Window"));
        connect(previousWindowAction, &QAction::triggered, this, [this]() { activateRelativeWindow(-1); });
        QMenu *windowListMenu = windowMenu->addMenu(QStringLiteral("Window List"));
        connect(windowListMenu, &QMenu::aboutToShow, this, [this, windowListMenu]() {
            windowListMenu->clear();
            for (ShellScratchpadWindow *window : openScratchpadWindows()) {
                QAction *windowAction = windowListMenu->addAction(window->windowTitle());
                windowAction->setCheckable(true);
                windowAction->setChecked(window == this);
                connect(windowAction, &QAction::triggered, this, [window]() {
                    window->showNormal();
                    window->raise();
                    window->activateWindow();
                });
            }
        });

        QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        QAction *shortcutsAction = helpMenu->addAction(QStringLiteral("Keyboard Shortcuts"));
        connect(shortcutsAction, &QAction::triggered, this, [this]() { showKeyboardShortcuts(); });
        QAction *aboutAction = helpMenu->addAction(QStringLiteral("About Shell Scratchpad"));
        connect(aboutAction, &QAction::triggered, this, [this]() {
            QMessageBox::about(this, QStringLiteral("About Shell Scratchpad"),
                QStringLiteral("<h3>Shell Scratchpad</h3>"
                               "<p>Run one-off Bash scripts in a focused desktop scratchpad.</p>"
                               "<p><b>Company:</b> WinterRose<br><b>License:</b> MPL-2.0</p>"
                               "<p>Version %1</p>").arg(QCoreApplication::applicationVersion()));
        });
    }

    void createNewScript()
    {
        if (process.state() != QProcess::NotRunning || !confirmReplaceDocument(true))
            return;
        clearOutput();
        currentScriptPath.clear();
        scriptEdit->clear();
        scriptEdit->document()->setModified(false);
        recoveryTimer.stop();
        clearRecoveryDraft();
        updateWindowTitle();
        scriptEdit->setFocus();
    }

    void placeCaretAtEndOfFirstLine()
    {
        if (!scriptEdit || !scriptEdit->document()->firstBlock().isValid())
            return;
        QTextCursor cursor(scriptEdit->document()->firstBlock());
        cursor.movePosition(QTextCursor::EndOfBlock);
        scriptEdit->setTextCursor(cursor);
        scriptEdit->setFocus();
    }

    void openScript()
    {
        const QString PATH = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Bash Script"), expandedWorkingDirectory(),
            QStringLiteral("Bash scripts (*.sh);;All files (*)"));
        if (PATH.isEmpty())
            return;
        openScriptPath(PATH, true);
    }

    bool openScriptPath(const QString &path, bool confirmReplace)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("Could not open script"),
                                 QStringLiteral("Could not read the script:\n%1").arg(file.errorString()));
            return false;
        }
        const QString CONTENT = QString::fromUtf8(file.readAll());
        if (confirmReplace && !confirmReplaceDocument(true))
            return false;
        clearOutput();
        currentScriptPath = QFileInfo(path).absoluteFilePath();
        applyScriptRunProfile(currentScriptPath);
        scriptEdit->setPlainText(CONTENT);
        placeCaretAtEndOfFirstLine();
        scriptEdit->document()->setModified(false);
        recoveryTimer.stop();
        if (confirmReplace || recoveryDraftOwned)
            clearRecoveryDraft();
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 5000);
        return true;
    }

    void updateWindowTitle()
    {
        QString title = currentScriptPath.isEmpty()
            ? QStringLiteral("Untitled — Shell Scratchpad")
            : QStringLiteral("%1 — Shell Scratchpad").arg(QFileInfo(currentScriptPath).fileName());
        if (scriptEdit && scriptEdit->document()->isModified())
            title.prepend(QLatin1Char('*'));
        setWindowTitle(title);
    }

    bool confirmReplaceDocument(bool openingAnotherScript = false)
    {
        if (currentScriptPath.isEmpty())
            return true;
        if (!scriptEdit->document()->isModified()) {
            if (!openingAnotherScript)
                return true;
            return QMessageBox::question(this, QStringLiteral("Open Another Script?"),
                QStringLiteral("A saved script is currently open:\n%1\n\nOpen another script? You can reopen this file later.")
                    .arg(QFileInfo(currentScriptPath).fileName()),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
        }
        QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Unsaved changes"),
                           QStringLiteral("Save changes to %1 before continuing?")
                               .arg(QFileInfo(currentScriptPath).fileName()),
                           QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        dialog.setDefaultButton(QMessageBox::Save);
        const int choice = dialog.exec();
        if (choice == QMessageBox::Cancel)
            return false;
        if (choice == QMessageBox::Save) {
            saveScript();
            return !scriptEdit->document()->isModified();
        }
        clearRecoveryDraft();
        return true;
    }

    QString recoveryDraftPath() const
    {
        const QString DIR = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                .filePath(QStringLiteral("recovery"));
        QDir().mkpath(DIR);
        return QDir(DIR).filePath(QStringLiteral("recovery.json"));
    }

    QString quickSaveDirectory() const
    {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("quick-saves"));
    }

    QString latestDraftPath() const
    {
        return QDir(quickSaveDirectory()).filePath(QStringLiteral("latest-draft.sh"));
    }

    static QList<QPair<QString, QString>> builtInQuickScripts()
    {
        return {
            {QStringLiteral("System boot and previous-boot logs"), QString::fromUtf8(R"bash(#!/usr/bin/env bash
printf '%s\n' '=== System and boot summary ==='
date --iso-8601=seconds
uptime
printf '\n%s\n' '=== Current boot journal (last 150 entries) ==='
journalctl -b -n 150 --no-pager
printf '\n%s\n' '=== Previous boot journal (last 150 entries) ==='
journalctl -b -1 -n 150 --no-pager || true
)bash")},
            {QStringLiteral("GPU, Mesa, Vulkan and kernel information"), QString::fromUtf8(R"bash(#!/usr/bin/env bash
printf '%s\n' '=== Kernel and distribution ==='
uname -a
cat /etc/os-release 2>/dev/null || true
printf '\n%s\n' '=== Graphics devices ==='
lspci -nnk 2>/dev/null | sed -n '/VGA compatible controller\|3D controller\|Display controller/,+3p' || true
printf '\n%s\n' '=== Mesa / OpenGL ==='
glxinfo -B 2>/dev/null || echo 'glxinfo is unavailable'
printf '\n%s\n' '=== Vulkan ==='
vulkaninfo --summary 2>/dev/null || echo 'vulkaninfo is unavailable'
)bash")},
            {QStringLiteral("NVMe inventory and SMART health"), QString::fromUtf8(R"bash(#!/usr/bin/env bash
printf '%s\n' '=== Block devices ==='
lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,FSTYPE,MOUNTPOINTS
printf '\n%s\n' '=== NVMe devices ==='
if command -v nvme >/dev/null 2>&1; then nvme list; else echo 'nvme-cli is unavailable'; fi
printf '\n%s\n' '=== SMART health ==='
if command -v smartctl >/dev/null 2>&1; then
    while IFS= read -r device; do
        [[ -n "$device" ]] || continue
        printf '\n--- %s ---\n' "$device"
        smartctl -H "$device" 2>&1 || true
    done < <(lsblk -dnpo NAME,TYPE | awk '$2 == "disk" {print $1}')
else
    echo 'smartmontools is unavailable'
fi
)bash")},
            {QStringLiteral("Network and DNS diagnostics"), QString::fromUtf8(R"bash(#!/usr/bin/env bash
printf '%s\n' '=== Interfaces and addresses ==='
ip -brief address
printf '\n%s\n' '=== Routes ==='
ip route
printf '\n%s\n' '=== DNS resolver ==='
if command -v resolvectl >/dev/null 2>&1; then resolvectl status; else cat /etc/resolv.conf; fi
printf '\n%s\n' '=== DNS lookups ==='
for host in example.com github.com; do printf '\n--- %s ---\n' "$host"; getent ahosts "$host" || true; done
printf '\n%s\n' '=== Link state ==='
ip -brief link
)bash")},
            {QStringLiteral("Memory pressure and disk usage"), QString::fromUtf8(R"bash(#!/usr/bin/env bash
printf '%s\n' '=== Memory ==='
free -h
printf '\n%s\n' '=== Memory pressure ==='
cat /proc/pressure/memory 2>/dev/null || true
printf '\n%s\n' '=== Swap ==='
cat /proc/swaps
printf '\n%s\n' '=== Disk space ==='
df -hT
printf '\n%s\n' '=== Inode usage ==='
df -ih
)bash")}
        };
    }

    void showQuickSavedScripts()
    {
        writeLatestDraft();
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Quick Saved Scripts"));
        dialog.setMinimumSize(900, 560);
        auto *layout = new QVBoxLayout(&dialog);
        auto *splitter = new QSplitter(Qt::Horizontal, &dialog);
        auto *listsPanel = new QWidget(splitter);
        auto *listsLayout = new QVBoxLayout(listsPanel);
        auto *quickSavesHeading = new QLabel(QStringLiteral("Your Quick Saves"), &dialog);
        auto *quickSavesList = new QListWidget(listsPanel);
        auto *diagnosticsHeading = new QLabel(QStringLiteral("Built-in Diagnostics"), &dialog);
        auto *diagnosticsScriptsList = new QListWidget(listsPanel);
        listsLayout->addWidget(quickSavesHeading);
        listsLayout->addWidget(quickSavesList, 1);
        listsLayout->addWidget(diagnosticsHeading);
        listsLayout->addWidget(diagnosticsScriptsList, 1);
        auto *previewPanel = new QWidget(splitter);
        auto *previewLayout = new QVBoxLayout(previewPanel);
        previewLayout->addWidget(new QLabel(QStringLiteral("Script Preview"), previewPanel));
        auto *previewEditor = new ShellScratchpad::CodeEditor(previewPanel);
        previewEditor->setReadOnly(true);
        previewEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
        previewEditor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        previewEditor->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #25282d; color: #e5e7eb; }"));
        new ShellScratchpad::Syntax::SyntaxHighlighter(previewEditor);
        previewLayout->addWidget(previewEditor, 1);
        splitter->addWidget(listsPanel);
        splitter->addWidget(previewPanel);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        layout->addWidget(splitter, 1);

        const QString DRAFT_PATH = latestDraftPath();
        if (QFileInfo::exists(DRAFT_PATH)) {
            auto *item = new QListWidgetItem(QStringLiteral("Latest Draft"), quickSavesList);
            item->setData(Qt::UserRole, DRAFT_PATH);
            item->setData(Qt::UserRole + 2, QStringLiteral("draft"));
        }
        QDir().mkpath(quickSaveDirectory());
        const QFileInfoList SAVED_FILES = QDir(quickSaveDirectory()).entryInfoList(
            {QStringLiteral("*.sh")}, QDir::Files, QDir::Name);
        for (const QFileInfo &fileInfo : SAVED_FILES) {
            if (fileInfo.absoluteFilePath() == DRAFT_PATH)
                continue;
            auto *item = new QListWidgetItem(fileInfo.completeBaseName(), quickSavesList);
            item->setData(Qt::UserRole, fileInfo.absoluteFilePath());
            item->setData(Qt::UserRole + 2, QStringLiteral("file"));
        }
        for (const auto &script : builtInQuickScripts()) {
            auto *item = new QListWidgetItem(script.first, diagnosticsScriptsList);
            item->setData(Qt::UserRole + 1, script.second);
            item->setData(Qt::UserRole + 2, QStringLiteral("builtin"));
        }

        auto *buttons = new QDialogButtonBox(&dialog);
        QPushButton *openButton = buttons->addButton(QDialogButtonBox::Open);
        QPushButton *deleteButton = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
        QPushButton *closeButton = buttons->addButton(QDialogButtonBox::Close);
        deleteButton->setEnabled(false);
        layout->addWidget(buttons);

        auto updateDeleteButton = [&]() {
            QListWidgetItem *item = quickSavesList->currentItem();
            deleteButton->setEnabled(item && item->data(Qt::UserRole + 2).toString() == QLatin1String("file"));
        };
        auto openSelected = [&]() {
            QListWidgetItem *item = quickSavesList->currentItem();
            if (!item)
                item = diagnosticsScriptsList->currentItem();
            if (!item)
                return;
            const QString TYPE = item->data(Qt::UserRole + 2).toString();
            if (TYPE == QLatin1String("builtin")) {
                if (!confirmReplaceDocument(true))
                    return;
                clearOutput();
                currentScriptPath.clear();
                scriptEdit->setPlainText(item->data(Qt::UserRole + 1).toString());
                scriptEdit->document()->setModified(false);
                updateWindowTitle();
                statusBar()->showMessage(QStringLiteral("Loaded diagnostic script: %1").arg(item->text()), 4000);
                dialog.accept();
            } else if (TYPE == QLatin1String("draft")) {
                QFile file(item->data(Qt::UserRole).toString());
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || !confirmReplaceDocument(true))
                    return;
                clearOutput();
                currentScriptPath.clear();
                scriptEdit->setPlainText(QString::fromUtf8(file.readAll()));
                scriptEdit->document()->setModified(true);
                updateWindowTitle();
                statusBar()->showMessage(QStringLiteral("Loaded latest draft"), 4000);
                dialog.accept();
            } else if (TYPE == QLatin1String("file")) {
                const QString PATH = item->data(Qt::UserRole).toString();
                if (openScriptPath(PATH, true))
                    dialog.accept();
            }
        };
        connect(openButton, &QPushButton::clicked, &dialog, openSelected);
        connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(deleteButton, &QPushButton::clicked, &dialog, [&]() {
            QListWidgetItem *item = quickSavesList->currentItem();
            if (!item || item->data(Qt::UserRole + 2).toString() != QLatin1String("file"))
                return;
            const QString PATH = item->data(Qt::UserRole).toString();
            if (QMessageBox::question(&dialog, QStringLiteral("Delete Quick Script?"),
                    QStringLiteral("Delete ‘%1’ from your quick saves?").arg(item->text()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            if (!QFile::remove(PATH)) {
                QMessageBox::warning(&dialog, QStringLiteral("Could not delete Quick Script"),
                                     QStringLiteral("The saved script could not be deleted."));
                return;
            }
            delete quickSavesList->takeItem(quickSavesList->row(item));
            updateDeleteButton();
        });
        connect(quickSavesList, &QListWidget::currentItemChanged, &dialog,
                [&](QListWidgetItem *current, QListWidgetItem *) {
            if (current)
                diagnosticsScriptsList->setCurrentItem(nullptr);
            updateDeleteButton();
        });
        connect(diagnosticsScriptsList, &QListWidget::currentItemChanged, &dialog,
                [&](QListWidgetItem *current, QListWidgetItem *) {
            if (current)
                quickSavesList->setCurrentItem(nullptr);
            updateDeleteButton();
        });
        const auto updatePreview = [&]() {
            QListWidgetItem *item = quickSavesList->currentItem();
            if (!item)
                item = diagnosticsScriptsList->currentItem();
            if (!item) {
                previewEditor->clear();
                return;
            }
            const QString TYPE = item->data(Qt::UserRole + 2).toString();
            if (TYPE == QLatin1String("builtin")) {
                previewEditor->setPlainText(item->data(Qt::UserRole + 1).toString());
                return;
            }
            QFile file(item->data(Qt::UserRole).toString());
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                previewEditor->setPlainText(QStringLiteral("Could not read this script."));
                return;
            }
            previewEditor->setPlainText(QString::fromUtf8(file.readAll()));
        };
        connect(quickSavesList, &QListWidget::currentItemChanged, &dialog,
                [updatePreview](QListWidgetItem *, QListWidgetItem *) { updatePreview(); });
        connect(diagnosticsScriptsList, &QListWidget::currentItemChanged, &dialog,
                [updatePreview](QListWidgetItem *, QListWidgetItem *) { updatePreview(); });
        connect(quickSavesList, &QListWidget::itemDoubleClicked, &dialog,
                [&](QListWidgetItem *) { openSelected(); });
        connect(diagnosticsScriptsList, &QListWidget::itemDoubleClicked, &dialog,
                [&](QListWidgetItem *) { openSelected(); });
        if (quickSavesList->count() > 0)
            quickSavesList->setCurrentRow(0);
        else if (diagnosticsScriptsList->count() > 0)
            diagnosticsScriptsList->setCurrentRow(0);
        updateDeleteButton();
        dialog.exec();
    }

    void saveQuickScript()
    {
        bool accepted = false;
        const QString TITLE = QInputDialog::getText(this, QStringLiteral("Save Quick Script"),
            QStringLiteral("Script title:"), QLineEdit::Normal, QString(), &accepted).trimmed();
        if (!accepted || TITLE.isEmpty())
            return;
        QString fileName = TITLE;
        fileName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
        if (fileName.isEmpty())
            return;
        const QString PATH = QDir(quickSaveDirectory()).filePath(fileName + QStringLiteral(".sh"));
        if (QFileInfo::exists(PATH) && QMessageBox::question(this, QStringLiteral("Replace Quick Script?"),
                QStringLiteral("A quick script named ‘%1’ already exists. Replace it?").arg(TITLE),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        QDir().mkpath(quickSaveDirectory());
        QSaveFile file(PATH);
        const QByteArray CONTENT = scriptEdit->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(CONTENT) != CONTENT.size() || !file.commit()) {
            QMessageBox::warning(this, QStringLiteral("Could not save Quick Script"),
                                 QStringLiteral("The script could not be saved to the quick-save list."));
            return;
        }
        statusBar()->showMessage(QStringLiteral("Saved Quick Script: %1").arg(TITLE), 4000);
    }

    void writeLatestDraft()
    {
        if (!scriptEdit->document()->isModified()) {
            if (!latestDraftSnapshot.isEmpty()) {
                QFile existing(latestDraftPath());
                if (existing.open(QIODevice::ReadOnly) && existing.readAll() == latestDraftSnapshot.toUtf8())
                    QFile::remove(latestDraftPath());
                latestDraftSnapshot.clear();
            }
            return;
        }
        QDir().mkpath(quickSaveDirectory());
        QSaveFile file(latestDraftPath());
        const QByteArray CONTENT = scriptEdit->toPlainText().toUtf8();
        if (file.open(QIODevice::WriteOnly) && file.write(CONTENT) == CONTENT.size() && file.commit())
            latestDraftSnapshot = QString::fromUtf8(CONTENT);
    }

    void startShellCheck()
    {
        const QString EXECUTABLE = QStandardPaths::findExecutable(QStringLiteral("shellcheck"));
        if (EXECUTABLE.isEmpty()) {
            diagnosticsList->clear();
            auto *item = new QListWidgetItem(
                QStringLiteral("ShellCheck is not installed; script checks are unavailable."), diagnosticsList);
            item->setForeground(QColor(QStringLiteral("#d6a84f")));
            QVector<ShellScratchpad::CodeEditor::TextDecoration> decorations;
            appendFuzzyVariableDiagnostics(decorations);
            scriptEdit->setTextDecorations(decorations);
            diagnosticsDock->show();
            shellCheckCurrent = true;
            if (runAfterShellCheck) {
                runAfterShellCheck = false;
                runPendingScriptAfterShellCheck();
            }
            return;
        }
        if (shellCheckProcess.state() != QProcess::NotRunning) {
            shellCheckPending = true;
            shellCheckProcess.kill();
            return;
        }
        shellCheckPending = false;
        activeShellCheckScript = scriptEdit->toPlainText();
        activeShellCheckGeneration = shellCheckGeneration;
        shellCheckProcess.start(EXECUTABLE,
            {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("-s"),
             QStringLiteral("bash"), QStringLiteral("-")});
    }

    void updateShellCheckResults()
    {
        if (activeShellCheckGeneration != shellCheckGeneration)
            return;
        shellCheckCurrent = true;
        const QJsonDocument DOCUMENT = QJsonDocument::fromJson(shellCheckProcess.readAllStandardOutput());
        diagnosticsList->clear();
        QVector<ShellScratchpad::CodeEditor::TextDecoration> decorations;
        if (DOCUMENT.isArray()) {
            for (const QJsonValue &value : DOCUMENT.array()) {
                const QJsonObject finding = value.toObject();
                const int LINE = finding.value(QStringLiteral("line")).toInt();
                const int COLUMN = finding.value(QStringLiteral("column")).toInt();
                const int CODE = finding.value(QStringLiteral("code")).toInt();
                const QString LEVEL = finding.value(QStringLiteral("level")).toString();
                const QString MESSAGE = finding.value(QStringLiteral("message")).toString();
                const QTextBlock START_BLOCK = scriptEdit->document()->findBlockByNumber(LINE - 1);
                if (START_BLOCK.isValid()) {
                    const int START_COLUMN = qMax(1, COLUMN);
                    qsizetype startOffset = START_BLOCK.position() + START_COLUMN - 1;
                    qsizetype endOffset = startOffset;
                    const int END_LINE = finding.value(QStringLiteral("endLine")).toInt(LINE);
                    const int END_COLUMN = finding.value(QStringLiteral("endColumn")).toInt(0);
                    const QTextBlock END_BLOCK = scriptEdit->document()->findBlockByNumber(END_LINE - 1);
                    if (END_COLUMN > 0 && END_BLOCK.isValid()) {
                        endOffset = END_BLOCK.position() + END_COLUMN - 1;
                    } else {
                        endOffset = START_BLOCK.position() + START_BLOCK.text().size();
                    }
                    if (endOffset <= startOffset)
                        endOffset = START_BLOCK.position() + START_BLOCK.text().size();
                    if (endOffset > startOffset) {
                        QTextCharFormat format;
                        if (LEVEL == QLatin1String("error")) {
                            format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
                            format.setUnderlineColor(QColor(QStringLiteral("#ef5350")));
                        } else if (LEVEL == QLatin1String("warning")) {
                            format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
                            format.setUnderlineColor(QColor(QStringLiteral("#e6a23c")));
                        } else {
                            format.setUnderlineStyle(QTextCharFormat::DotLine);
                            format.setUnderlineColor(QColor(QStringLiteral("#69b7e3")));
                        }
                        decorations.append({startOffset, endOffset - startOffset, format});
                    }
                }
                auto *item = new QListWidgetItem(QStringLiteral("%1  SC%2  Line %3:%4 — %5")
                    .arg(LEVEL.toUpper()).arg(CODE, 4, 10, QLatin1Char('0'))
                    .arg(LINE).arg(COLUMN).arg(MESSAGE), diagnosticsList);
                item->setData(Qt::UserRole, LINE);
                item->setData(Qt::UserRole + 1, CODE);
                item->setData(Qt::UserRole + 2, COLUMN);
                item->setData(Qt::UserRole + 3, LEVEL);
                item->setData(Qt::UserRole + 4, MESSAGE);
                if (LEVEL == QLatin1String("error"))
                    item->setForeground(QColor(QStringLiteral("#e06c75")));
                else if (LEVEL == QLatin1String("warning"))
                    item->setForeground(QColor(QStringLiteral("#d6a84f")));
            }
        }
        appendFuzzyVariableDiagnostics(decorations);
        scriptEdit->setTextDecorations(decorations);
        if (diagnosticsList->count() == 0) {
            diagnosticsDock->hide();
            statusBar()->showMessage(QStringLiteral("ShellCheck: no issues found"), 3000);
        } else {
            diagnosticsDock->show();
            statusBar()->showMessage(QStringLiteral("ShellCheck found %1 issue(s)").arg(diagnosticsList->count()), 4000);
        }
        if (runAfterShellCheck) {
            runPendingScriptAfterShellCheck();
        }
    }

    void appendFuzzyVariableDiagnostics(QVector<ShellScratchpad::CodeEditor::TextDecoration> &decorations)
    {
        const QString SOURCE = scriptEdit->toPlainText();
        QSet<QString> knownVariables;
        QStringList candidates;
        const auto addCandidate = [&knownVariables, &candidates](const QString &name) {
            if (!name.isEmpty() && !knownVariables.contains(name)) {
                knownVariables.insert(name);
                candidates.append(name);
            }
        };
        for (const QString &name : QProcessEnvironment::systemEnvironment().keys())
            addCandidate(name);
        const QJsonObject PROFILE_VARIABLES = selectedRunProfile().value(QStringLiteral("environment")).toObject();
        for (auto it = PROFILE_VARIABLES.begin(); it != PROFILE_VARIABLES.end(); ++it)
            addCandidate(it.key());
        if (completionController) {
            for (const auto &symbol : completionController->scriptSymbols()) {
                if (symbol.kind == ShellScratchpad::Completion::CompletionController::ScriptSymbol::Kind::Variable)
                    addCandidate(symbol.name);
            }
        }
        for (const QString &name : {QStringLiteral("BASH"), QStringLiteral("BASHOPTS"),
                 QStringLiteral("BASHPID"), QStringLiteral("BASH_SOURCE"), QStringLiteral("BASH_LINENO"),
                 QStringLiteral("BASH_VERSION"), QStringLiteral("EUID"), QStringLiteral("FUNCNAME"),
                 QStringLiteral("GROUPS"), QStringLiteral("HOME"), QStringLiteral("HOSTNAME"),
                 QStringLiteral("IFS"), QStringLiteral("LINENO"), QStringLiteral("OLDPWD"),
                 QStringLiteral("OPTARG"), QStringLiteral("OPTIND"), QStringLiteral("OSTYPE"),
                 QStringLiteral("PPID"), QStringLiteral("PWD"), QStringLiteral("RANDOM"),
                 QStringLiteral("SECONDS"), QStringLiteral("SHELLOPTS"), QStringLiteral("SHLVL"),
                 QStringLiteral("UID")})
            addCandidate(name);

        const QVector<ShellScratchpad::Syntax::SyntaxToken> tokens =
            ShellScratchpad::Syntax::SyntaxTokenizer::tokenize(SOURCE);
        static const QRegularExpression VARIABLE_NAME(QStringLiteral(R"(^\$\{?([A-Za-z_][A-Za-z0-9_]*))"));
        for (const auto &token : tokens) {
            if (token.kind != ShellScratchpad::Syntax::TokenKind::Variable)
                continue;
            const QString tokenText = SOURCE.mid(token.startOffset, token.length);
            const auto match = VARIABLE_NAME.match(tokenText);
            if (!match.hasMatch())
                continue;
            const QString name = match.captured(1);
            if (knownVariables.contains(name))
                continue;
            const int maximumDistance = qMin(3, qMax(1, name.size() / 4));
            const auto matches = ShellScratchpad::Util::fuzzyMatches(name, candidates, maximumDistance, 3);
            if (matches.isEmpty() || (matches.size() > 1 && matches.at(0).distance == matches.at(1).distance))
                continue;
            const QString suggestion = matches.first().candidate;
            const int identifierOffset = static_cast<int>(token.startOffset + match.capturedStart(1));
            const int identifierLength = name.size();
            const int column = token.column + match.capturedStart(1);
            const QString renderedSuggestion = tokenText.left(match.capturedStart(1)) + suggestion +
                tokenText.mid(match.capturedEnd(1));
            auto *item = new QListWidgetItem(QStringLiteral("INFO  Line %1:%2 — %3 may be a typo; did you mean %4? Double-click to apply.")
                .arg(token.line).arg(column).arg(tokenText, renderedSuggestion), diagnosticsList);
            item->setData(Qt::UserRole, token.line);
            item->setData(Qt::UserRole + 1, 0);
            item->setData(Qt::UserRole + 2, column);
            item->setData(Qt::UserRole + 3, QStringLiteral("info"));
            item->setData(Qt::UserRole + 4, QStringLiteral("%1 may be a typo; did you mean %2?")
                .arg(tokenText, renderedSuggestion));
            item->setData(Qt::UserRole + 5, identifierOffset);
            item->setData(Qt::UserRole + 6, identifierLength);
            item->setData(Qt::UserRole + 7, suggestion);
            item->setData(Qt::UserRole + 8, name);
            item->setForeground(QColor(QStringLiteral("#69b7e3")));

            QTextCharFormat format;
            format.setUnderlineStyle(QTextCharFormat::DotLine);
            format.setUnderlineColor(QColor(QStringLiteral("#69b7e3")));
            decorations.append({identifierOffset, identifierLength, format});
        }
    }

    void configureShellCheckProcess()
    {
        connect(&shellCheckProcess, &QProcess::started, this, [this]() {
            shellCheckProcess.write(activeShellCheckScript.toUtf8());
            shellCheckProcess.closeWriteChannel();
        });
        connect(&shellCheckProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            if (shellCheckPending) {
                shellCheckPending = false;
                shellCheckTimer.stop();
                startShellCheck();
                return;
            }
            updateShellCheckResults();
        });
        connect(&shellCheckProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            if (shellCheckPending) {
                shellCheckPending = false;
                shellCheckTimer.stop();
                startShellCheck();
                return;
            }
            if (activeShellCheckGeneration != shellCheckGeneration)
                return;
            diagnosticsList->clear();
            auto *item = new QListWidgetItem(QStringLiteral("ShellCheck could not be started."), diagnosticsList);
            item->setForeground(QColor(QStringLiteral("#d6a84f")));
            diagnosticsDock->show();
            shellCheckCurrent = true;
            if (runAfterShellCheck) {
                runAfterShellCheck = false;
                runPendingScriptAfterShellCheck();
            }
        });
    }

    void clearRecoveryDraft()
    {
        if (recoveryDraftOwned) {
            QFile::remove(recoveryDraftPath());
            recoveryDraftOwned = false;
        }
    }

    void writeRecoveryDraft()
    {
        if (!recoveryEnabled)
            return;
        if (!scriptEdit->document()->isModified()) {
            clearRecoveryDraft();
            return;
        }
        QJsonObject draft;
        draft.insert(QStringLiteral("script"), scriptEdit->toPlainText());
        draft.insert(QStringLiteral("path"), currentScriptPath);
        QFile file(recoveryDraftPath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(draft).toJson(QJsonDocument::Compact));
            recoveryDraftOwned = true;
        }
    }

    void restoreRecoveryDraft()
    {
        QFile file(recoveryDraftPath());
        if (!file.open(QIODevice::ReadOnly))
            return;
        recoveryDraftOwned = true;
        const QJsonObject draft = QJsonDocument::fromJson(file.readAll()).object();
        if (!draft.contains(QStringLiteral("script"))) {
            clearRecoveryDraft();
            return;
        }
        const QString CONTENT = draft.value(QStringLiteral("script")).toString();
        const auto ANSWER = QMessageBox::question(
            this, QStringLiteral("Recover previous script?"),
            QStringLiteral("A draft from an interrupted session is available. Restore it?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        clearRecoveryDraft();
        if (ANSWER != QMessageBox::Yes)
            return;
        currentScriptPath = draft.value(QStringLiteral("path")).toString();
        if (!currentScriptPath.isEmpty())
            applyScriptRunProfile(currentScriptPath);
        scriptEdit->setPlainText(CONTENT);
        scriptEdit->document()->setModified(true);
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("Recovered draft"), 5000);
    }

    void showScriptFind()
    {
        scriptSearchRow->show();
        scriptFindEdit->setFocus();
        if (!scriptEdit->textCursor().selectedText().isEmpty())
            scriptFindEdit->setText(scriptEdit->textCursor().selectedText());
        scriptFindEdit->selectAll();
    }

    void findInScript(bool backward = false, bool fromTyping = false)
    {
        const QString query = scriptFindEdit->text();
        if (query.isEmpty())
            return;
        QTextDocument::FindFlags flags;
        if (backward)
            flags |= QTextDocument::FindBackward;
        if (scriptFindCaseSensitiveAction->isChecked())
            flags |= QTextDocument::FindCaseSensitively;
        QTextCursor start = scriptEdit->textCursor();
        if (fromTyping) {
            start = QTextCursor(scriptEdit->document());
            start.movePosition(QTextCursor::Start);
        } else if (start.hasSelection()) {
            // Begin beyond the current result so repeated searches visit each
            // occurrence rather than rediscovering the same one.
            start.setPosition(backward ? start.selectionStart() : start.selectionEnd());
        }
        QTextCursor match = scriptEdit->document()->find(query, start, flags);
        if (match.isNull()) {
            QTextCursor wrapped(scriptEdit->document());
            wrapped.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            match = scriptEdit->document()->find(query, wrapped, flags);
        }
        if (!match.isNull()) {
            scriptEdit->setTextCursor(match);
            scriptEdit->centerCursor();
        } else {
            QTextCursor cursor = scriptEdit->textCursor();
            cursor.clearSelection();
            scriptEdit->setTextCursor(cursor);
            statusBar()->showMessage(QStringLiteral("Text not found: %1").arg(query), 3000);
        }
    }

    void changeZoom(int delta)
    {
        zoomLevel = qBound(-6, zoomLevel + delta, 12);
        QFont scriptFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QFont outputFont = scriptFont;
        const int SIZE = qMax(6, scriptFont.pointSize() + zoomLevel);
        scriptFont.setPointSize(SIZE);
        outputFont.setPointSize(SIZE);
        scriptEdit->setFont(scriptFont);
        outputEdit->setFont(outputFont);
    }

    void resetZoom()
    {
        zoomLevel = 0;
        const QFont FONT = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        scriptEdit->setFont(FONT);
        outputEdit->setFont(FONT);
    }

    void activateRelativeWindow(int direction)
    {
        const QList<ShellScratchpadWindow *> WINDOWS = openScratchpadWindows();
        const int CURRENT_INDEX = WINDOWS.indexOf(this);
        if (CURRENT_INDEX < 0 || WINDOWS.size() < 2)
            return;
        const int NEXT_INDEX = (CURRENT_INDEX + direction + WINDOWS.size()) % WINDOWS.size();
        WINDOWS.at(NEXT_INDEX)->showNormal();
        WINDOWS.at(NEXT_INDEX)->raise();
        WINDOWS.at(NEXT_INDEX)->activateWindow();
    }

    void showKeyboardShortcuts()
    {
        QMessageBox::information(this, QStringLiteral("Keyboard Shortcuts"),
            QStringLiteral("<b>File</b><br>"
                           "Ctrl+N — New Script<br>Ctrl+Shift+N — New Window<br>"
                           "Ctrl+O — Open Script<br>Ctrl+W — Close Window<br><br>"
                           "<b>Edit</b><br>Ctrl+Z — Undo<br>Ctrl+Shift+Z — Redo<br>"
                           "Ctrl+X/C/V — Cut/Copy/Paste<br>Ctrl+A — Select All<br>Ctrl+F — Find<br><br>"
                           "<b>Script</b><br>Alt+Enter — Run Script<br>"
                           "Shift+Alt+Enter — Stop Script<br><br>"
                           "<b>View</b><br>Ctrl++ — Zoom In<br>Ctrl+- — Zoom Out<br>Ctrl+0 — Reset Zoom<br><br>"
                           "Ctrl+M, Ctrl+O — Collapse All Sections<br>"
                           "Ctrl+M, Ctrl+P — Expand All Sections<br><br>"
                           "Ctrl+Enter — Insert a blank line above the current line<br>"
                           "Ctrl+S — Save an opened script (does nothing for untitled scripts)."));
    }

    QString expandedWorkingDirectory() const
    {
        QString path = directoryEdit->text().trimmed();
        if (path == QStringLiteral("~"))
            return QDir::homePath();
        if (path.startsWith(QStringLiteral("~/")))
            path = QDir::home().filePath(path.mid(2));
        return QDir::cleanPath(path);
    }

    void saveScript()
    {
        if (!currentScriptPath.isEmpty()) {
            writeScriptToPath(currentScriptPath);
            return;
        }
        QString saveDirectory = expandedWorkingDirectory();
        const QFileInfo WORKING_DIRECTORY_INFO(saveDirectory);
        if (!WORKING_DIRECTORY_INFO.exists() || !WORKING_DIRECTORY_INFO.isDir())
            saveDirectory = QDir::homePath();

        QDialog saveDialog(this);
        saveDialog.setWindowTitle(QStringLiteral("Save Bash Script"));
        saveDialog.setMinimumWidth(480);

        auto *dialogLayout = new QVBoxLayout(&saveDialog);
        auto *nameLabel = new QLabel(QStringLiteral("Script name:"), &saveDialog);
        auto *nameEdit = new QLineEdit(QStringLiteral("script.sh"), &saveDialog);
        nameEdit->selectAll();
        auto *locationLabel = new QLabel(&saveDialog);
        locationLabel->setWordWrap(true);
        auto updateLocationLabel = [&]() {
            locationLabel->setText(QStringLiteral("Save directory: %1").arg(QDir::toNativeSeparators(saveDirectory)));
        };
        updateLocationLabel();

        auto *chooseLocationButton = new QPushButton(QStringLiteral("Choose different save location…"), &saveDialog);
        chooseLocationButton->setToolTip(QStringLiteral("Open the file chooser only if you want to save outside the working directory."));
        connect(chooseLocationButton, &QPushButton::clicked, &saveDialog, [&]() {
            QString suggestedName = nameEdit->text().trimmed();
            if (suggestedName.isEmpty())
                suggestedName = QStringLiteral("script.sh");

            const QString SELECTED_PATH = QFileDialog::getSaveFileName(
                &saveDialog,
                QStringLiteral("Choose Save Location"),
                QDir(saveDirectory).filePath(suggestedName),
                QStringLiteral("Bash scripts (*.sh);;All files (*)"));
            if (SELECTED_PATH.isEmpty())
                return;

            const QFileInfo SELECTED_INFO(SELECTED_PATH);
            saveDirectory = SELECTED_INFO.absolutePath();
            nameEdit->setText(SELECTED_INFO.fileName());
            updateLocationLabel();
            nameEdit->setFocus();
            nameEdit->selectAll();
        });

        auto *buttonBox = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, &saveDialog);
        connect(buttonBox, &QDialogButtonBox::accepted, &saveDialog, [&]() {
            const QString FILE_NAME = nameEdit->text().trimmed();
            if (FILE_NAME.isEmpty()) {
                QMessageBox::warning(&saveDialog, QStringLiteral("Script name required"),
                                     QStringLiteral("Enter a name for the script."));
                nameEdit->setFocus();
                return;
            }
            if (QFileInfo(FILE_NAME).fileName() != FILE_NAME || FILE_NAME == QStringLiteral(".") ||
                FILE_NAME == QStringLiteral("..")) {
                QMessageBox::warning(&saveDialog, QStringLiteral("Invalid script name"),
                                     QStringLiteral("Enter a file name only. Use ‘Choose different save location…’ to change the directory."));
                nameEdit->setFocus();
                return;
            }
            saveDialog.accept();
        });
        connect(buttonBox, &QDialogButtonBox::rejected, &saveDialog, &QDialog::reject);

        dialogLayout->addWidget(nameLabel);
        dialogLayout->addWidget(nameEdit);
        dialogLayout->addWidget(locationLabel);
        dialogLayout->addWidget(chooseLocationButton);
        dialogLayout->addWidget(buttonBox);

        if (saveDialog.exec() != QDialog::Accepted)
            return;

        QString fileName = nameEdit->text().trimmed();
        if (QFileInfo(fileName).suffix().isEmpty())
            fileName += QStringLiteral(".sh");
        const QString SELECTED_PATH = QDir(saveDirectory).filePath(fileName);

        if (QFileInfo::exists(SELECTED_PATH)) {
            const auto OVERWRITE_ANSWER = QMessageBox::question(
                this, QStringLiteral("Overwrite existing script?"),
                QStringLiteral("‘%1’ already exists. Do you want to overwrite it?").arg(fileName),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (OVERWRITE_ANSWER != QMessageBox::Yes)
                return;
        }

        writeScriptToPath(SELECTED_PATH);
    }

    void writeScriptToPath(const QString &path)
    {
        QFile scriptFile(path);
        if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("Could not save script"),
                                 QStringLiteral("Could not write the script:\n%1").arg(scriptFile.errorString()));
            return;
        }

        const QByteArray SCRIPT_BYTES = scriptEdit->toPlainText().toUtf8();
        if (scriptFile.write(SCRIPT_BYTES) != SCRIPT_BYTES.size()) {
            const QString ERROR_TEXT = scriptFile.errorString();
            scriptFile.close();
            QMessageBox::warning(this, QStringLiteral("Could not save script"),
                                 QStringLiteral("The script could not be written completely:\n%1").arg(ERROR_TEXT));
            return;
        }
        scriptFile.close();

        QFile::Permissions scriptPermissions = QFile::permissions(path);
        scriptPermissions |= QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
        if (!QFile::setPermissions(path, scriptPermissions)) {
            QMessageBox::warning(this, QStringLiteral("Script saved without execute permission"),
                                 QStringLiteral("The script was saved, but its executable permission could not be set."));
            statusBar()->showMessage(QStringLiteral("Script saved; could not set executable permission"), 5000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Saved executable script: %1").arg(path), 5000);
        currentScriptPath = QFileInfo(path).absoluteFilePath();
        saveScriptRunProfile(runProfileCombo->currentIndex() > 0 ? runProfileCombo->currentText() : QString());
        scriptEdit->document()->setModified(false);
        recoveryTimer.stop();
        clearRecoveryDraft();
        updateWindowTitle();
    }

    void showRunHistory()
    {
        if (runHistoryDialog) {
            runHistoryDialog->show();
            runHistoryDialog->raise();
            runHistoryDialog->activateWindow();
            return;
        }
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Run History"));
        dialog->resize(720, 460);
        auto *layout = new QVBoxLayout(dialog);
        auto *list = new QListWidget(dialog);
        list->setWordWrap(true);
        layout->addWidget(list, 1);
        runHistoryDialog = dialog;
        runHistoryList = list;
        if (runHistoryEntries.isEmpty())
            list->addItem(QStringLiteral("Run a script to see its top-level commands here."));
        else
            for (qsizetype index = 0; index < runHistoryEntries.size(); ++index) {
                const int LINE = index < runHistorySourceLines.size() ? runHistorySourceLines.at(index) : 0;
                list->addItem(LINE > 0 ? QStringLiteral("Line %1: %2").arg(LINE).arg(runHistoryEntries.at(index))
                                       : runHistoryEntries.at(index));
            }
        connect(list, &QListWidget::itemActivated, this, [this, list](QListWidgetItem *item) {
            const int INDEX = list->row(item);
            if (INDEX < 0 || INDEX >= runHistorySourceLines.size() || runHistorySourceLines.at(INDEX) <= 0)
                return;
            QTextCursor cursor(scriptEdit->document()->findBlockByNumber(runHistorySourceLines.at(INDEX) - 1));
            scriptEdit->setTextCursor(cursor);
            scriptEdit->centerCursor();
            scriptEdit->setFocus();
        });
        connect(dialog, &QObject::destroyed, this, [this]() {
            runHistoryList = nullptr;
        });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    }

    QJsonArray storedRunProfiles() const
    {
        return QJsonArray::fromVariantList(QSettings().value(QStringLiteral("runProfiles/items")).toList());
    }

    QString normalizedScriptPath(const QString &path) const
    {
        const QFileInfo INFO(path);
        const QString CANONICAL = INFO.canonicalFilePath();
        return QDir::cleanPath(CANONICAL.isEmpty() ? INFO.absoluteFilePath() : CANONICAL);
    }

    QString savedScriptRunProfile(const QString &path) const
    {
        if (path.isEmpty())
            return QString();
        const QString IDENTITY = normalizedScriptPath(path);
        const QJsonArray OVERRIDES = QJsonArray::fromVariantList(
            QSettings().value(QStringLiteral("runProfiles/scriptOverrides")).toList());
        for (const QJsonValue &value : OVERRIDES) {
            const QJsonObject ENTRY = value.toObject();
            if (normalizedScriptPath(ENTRY.value(QStringLiteral("path")).toString()) == IDENTITY)
                return ENTRY.value(QStringLiteral("profile")).toString();
        }
        return QString();
    }

    void saveScriptRunProfile(const QString &profileName)
    {
        if (currentScriptPath.isEmpty())
            return;
        const QString IDENTITY = normalizedScriptPath(currentScriptPath);
        QJsonArray updated;
        const QJsonArray existing = QJsonArray::fromVariantList(
            QSettings().value(QStringLiteral("runProfiles/scriptOverrides")).toList());
        for (const QJsonValue &value : existing) {
            const QJsonObject ENTRY = value.toObject();
            if (normalizedScriptPath(ENTRY.value(QStringLiteral("path")).toString()) != IDENTITY)
                updated.append(value);
        }
        if (!profileName.isEmpty()) {
            updated.append(QJsonObject{{QStringLiteral("path"), IDENTITY},
                                       {QStringLiteral("profile"), profileName}});
        }
        QSettings().setValue(QStringLiteral("runProfiles/scriptOverrides"), updated.toVariantList());
    }

    void applyScriptRunProfile(const QString &path)
    {
        if (!runProfileCombo)
            return;
        const QString PROFILE_NAME = savedScriptRunProfile(path);
        int index = PROFILE_NAME.isEmpty() ? 0 : runProfileCombo->findText(PROFILE_NAME, Qt::MatchFixedString);
        if (index < 0) {
            index = 0;
            saveScriptRunProfile(QString());
        }
        applyingScriptRunProfile = true;
        runProfileCombo->setCurrentIndex(index);
        if (index == 0) {
            directoryEdit->setText(defaultWorkingDirectory);
            QSettings().setValue(QStringLiteral("runProfiles/active"), QString());
        } else {
            const QString NAME = runProfileCombo->itemText(index);
            for (const QJsonValue &value : storedRunProfiles()) {
                const QJsonObject PROFILE = value.toObject();
                if (PROFILE.value(QStringLiteral("name")).toString() == NAME) {
                    const QString DIRECTORY = PROFILE.value(QStringLiteral("directory")).toString();
                    if (!DIRECTORY.isEmpty())
                        directoryEdit->setText(DIRECTORY);
                    QSettings().setValue(QStringLiteral("runProfiles/active"), NAME);
                    break;
                }
            }
        }
        applyingScriptRunProfile = false;
    }

    QJsonObject selectedRunProfile() const
    {
        const QString NAME = runProfileCombo ? runProfileCombo->currentText() : QString();
        for (const QJsonValue &value : storedRunProfiles()) {
            const QJsonObject PROFILE = value.toObject();
            if (PROFILE.value(QStringLiteral("name")).toString() == NAME)
                return PROFILE;
        }
        return {};
    }

    void refreshCompletionProfileVariables()
    {
        if (!completionController)
            return;
        QStringList names;
        const QJsonObject OVERRIDES = selectedRunProfile().value(QStringLiteral("environment")).toObject();
        for (auto it = OVERRIDES.begin(); it != OVERRIDES.end(); ++it)
            names.append(it.key());
        completionController->setProfileEnvironmentVariables(names);
        refreshNavigationSymbols();
    }

    void refreshNavigationSymbols()
    {
        if (!scriptEdit || !completionController)
            return;
        QVector<ShellScratchpad::CodeEditor::NavigationSymbol> symbols;
        const QJsonObject PROFILE = selectedRunProfile();
        const QJsonObject OVERRIDES = PROFILE.value(QStringLiteral("environment")).toObject();
        for (auto it = OVERRIDES.begin(); it != OVERRIDES.end(); ++it)
            symbols.append({it.key(), -1, true});
        for (const auto &symbol : completionController->scriptSymbols()) {
            const bool PROFILE_SHADOWS_SYMBOL = OVERRIDES.contains(symbol.name);
            const bool ALREADY_ADDED = std::any_of(symbols.cbegin(), symbols.cend(), [&symbol](const auto &entry) {
                return entry.name == symbol.name;
            });
            if (!PROFILE_SHADOWS_SYMBOL && !ALREADY_ADDED)
                symbols.append({symbol.name, symbol.declarationOffset, false});
        }
        scriptEdit->setNavigationSymbols(symbols);
    }

    void loadRunProfiles()
    {
        if (!runProfileCombo)
            return;
        const QString ACTIVE = QSettings().value(QStringLiteral("runProfiles/active")).toString();
        runProfileCombo->blockSignals(true);
        runProfileCombo->clear();
        runProfileCombo->addItem(QStringLiteral("Default profile"));
        for (const QJsonValue &value : storedRunProfiles()) {
            const QString NAME = value.toObject().value(QStringLiteral("name")).toString();
            if (!NAME.isEmpty())
                runProfileCombo->addItem(NAME);
        }
        const int INDEX = runProfileCombo->findText(ACTIVE);
        runProfileCombo->setCurrentIndex(INDEX > 0 ? INDEX : 0);
        runProfileCombo->blockSignals(false);
        if (runProfileCombo->currentIndex() > 0) {
            const QString PATH = selectedRunProfile().value(QStringLiteral("directory")).toString();
            if (!PATH.isEmpty())
                directoryEdit->setText(PATH);
        }
        refreshCompletionProfileVariables();
    }

    void manageRunProfiles(const QString &focusProfileName = QString(),
                           const QString &focusVariableName = QString())
    {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Run Profiles"));
        dialog.resize(680, 460);
        auto *layout = new QHBoxLayout(&dialog);
        auto *list = new QListWidget(&dialog);
        auto *editor = new QWidget(&dialog);
        auto *form = new QFormLayout(editor);
        auto *name = new QLineEdit(editor);
        auto *directory = new QLineEdit(editor);
        auto *environment = new QPlainTextEdit(editor);
        environment->setPlaceholderText(QStringLiteral("KEY=value (one per line)"));
        auto *directoryBrowse = new QPushButton(QStringLiteral("Browse…"), editor);
        auto *directoryRow = new QHBoxLayout();
        directoryRow->addWidget(directory, 1);
        directoryRow->addWidget(directoryBrowse);
        auto *directoryContainer = new QWidget(editor);
        directoryContainer->setLayout(directoryRow);
        form->addRow(QStringLiteral("Profile name"), name);
        form->addRow(QStringLiteral("Working directory"), directoryContainer);
        form->addRow(QStringLiteral("Environment overrides"), environment);
        auto *buttons = new QHBoxLayout();
        auto *newButton = new QPushButton(QStringLiteral("New"), editor);
        auto *deleteButton = new QPushButton(QStringLiteral("Delete"), editor);
        auto *saveButton = new QPushButton(QStringLiteral("Save Profile"), editor);
        buttons->addWidget(newButton);
        buttons->addWidget(deleteButton);
        buttons->addStretch(1);
        buttons->addWidget(saveButton);
        form->addRow(buttons);
        layout->addWidget(list, 1);
        layout->addWidget(editor, 2);

        QJsonArray profiles = storedRunProfiles();
        auto refillList = [&]() {
            list->clear();
            for (const QJsonValue &value : profiles)
                list->addItem(value.toObject().value(QStringLiteral("name")).toString());
        };
        auto loadSelected = [&]() {
            const int INDEX = list->currentRow();
            if (INDEX < 0 || INDEX >= profiles.size()) {
                name->clear(); directory->clear(); environment->clear(); return;
            }
            const QJsonObject PROFILE = profiles.at(INDEX).toObject();
            name->setText(PROFILE.value(QStringLiteral("name")).toString());
            directory->setText(PROFILE.value(QStringLiteral("directory")).toString());
            QStringList lines;
            const QJsonObject OVERRIDES = PROFILE.value(QStringLiteral("environment")).toObject();
            for (auto it = OVERRIDES.begin(); it != OVERRIDES.end(); ++it)
                lines.append(it.key() + QLatin1Char('=') + it.value().toString());
            environment->setPlainText(lines.join(QLatin1Char('\n')));
        };
        refillList();
        connect(list, &QListWidget::currentRowChanged, &dialog, [&](int) { loadSelected(); });
        if (!focusProfileName.isEmpty()) {
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row)->text() != focusProfileName)
                    continue;
                list->setCurrentRow(row);
                if (!focusVariableName.isEmpty()) {
                    const QStringList lines = environment->toPlainText().split(QLatin1Char('\n'));
                    for (int lineNumber = 0; lineNumber < lines.size(); ++lineNumber) {
                        if (!lines.at(lineNumber).startsWith(focusVariableName + QLatin1Char('=')))
                            continue;
                        const QTextBlock block = environment->document()->findBlockByNumber(lineNumber);
                        QTextCursor cursor(block);
                        cursor.setPosition(block.position() + focusVariableName.size(), QTextCursor::KeepAnchor);
                        environment->setTextCursor(cursor);
                        environment->setFocus();
                        break;
                    }
                }
                break;
            }
        }
        connect(newButton, &QPushButton::clicked, &dialog, [&]() {
            list->clearSelection(); list->setCurrentRow(-1); name->clear(); directory->setText(expandedWorkingDirectory()); environment->clear(); name->setFocus();
        });
        connect(directoryBrowse, &QPushButton::clicked, &dialog, [&]() {
            const QString PATH = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Choose profile working directory"), directory->text());
            if (!PATH.isEmpty()) directory->setText(PATH);
        });
        connect(deleteButton, &QPushButton::clicked, &dialog, [&]() {
            const int INDEX = list->currentRow();
            if (INDEX < 0 || INDEX >= profiles.size()) return;
            profiles.removeAt(INDEX); refillList(); loadSelected();
        });
        connect(saveButton, &QPushButton::clicked, &dialog, [&]() {
            const QString PROFILE_NAME = name->text().trimmed();
            if (PROFILE_NAME.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Profile name required"), QStringLiteral("Enter a name for this run profile."));
                return;
            }
            QJsonObject overrides;
            const QStringList lines = environment->toPlainText().split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const qsizetype SEPARATOR = line.indexOf(QLatin1Char('='));
                if (SEPARATOR <= 0) continue;
                const QString KEY = line.left(SEPARATOR).trimmed();
                if (KEY.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]"))) || KEY.at(0).isDigit()) {
                    QMessageBox::warning(&dialog, QStringLiteral("Invalid environment variable"), QStringLiteral("Use shell variable names such as NAME=value."));
                    return;
                }
                overrides.insert(KEY, line.mid(SEPARATOR + 1));
            }
            QJsonObject profile{{QStringLiteral("name"), PROFILE_NAME},
                                {QStringLiteral("directory"), directory->text().trimmed()},
                                {QStringLiteral("environment"), overrides}};
            int index = -1;
            for (int i = 0; i < profiles.size(); ++i)
                if (profiles.at(i).toObject().value(QStringLiteral("name")).toString() == PROFILE_NAME) { index = i; break; }
            if (index < 0) profiles.append(profile); else profiles.replace(index, profile);
            QSettings().setValue(QStringLiteral("runProfiles/items"), profiles.toVariantList());
            QSettings().setValue(QStringLiteral("runProfiles/active"), PROFILE_NAME);
            refillList();
            list->setCurrentRow(index < 0 ? profiles.size() - 1 : index);
            loadRunProfiles();
        });
        connect(&dialog, &QDialog::accepted, &dialog, [&]() {
            QSettings().setValue(QStringLiteral("runProfiles/items"), profiles.toVariantList());
        });
        auto *closeBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(closeBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
        form->addRow(closeBox);
        connect(closeBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        dialog.exec();
        QSettings().setValue(QStringLiteral("runProfiles/items"), profiles.toVariantList());
        loadRunProfiles();
    }

    QStringList outputBookmarks() const
    {
        return QSettings().value(QStringLiteral("outputBookmarks/items")).toStringList();
    }

    void addOutputBookmark()
    {
        QString line = outputEdit->textCursor().hasSelection()
            ? outputEdit->textCursor().selectedText() : outputEdit->textCursor().block().text();
        line.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        line = line.trimmed();
        if (line.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Select an output line to bookmark"), 2500);
            return;
        }
        QStringList bookmarks = outputBookmarks();
        if (!bookmarks.contains(line)) bookmarks.append(line);
        QSettings().setValue(QStringLiteral("outputBookmarks/items"), bookmarks);
        statusBar()->showMessage(QStringLiteral("Output line bookmarked"), 2500);
    }

    void showOutputBookmarks()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Output Bookmarks"));
        dialog.resize(700, 420);
        auto *layout = new QVBoxLayout(&dialog);
        auto *list = new QListWidget(&dialog);
        const QStringList bookmarks = outputBookmarks();
        list->addItems(bookmarks);
        layout->addWidget(list, 1);
        auto *buttons = new QDialogButtonBox(&dialog);
        auto *remove = buttons->addButton(QStringLiteral("Remove Bookmark"), QDialogButtonBox::ActionRole);
        auto *jump = buttons->addButton(QStringLiteral("Find in Current Output"), QDialogButtonBox::ActionRole);
        buttons->addButton(QDialogButtonBox::Close);
        layout->addWidget(buttons);
        connect(remove, &QPushButton::clicked, &dialog, [&]() {
            const int INDEX = list->currentRow(); if (INDEX < 0) return;
            QStringList updated = outputBookmarks(); updated.removeAt(INDEX);
            QSettings().setValue(QStringLiteral("outputBookmarks/items"), updated);
            delete list->takeItem(INDEX);
        });
        connect(jump, &QPushButton::clicked, &dialog, [&]() {
            if (!list->currentItem()) return;
            const QString BOOKMARK = list->currentItem()->text();
            const qsizetype INDEX = outputContent.indexOf(BOOKMARK, 0, Qt::CaseInsensitive);
            if (INDEX < 0) { statusBar()->showMessage(QStringLiteral("This bookmarked line is not in the current output"), 3000); return; }
            if (!outputFindEdit->text().isEmpty()) outputFindEdit->clear();
            QTextCursor cursor(outputEdit->document()); cursor.setPosition(qMin<qsizetype>(INDEX, outputEdit->document()->characterCount() - 1));
            outputEdit->setTextCursor(cursor); outputEdit->ensureCursorVisible(); outputEdit->setFocus();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
        dialog.exec();
    }

    bool prepareRunHistoryPipe()
    {
        runHistoryEntries.clear();
        runHistorySourceLines.clear();
        runHistoryBuffer.clear();
        runHistoryTruncated = false;
        if (runHistoryList) {
            runHistoryList->clear();
            runHistoryList->addItem(QStringLiteral("Waiting for script commands…"));
        }
        int descriptors[2] = {-1, -1};
        if (::pipe2(descriptors, O_CLOEXEC) != 0)
            return false;
        const int READ_FLAGS = ::fcntl(descriptors[0], F_GETFL);
        if (READ_FLAGS < 0 || ::fcntl(descriptors[0], F_SETFL, READ_FLAGS | O_NONBLOCK) < 0) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
        runHistoryReadDescriptor = descriptors[0];
        runHistoryWriteDescriptor = descriptors[1];
        runHistoryNotifier = new QSocketNotifier(runHistoryReadDescriptor, QSocketNotifier::Read, this);
        connect(runHistoryNotifier, &QSocketNotifier::activated, this,
                [this](QSocketDescriptor, QSocketNotifier::Type) { readRunHistoryPipe(); });
        return true;
    }

    bool prepareDebuggerSocket()
    {
        closeDebuggerSocket();
        int descriptors[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0)
            return false;
        const int FLAGS = ::fcntl(descriptors[0], F_GETFL);
        if (FLAGS < 0 || ::fcntl(descriptors[0], F_SETFL, FLAGS | O_NONBLOCK) < 0) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
        debuggerSocketDescriptor = descriptors[0];
        debuggerChildSocketDescriptor = descriptors[1];
        debuggerSocketBuffer.clear();
        debuggerNotifier = new QSocketNotifier(debuggerSocketDescriptor, QSocketNotifier::Read, this);
        connect(debuggerNotifier, &QSocketNotifier::activated, this,
                [this](QSocketDescriptor, QSocketNotifier::Type) { readDebuggerSocket(); });
        return true;
    }

    void closeDebuggerChildSocket()
    {
        if (debuggerChildSocketDescriptor >= 0) {
            ::close(debuggerChildSocketDescriptor);
            debuggerChildSocketDescriptor = -1;
        }
    }

    void closeDebuggerSocket()
    {
        if (debuggerNotifier) {
            debuggerNotifier->setEnabled(false);
            debuggerNotifier->deleteLater();
            debuggerNotifier = nullptr;
        }
        if (debuggerSocketDescriptor >= 0) {
            ::close(debuggerSocketDescriptor);
            debuggerSocketDescriptor = -1;
        }
        closeDebuggerChildSocket();
        debuggerSocketBuffer.clear();
    }

    void sendDebuggerCommand(char command)
    {
        if (!debuggerEnabled || !debuggerPaused || debuggerSocketDescriptor < 0)
            return;
        const char MESSAGE[2] = {command, '\n'};
        const ssize_t WRITTEN = ::send(debuggerSocketDescriptor, MESSAGE, sizeof(MESSAGE), MSG_NOSIGNAL);
        if (WRITTEN != sizeof(MESSAGE)) {
            statusBar()->showMessage(QStringLiteral("Could not send debugger command"), 3000);
            return;
        }
        debuggerPaused = false;
        scriptEdit->setDebugLine(0);
        runStatusLabel->setText(QStringLiteral("Running"));
        debuggerBanner->setText(QStringLiteral("DEBUG MODE — running until the next breakpoint"));
        debuggerBanner->setStyleSheet(QStringLiteral("QLabel { background: #805c12; color: white; font-weight: bold; padding: 6px; border-radius: 3px; }"));
        statusBar()->showMessage(QStringLiteral("Running under debugger…"));
        updateDebuggerActions();
    }

    void evaluateImmediate()
    {
        if (!debuggerEnabled || !debuggerPaused || debuggerSocketDescriptor < 0 || !immediateInputEdit)
            return;
        const QString EXPRESSION = immediateInputEdit->text();
        if (EXPRESSION.trimmed().isEmpty())
            return;
        const QByteArray ENCODED = EXPRESSION.toUtf8().toBase64(QByteArray::Base64Encoding);
        const QByteArray MESSAGE = QByteArray("e:") + ENCODED + '\n';
        const ssize_t WRITTEN = ::send(debuggerSocketDescriptor, MESSAGE.constData(),
                                       static_cast<size_t>(MESSAGE.size()), MSG_NOSIGNAL);
        if (WRITTEN != MESSAGE.size()) {
            statusBar()->showMessage(QStringLiteral("Could not send expression to the paused Bash process"), 3000);
            return;
        }
        pendingImmediateExpression = EXPRESSION;
        immediateOutputEdit->appendPlainText(QStringLiteral("> %1").arg(EXPRESSION));
        immediateInputEdit->clear();
        immediateInputEdit->setEnabled(false);
        immediateRunButton->setEnabled(false);
    }

    void refreshDebuggerVariableTree()
    {
        if (!variablesTree)
            return;
        variablesTree->clear();
        const QString FILTER = variablesFilter ? variablesFilter->text().trimmed() : QString();
        static const QRegularExpression DECLARATION(
            QStringLiteral(R"(^declare\s+(\S+)\s+([^=\s]+)(?:=(.*))?$)"));
        QSet<QString> SCRIPT_VARIABLES;
        if (completionController) {
            for (const auto &symbol : completionController->scriptSymbols()) {
                if (symbol.kind == ShellScratchpad::Completion::CompletionController::ScriptSymbol::Kind::Variable)
                    SCRIPT_VARIABLES.insert(symbol.name);
            }
        }
        QSet<QString> PROFILE_VARIABLES;
        const QJsonObject PROFILE_OVERRIDES = selectedRunProfile().value(QStringLiteral("environment")).toObject();
        for (auto it = PROFILE_OVERRIDES.begin(); it != PROFILE_OVERRIDES.end(); ++it)
            PROFILE_VARIABLES.insert(it.key());

        auto addGroup = [this, &FILTER](const QString &title, const QSet<QString> &candidates) {
            QStringList names;
            for (const QString &name : candidates) {
                if (debuggerVariables.contains(name))
                    names.append(name);
            }
            names.sort(Qt::CaseInsensitive);
            QList<QTreeWidgetItem *> rows;
            for (const QString &name : names) {
                const QString RAW = debuggerVariables.value(name);
                if (!FILTER.isEmpty() && !name.contains(FILTER, Qt::CaseInsensitive)
                    && !RAW.contains(FILTER, Qt::CaseInsensitive))
                    continue;
                const auto MATCH = DECLARATION.match(RAW);
                const QString FLAGS = MATCH.hasMatch() ? MATCH.captured(1) : QString();
                QString value = MATCH.hasMatch() ? MATCH.captured(3) : RAW;
                if (value.isEmpty() && MATCH.hasMatch() && !RAW.contains(QLatin1Char('=')))
                    value = QStringLiteral("(unset)");
                auto *item = new QTreeWidgetItem({name, FLAGS, value});
                item->setToolTip(2, RAW);
                rows.append(item);
            }
            if (rows.isEmpty() && !FILTER.isEmpty())
                return;
            auto *group = new QTreeWidgetItem(variablesTree, {QStringLiteral("%1 (%2)").arg(title).arg(rows.size())});
            group->setFirstColumnSpanned(true);
            group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
            for (QTreeWidgetItem *row : rows)
                group->addChild(row);
            group->setExpanded(true);
        };

        addGroup(QStringLiteral("Script-local variables"), SCRIPT_VARIABLES);
        QSet<QString> PROFILE_ONLY_VARIABLES;
        for (const QString &name : PROFILE_VARIABLES) {
            if (!SCRIPT_VARIABLES.contains(name))
                PROFILE_ONLY_VARIABLES.insert(name);
        }
        QSet<QString> remaining;
        for (auto it = debuggerVariables.cbegin(); it != debuggerVariables.cend(); ++it) {
            if (!SCRIPT_VARIABLES.contains(it.key()) && !PROFILE_VARIABLES.contains(it.key()))
                remaining.insert(it.key());
        }
        addGroup(QStringLiteral("Execution profile overrides"), PROFILE_ONLY_VARIABLES);
        addGroup(QStringLiteral("Other variables"), remaining);
        if (variablesTree->columnWidth(0) < 120)
            variablesTree->setColumnWidth(0, 130);
    }

    void updateDebuggerActions()
    {
        const bool PAUSED = debuggerEnabled && debuggerPaused;
        if (debugContinueAction) debugContinueAction->setEnabled(PAUSED);
        if (debugStepOverAction) debugStepOverAction->setEnabled(PAUSED);
        if (debugStepInAction) debugStepInAction->setEnabled(PAUSED);
        if (debugStepOutAction) debugStepOutAction->setEnabled(PAUSED && debuggerCurrentDepth > 1);
        if (immediateInputEdit) immediateInputEdit->setEnabled(PAUSED && pendingImmediateExpression.isEmpty());
        if (immediateRunButton) immediateRunButton->setEnabled(PAUSED && pendingImmediateExpression.isEmpty());
    }

    void readDebuggerSocket()
    {
        if (debuggerSocketDescriptor < 0)
            return;
        char buffer[16384];
        while (true) {
            const ssize_t READ_SIZE = ::read(debuggerSocketDescriptor, buffer, sizeof(buffer));
            if (READ_SIZE > 0) {
                debuggerSocketBuffer.append(buffer, static_cast<qsizetype>(READ_SIZE));
                continue;
            }
            if (READ_SIZE == 0 && debuggerNotifier)
                debuggerNotifier->setEnabled(false);
            if (READ_SIZE < 0 && errno == EINTR)
                continue;
            break;
        }
        while (true) {
            const qsizetype NEWLINE = debuggerSocketBuffer.indexOf('\n');
            if (NEWLINE < 0)
                break;
            const QByteArray RECORD = debuggerSocketBuffer.left(NEWLINE);
            debuggerSocketBuffer.remove(0, NEWLINE + 1);
            const QList<QByteArray> FIELDS = RECORD.split('\t');
            if (FIELDS.isEmpty())
                continue;
            if (FIELDS[0] == "P" && FIELDS.size() >= 4) {
                debuggerPaused = true;
                debuggerCurrentDepth = FIELDS[2].toInt();
                const int LINE = FIELDS[1].toInt();
                const QString COMMAND = QString::fromUtf8(QByteArray::fromBase64(FIELDS[3]));
                debuggerVariables.clear();
                refreshDebuggerVariableTree();
                if (completionController)
                    completionController->setRuntimeVariables({});
                const QTextBlock currentBlock = scriptEdit->document()->findBlockByNumber(LINE - 1);
                if (currentBlock.isValid()) {
                    scriptEdit->revealPosition(currentBlock.position());
                    scriptEdit->setDebugLine(LINE);
                    scriptEdit->setTextCursor(QTextCursor(currentBlock));
                    scriptEdit->centerCursor();
                }
                runStatusLabel->setText(QStringLiteral("Paused at line %1").arg(LINE));
                debuggerBanner->setText(QStringLiteral("BREAK MODE — paused before line %1").arg(LINE));
                debuggerBanner->setStyleSheet(QStringLiteral("QLabel { background: #a52a2a; color: white; font-weight: bold; padding: 6px; border-radius: 3px; }"));
                statusBar()->showMessage(QStringLiteral("Paused at line %1: %2").arg(LINE).arg(COMMAND));
                updateDebuggerActions();
            } else if (FIELDS[0] == "V" && FIELDS.size() >= 2) {
                const QString DECLARATIONS = QString::fromUtf8(QByteArray::fromBase64(FIELDS[1]));
                static const QRegularExpression DECLARATION_NAME(
                    QStringLiteral(R"(^declare\s+\S+\s+([^=\s]+))"));
                for (const QString &declaration : DECLARATIONS.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    const auto MATCH = DECLARATION_NAME.match(declaration);
                    if (!MATCH.hasMatch())
                        continue;
                    const QString NAME = MATCH.captured(1);
                    if (NAME.startsWith(QStringLiteral("SSPAD_")) || NAME.startsWith(QStringLiteral("__sspad_")))
                        continue;
                    debuggerVariables.insert(NAME, declaration);
                }
            } else if (FIELDS[0] == "R") {
                if (completionController)
                    completionController->setRuntimeVariables(debuggerVariables);
                refreshDebuggerVariableTree();
                if (!pendingImmediateExpression.isEmpty()) {
                    pendingImmediateExpression.clear();
                    updateDebuggerActions();
                }
            } else if (FIELDS[0] == "E" && FIELDS.size() >= 3) {
                const int EXIT_CODE = FIELDS[1].toInt();
                const QByteArray RESULT = QByteArray::fromBase64(FIELDS[2]);
                QString display = QString::fromLocal8Bit(RESULT);
                if (display.isEmpty())
                    display = QStringLiteral("(no output)");
                immediateOutputEdit->appendPlainText(display);
                immediateOutputEdit->appendPlainText(QStringLiteral("[exit status: %1]").arg(EXIT_CODE));
            }
        }
    }

    void closeRunHistoryWriteEnd()
    {
        if (runHistoryWriteDescriptor >= 0) {
            ::close(runHistoryWriteDescriptor);
            runHistoryWriteDescriptor = -1;
        }
    }

    void closeRunHistoryPipe()
    {
        closeRunHistoryWriteEnd();
        if (runHistoryNotifier) {
            runHistoryNotifier->setEnabled(false);
            runHistoryNotifier->deleteLater();
            runHistoryNotifier = nullptr;
        }
        if (runHistoryReadDescriptor >= 0) {
            ::close(runHistoryReadDescriptor);
            runHistoryReadDescriptor = -1;
        }
        runHistoryBuffer.clear();
    }

    void readRunHistoryPipe()
    {
        if (runHistoryReadDescriptor < 0)
            return;
        char buffer[8192];
        bool reachedEnd = false;
        while (true) {
            const ssize_t READ_SIZE = ::read(runHistoryReadDescriptor, buffer, sizeof(buffer));
            if (READ_SIZE > 0) {
                runHistoryBuffer.append(buffer, static_cast<qsizetype>(READ_SIZE));
                continue;
            }
            if (READ_SIZE == 0) {
                reachedEnd = true;
                break;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            closeRunHistoryPipe();
            return;
        }
        while (true) {
            const qsizetype TERMINATOR = runHistoryBuffer.indexOf('\0');
            if (TERMINATOR < 0)
                break;
            const QByteArray RECORD = runHistoryBuffer.left(TERMINATOR);
            runHistoryBuffer.remove(0, TERMINATOR + 1);
            appendRunHistoryRecord(RECORD);
        }
        if (reachedEnd)
            closeRunHistoryPipe();
    }

    void appendRunHistoryRecord(const QByteArray &record)
    {
        if (record.isEmpty())
            return;
        const char TYPE = record.at(0);
        int SOURCE_LINE = 0;
        QByteArray COMMAND_BYTES = record.mid(1);
        if (TYPE == 'C') {
            const qsizetype TAB = COMMAND_BYTES.indexOf('\t');
            if (TAB >= 0) {
                bool validLine = false;
                SOURCE_LINE = COMMAND_BYTES.left(TAB).toInt(&validLine) + currentHistoryLineOffset;
                if (!validLine)
                    SOURCE_LINE = 0;
                COMMAND_BYTES.remove(0, TAB + 1);
            }
        }
        const QString COMMAND = QString::fromUtf8(COMMAND_BYTES);
        const QString TRIMMED_COMMAND = COMMAND.trimmed();
        if (TRIMMED_COMMAND.isEmpty())
            return;
        static const QRegularExpression SUDO_COMMAND(
            QStringLiteral(R"(^\s*(?:[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*(?:command\s+)?(?:/[^\s]*/)?sudo(?:\s|$))"));
        QString DISPLAY_COMMAND = COMMAND;
        DISPLAY_COMMAND.replace(QLatin1Char('\r'), QString());
        DISPLAY_COMMAND.replace(QLatin1Char('\n'), QStringLiteral(" ↵ "));
        const bool IS_SUDO = TYPE == 'S' || SUDO_COMMAND.match(TRIMMED_COMMAND).hasMatch();
        if (TYPE == 'S') {
            for (qsizetype index = runHistoryEntries.size() - 1; index >= 0; --index) {
                if (runHistoryEntries.at(index) == DISPLAY_COMMAND ||
                    runHistoryEntries.at(index) == QStringLiteral("[sudo] ") + DISPLAY_COMMAND) {
                    runHistoryEntries[index] = QStringLiteral("[sudo] ") + DISPLAY_COMMAND;
                    if (runHistoryList && index < runHistoryList->count())
                        runHistoryList->item(static_cast<int>(index))->setText(
                            runHistorySourceLines.at(index) > 0
                                ? QStringLiteral("Line %1: %2").arg(runHistorySourceLines.at(index)).arg(runHistoryEntries.at(index))
                                : runHistoryEntries.at(index));
                    return;
                }
            }
        }
        if (IS_SUDO && !DISPLAY_COMMAND.startsWith(QStringLiteral("[sudo] ")))
            DISPLAY_COMMAND.prepend(QStringLiteral("[sudo] "));
        if (runHistoryEntries.size() >= 5000) {
            if (!runHistoryTruncated) {
                runHistoryTruncated = true;
                const QString NOTICE = QStringLiteral("History truncated after 5000 commands.");
                runHistoryEntries.append(NOTICE);
                if (runHistoryList)
                    runHistoryList->addItem(NOTICE);
            }
            return;
        }
        runHistoryEntries.append(DISPLAY_COMMAND);
        runHistorySourceLines.append(SOURCE_LINE);
        if (runHistoryList) {
            if (runHistoryList->count() == 1 &&
                runHistoryList->item(0)->text() == QLatin1String("Waiting for script commands…"))
                runHistoryList->clear();
            runHistoryList->addItem(SOURCE_LINE > 0
                ? QStringLiteral("Line %1: %2").arg(SOURCE_LINE).arg(DISPLAY_COMMAND)
                : DISPLAY_COMMAND);
            runHistoryList->scrollToBottom();
        }
    }


    void runPendingScriptAfterShellCheck()
    {
        const bool SELECTION_ONLY = runSelectionAfterShellCheck;
        const bool FORCE_DEBUG = runForceDebugAfterShellCheck;
        runAfterShellCheck = false;
        runSelectionAfterShellCheck = false;
        runForceDebugAfterShellCheck = false;
        runScript(SELECTION_ONLY, FORCE_DEBUG);
    }

    void runScript(bool selectionOnly = false, bool forceDebug = false)
    {

        if (process.state() != QProcess::NotRunning)
            return;

        const QString WORKING_DIRECTORY = expandedWorkingDirectory();
        const QFileInfo DIRECTORY_INFO(WORKING_DIRECTORY);
        if (!DIRECTORY_INFO.exists() || !DIRECTORY_INFO.isDir()) {
            QMessageBox::warning(this, QStringLiteral("Invalid working directory"),
                                 QStringLiteral("That working directory does not exist or is not a directory."));
            directoryEdit->setFocus();
            return;
        }

        if (!shellCheckCurrent) {
            runAfterShellCheck = true;
            runSelectionAfterShellCheck = selectionOnly;
            runForceDebugAfterShellCheck = forceDebug;
            statusBar()->showMessage(QStringLiteral("Checking script with ShellCheck…"));
            if (shellCheckProcess.state() == QProcess::NotRunning)
                shellCheckTimer.start(0);
            else if (activeShellCheckGeneration != shellCheckGeneration) {
                shellCheckPending = true;
                shellCheckProcess.kill();
            }
            return;
        }

        QString sourceText = scriptEdit->toPlainText();
        int selectedStartLine = 1;
        if (selectionOnly) {
            const QTextCursor CURSOR = scriptEdit->textCursor();
            if (!CURSOR.hasSelection()) {
                statusBar()->showMessage(QStringLiteral("Select script text to run"), 3000);
                return;
            }
            selectedStartLine = scriptEdit->document()->findBlock(CURSOR.selectionStart()).blockNumber() + 1;
            sourceText = CURSOR.selectedText();
            sourceText.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        }
        const bool CAPTURE_HISTORY = prepareRunHistoryPipe();
        const QSet<int> BREAKPOINTS = scriptEdit->breakpoints();
        const bool DEBUGGING_ENABLED = forceDebug || !BREAKPOINTS.isEmpty();
        if (DEBUGGING_ENABLED && !prepareDebuggerSocket()) {
            closeRunHistoryPipe();
            QMessageBox::critical(this, QStringLiteral("Could not start debugger"),
                                  QStringLiteral("Could not create the local debugger control channel."));
            return;
        }
        debuggerEnabled = DEBUGGING_ENABLED;
        debuggerPaused = false;
        debuggerCurrentDepth = 0;
        debuggerVariables.clear();
        pendingImmediateExpression.clear();
        debuggerBanner->setVisible(DEBUGGING_ENABLED);
        variablesPanel->setVisible(DEBUGGING_ENABLED);
        immediatePanel->setVisible(DEBUGGING_ENABLED);
        if (DEBUGGING_ENABLED) {
            debuggerBanner->setText(QStringLiteral("DEBUG MODE — running until the next breakpoint"));
            debuggerBanner->setStyleSheet(QStringLiteral("QLabel { background: #805c12; color: white; font-weight: bold; padding: 6px; border-radius: 3px; }"));
            immediateOutputEdit->clear();
        }
        refreshDebuggerVariableTree();
        if (completionController)
            completionController->setRuntimeVariables({});
        updateDebuggerActions();
        scriptEdit->setReadOnly(DEBUGGING_ENABLED);
        if (toggleBreakpointAction)
            toggleBreakpointAction->setEnabled(false);
        const QByteArray RUNTIME_SCRIPT = buildRuntimeScript(
            sourceText.toUtf8(), guiSudoPromptsAction->isChecked(),
            reuseSudoPasswordAction->isChecked(), CAPTURE_HISTORY, DEBUGGING_ENABLED,
            BREAKPOINTS, selectedStartLine - 1, forceDebug);
        currentHistoryLineOffset = selectionOnly ? selectedStartLine - 1 : 0;
        scriptFileDescriptor = createAnonymousScriptFile(RUNTIME_SCRIPT);
        if (scriptFileDescriptor < 0) {
            closeRunHistoryPipe();
            closeDebuggerSocket();
            debuggerEnabled = false;
            scriptEdit->setReadOnly(false);
            updateDebuggerActions();
            if (toggleBreakpointAction)
                toggleBreakpointAction->setEnabled(true);
            QMessageBox::critical(this, QStringLiteral("Could not prepare script"),
                                  QStringLiteral("Could not prepare an in-memory script file:\n%1")
                                      .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }

        if (clearOutputEachRunAction->isChecked()) {
            clearOutput();
        }
        pendingOutput.clear();

        process.setWorkingDirectory(DIRECTORY_INFO.absoluteFilePath());
        QProcessEnvironment runEnvironment = QProcessEnvironment::systemEnvironment();
        const QJsonObject PROFILE = selectedRunProfile();
        const QJsonObject OVERRIDES = PROFILE.value(QStringLiteral("environment")).toObject();
        for (auto it = OVERRIDES.begin(); it != OVERRIDES.end(); ++it)
            runEnvironment.insert(it.key(), it.value().toString());
        process.setProcessEnvironment(runEnvironment);
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setArguments({QStringLiteral("--noprofile"), QStringLiteral("--norc"),
                              QStringLiteral("/dev/fd/%1").arg(SCRIPT_DESCRIPTOR)});
        const int CHILD_SCRIPT_DESCRIPTOR = scriptFileDescriptor;
        const int CHILD_HISTORY_READ_DESCRIPTOR = runHistoryReadDescriptor;
        const int CHILD_HISTORY_WRITE_DESCRIPTOR = runHistoryWriteDescriptor;
        const int CHILD_DEBUG_DESCRIPTOR = debuggerChildSocketDescriptor;
        const int PARENT_DEBUG_DESCRIPTOR = debuggerSocketDescriptor;
        process.setChildProcessModifier([CHILD_SCRIPT_DESCRIPTOR, CHILD_HISTORY_READ_DESCRIPTOR,
                                         CHILD_HISTORY_WRITE_DESCRIPTOR, CHILD_DEBUG_DESCRIPTOR,
                                         PARENT_DEBUG_DESCRIPTOR]() {
            if (::setsid() < 0)
                ::_exit(126);
            if (::dup2(CHILD_SCRIPT_DESCRIPTOR, SCRIPT_DESCRIPTOR) < 0)
                ::_exit(126);
            const int DESCRIPTOR_FLAGS = ::fcntl(SCRIPT_DESCRIPTOR, F_GETFD);
            if (DESCRIPTOR_FLAGS < 0 ||
                ::fcntl(SCRIPT_DESCRIPTOR, F_SETFD, DESCRIPTOR_FLAGS & ~FD_CLOEXEC) < 0)
                ::_exit(126);
            if (CHILD_HISTORY_WRITE_DESCRIPTOR >= 0) {
                if (::dup2(CHILD_HISTORY_WRITE_DESCRIPTOR, HISTORY_DESCRIPTOR) < 0)
                    ::_exit(126);
                const int HISTORY_FLAGS = ::fcntl(HISTORY_DESCRIPTOR, F_GETFD);
                if (HISTORY_FLAGS < 0 ||
                    ::fcntl(HISTORY_DESCRIPTOR, F_SETFD, HISTORY_FLAGS & ~FD_CLOEXEC) < 0)
                    ::_exit(126);
                if (CHILD_HISTORY_READ_DESCRIPTOR >= 0)
                    ::close(CHILD_HISTORY_READ_DESCRIPTOR);
                if (CHILD_HISTORY_WRITE_DESCRIPTOR != HISTORY_DESCRIPTOR)
                    ::close(CHILD_HISTORY_WRITE_DESCRIPTOR);
            }
            if (CHILD_DEBUG_DESCRIPTOR >= 0) {
                if (::dup2(CHILD_DEBUG_DESCRIPTOR, DEBUG_DESCRIPTOR) < 0)
                    ::_exit(126);
                const int DEBUG_FLAGS = ::fcntl(DEBUG_DESCRIPTOR, F_GETFD);
                if (DEBUG_FLAGS < 0 ||
                    ::fcntl(DEBUG_DESCRIPTOR, F_SETFD, DEBUG_FLAGS & ~FD_CLOEXEC) < 0)
                    ::_exit(126);
                if (PARENT_DEBUG_DESCRIPTOR >= 0 && PARENT_DEBUG_DESCRIPTOR != DEBUG_DESCRIPTOR)
                    ::close(PARENT_DEBUG_DESCRIPTOR);
                if (CHILD_DEBUG_DESCRIPTOR != DEBUG_DESCRIPTOR)
                    ::close(CHILD_DEBUG_DESCRIPTOR);
            }
        });

        runButton->setEnabled(false);
        stopButton->setEnabled(true);
        directoryEdit->setEnabled(false);
        stopRequested = false;
        runElapsed.start();
        runDurationLabel->clear();
        runStatusLabel->setText(QStringLiteral("Running"));
        statusBar()->showMessage(QStringLiteral("Running…"));
        promptTimer.stop();
        process.start();
    }

    void stopScript()
    {
        if (process.state() == QProcess::NotRunning)
            return;
        promptTimer.stop();
        stopRequested = true;
        statusBar()->showMessage(QStringLiteral("Stopping script…"));
        terminateProcessGroup();
        killTimer.setSingleShot(true);
        killTimer.start(1500);
    }

    void terminateProcessGroup()
    {
        killProcessGroup(SIGTERM);
    }

    void killProcessGroup(int signalNumber)
    {
        const qint64 pid = process.processId();
        if (pid > 0)
            ::kill(-static_cast<pid_t>(pid), signalNumber);
    }

    void closeScriptFileDescriptor()
    {
        if (scriptFileDescriptor >= 0) {
            ::close(scriptFileDescriptor);
            scriptFileDescriptor = -1;
        }
    }

    void appendOutput(const QString &text)
    {
        if (text.isEmpty())
            return;
        outputContent += text;
        if (outputFindEdit->text().isEmpty()) {
            QTextCursor cursor = outputEdit->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(text);
            outputEdit->setTextCursor(cursor);
            outputEdit->ensureCursorVisible();
        } else {
            outputFilterGeneration->fetch_add(1, std::memory_order_relaxed);
            if (!outputFilterTimer.isActive())
                outputFilterTimer.start();
        }
    }

    void refreshOutputView()
    {
        const QString QUERY = outputFindEdit->text();
        outputFindNavigationPosition = 0;
        outputFilterTimer.stop();
        outputFilterGeneration->fetch_add(1, std::memory_order_relaxed);
        if (QUERY.isEmpty()) {
            outputMatchHighlighter->setQuery(QString(), outputFindCaseSensitiveAction->isChecked());
            outputEdit->setPlainText(outputContent);
            outputEdit->moveCursor(QTextCursor::End);
            outputEdit->ensureCursorVisible();
            return;
        }
        outputFilterTimer.start();
    }

    void navigateOutputMatches(bool backward)
    {
        const QString QUERY = outputFindEdit->text();
        if (QUERY.isEmpty())
            return;
        QTextDocument::FindFlags flags;
        if (backward)
            flags |= QTextDocument::FindBackward;
        if (outputFindCaseSensitiveAction->isChecked())
            flags |= QTextDocument::FindCaseSensitively;

        QTextCursor start(outputEdit->document());
        const qsizetype boundedPosition = qBound<qsizetype>(qsizetype(0), outputFindNavigationPosition,
            static_cast<qsizetype>(outputEdit->document()->characterCount() - 1));
        start.setPosition(static_cast<int>(boundedPosition));
        QTextCursor match = outputEdit->document()->find(QUERY, start, flags);
        if (match.isNull()) {
            QTextCursor wrapped(outputEdit->document());
            wrapped.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            match = outputEdit->document()->find(QUERY, wrapped, flags);
        }
        if (match.isNull())
            return;

        // Output remains read-only and unselected: navigation only scrolls the
        // matching line into view.
        const qsizetype matchStart = match.selectionStart();
        const qsizetype matchEnd = match.selectionEnd();
        QTextCursor visible(outputEdit->document());
        visible.setPosition(static_cast<int>(matchStart));
        outputEdit->setTextCursor(visible);
        outputEdit->ensureCursorVisible();
        outputFindNavigationPosition = backward ? matchStart : matchEnd;
    }

    void filterOutputInBackground()
    {
        const QString QUERY = outputFindEdit->text();
        const bool CASE_SENSITIVE = outputFindCaseSensitiveAction->isChecked();
        if (QUERY.isEmpty())
            return;
        const QString SOURCE = outputContent;
        const quint64 GENERATION = outputFilterGeneration->load(std::memory_order_relaxed);
        const auto GENERATION_STATE = outputFilterGeneration;
        const QPointer<ShellScratchpadWindow> SELF(this);
        QThreadPool *pool = &outputFilterPool;
        pool->start(QRunnable::create([SOURCE, QUERY, CASE_SENSITIVE, GENERATION, GENERATION_STATE, SELF]() {
            QString filtered;
            bool hasMatchingLine = false;
            qsizetype lineStart = 0;
            while (lineStart <= SOURCE.size()) {
                if (GENERATION_STATE->load(std::memory_order_relaxed) != GENERATION)
                    return;
                const qsizetype newline = SOURCE.indexOf(QLatin1Char('\n'), lineStart);
                const qsizetype lineEnd = newline < 0 ? SOURCE.size() : newline;
                const QStringView line(SOURCE.constData() + lineStart, lineEnd - lineStart);
                if (line.contains(QUERY, CASE_SENSITIVE ? Qt::CaseSensitive : Qt::CaseInsensitive)) {
                    if (hasMatchingLine)
                        filtered.append(QLatin1Char('\n'));
                    filtered.append(line);
                    hasMatchingLine = true;
                }
                if (newline < 0)
                    break;
                lineStart = newline + 1;
            }

            QCoreApplication *app = QCoreApplication::instance();
            if (!app)
                return;
            QMetaObject::invokeMethod(app, [SELF, GENERATION_STATE, GENERATION, QUERY, CASE_SENSITIVE,
                                            FILTERED = std::move(filtered)]() mutable {
                if (!SELF || GENERATION_STATE->load(std::memory_order_relaxed) != GENERATION ||
                    SELF->outputFindEdit->text() != QUERY ||
                    SELF->outputFindCaseSensitiveAction->isChecked() != CASE_SENSITIVE)
                    return;
                SELF->outputMatchHighlighter->setQuery(QUERY, CASE_SENSITIVE);
                SELF->outputEdit->setPlainText(FILTERED);
                SELF->outputFindNavigationPosition = 0;
                SELF->outputEdit->moveCursor(QTextCursor::End);
                SELF->outputEdit->ensureCursorVisible();
            }, Qt::QueuedConnection);
        }));
    }

    void clearOutput()
    {
        if (outputFindEdit && !outputFindEdit->text().isEmpty())
            outputFindEdit->clear();
        outputFilterTimer.stop();
        outputFilterGeneration->fetch_add(1, std::memory_order_relaxed);
        outputContent.clear();
        outputEdit->clear();
    }

    void copyOutput()
    {
        const QTextCursor cursor = outputEdit->textCursor();
        QString text = cursor.hasSelection() ? cursor.selectedText() : outputEdit->toPlainText();
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        QApplication::clipboard()->setText(text);
        statusBar()->showMessage(cursor.hasSelection()
            ? QStringLiteral("Selected output copied to clipboard")
            : QStringLiteral("Output copied to clipboard"), 2500);
    }

    void processOutputAvailable()
    {
        pendingOutput.append(process.readAllStandardOutput());

        while (true) {
            const qsizetype MARKER_START = pendingOutput.indexOf(INPUT_MARKER_PREFIX);
            if (MARKER_START < 0) {
                qsizetype matchingSuffixLength = 0;
                const qsizetype MAXIMUM_SUFFIX = qMin(pendingOutput.size(), INPUT_MARKER_PREFIX.size() - 1);
                for (qsizetype LENGTH = MAXIMUM_SUFFIX; LENGTH > 0; --LENGTH) {
                    if (pendingOutput.endsWith(INPUT_MARKER_PREFIX.left(LENGTH))) {
                        matchingSuffixLength = LENGTH;
                        break;
                    }
                }

                const qsizetype DISPLAY_LENGTH = pendingOutput.size() - matchingSuffixLength;
                if (DISPLAY_LENGTH > 0) {
                    appendOutput(QString::fromLocal8Bit(pendingOutput.constData(), DISPLAY_LENGTH));
                    pendingOutput.remove(0, DISPLAY_LENGTH);
                    promptTimer.stop();
                    scheduleExternalPromptCheck();
                }
                return;
            }

            if (MARKER_START > 0) {
                appendOutput(QString::fromLocal8Bit(pendingOutput.constData(), MARKER_START));
                pendingOutput.remove(0, MARKER_START);
                scheduleExternalPromptCheck();
            }

            const qsizetype MARKER_END = pendingOutput.indexOf(INPUT_MARKER_END, INPUT_MARKER_PREFIX.size());
            if (MARKER_END < 0)
                return;

            const QByteArray MARKER_PAYLOAD = pendingOutput.mid(
                INPUT_MARKER_PREFIX.size(), MARKER_END - INPUT_MARKER_PREFIX.size());
            pendingOutput.remove(0, MARKER_END + 1);
            if (!pendingOutput.isEmpty() && pendingOutput.startsWith('\r'))
                pendingOutput.remove(0, 1);
            if (!pendingOutput.isEmpty() && pendingOutput.startsWith('\n'))
                pendingOutput.remove(0, 1);

            const qsizetype SEPARATOR = MARKER_PAYLOAD.indexOf(':');
            if (SEPARATOR >= 0) {
                const QByteArray REQUEST_TYPE = MARKER_PAYLOAD.left(SEPARATOR);
                const QByteArray ENCODED_PROMPT = MARKER_PAYLOAD.mid(SEPARATOR + 1);
                const QString PROMPT_TEXT = QString::fromUtf8(QByteArray::fromBase64(ENCODED_PROMPT));
                promptTimer.stop();
                handleInputRequest(QString::fromLatin1(REQUEST_TYPE), PROMPT_TEXT, false);
            }
        }
    }

    QString latestOutputLine() const
    {
        const QStringList LINES = outputEdit->toPlainText().split(QLatin1Char('\n'));
        for (auto lineIterator = LINES.crbegin(); lineIterator != LINES.crend(); ++lineIterator) {
            const QString CANDIDATE = lineIterator->trimmed();
            if (CANDIDATE.isEmpty() || CANDIDATE.startsWith(QStringLiteral("$ bash (cwd:")) ||
                CANDIDATE.startsWith(QStringLiteral("[Finished with exit code")))
                continue;
            return CANDIDATE;
        }
        return QString();
    }

    bool looksLikeYesNoPrompt(const QString &prompt) const
    {
        static const QRegularExpression explicitChoices(
            QString::fromLatin1(R"((\[\s*[yYnN]\s*/\s*[yYnN]\s*\]|\(\s*[yYnN]\s*/\s*[yYnN]\s*\)|\b(?:y/n|n/y|yes/no|no/yes)\b)\s*$)"),
            QRegularExpression::CaseInsensitiveOption);
        if (explicitChoices.match(prompt).hasMatch())
            return true;

        static const QRegularExpression questionWords(
            QString::fromLatin1(R"(\b(?:continue|proceed|confirm|overwrite|replace|delete|remove|install|uninstall|allow|enable|disable|accept|save|apply|are you sure|do you want|would you like|should we)\b.*[?:]\s*$)"),
            QRegularExpression::CaseInsensitiveOption);
        return questionWords.match(prompt).hasMatch();
    }

    bool looksLikeInputPrompt(const QString &prompt) const
    {
        static const QRegularExpression promptEnding(
            QString::fromLatin1(R"((?:[:?]|\b(?:password|passphrase|enter|input|type|choose|select|name|value|email|username|token)\b)\s*$)"),
            QRegularExpression::CaseInsensitiveOption);
        return promptEnding.match(prompt).hasMatch();
    }

    void scheduleExternalPromptCheck()
    {
        if (process.state() == QProcess::NotRunning || promptDialogOpen)
            return;

        const QString LATEST_LINE = latestOutputLine();
        if (LATEST_LINE.isEmpty()) {
            promptTimer.stop();
            return;
        }

        if (looksLikeYesNoPrompt(LATEST_LINE) || looksLikeInputPrompt(LATEST_LINE))
            promptTimer.start();
        else
            promptTimer.stop();
    }

    void detectExternalCommandPrompt()
    {
        if (process.state() == QProcess::NotRunning || promptDialogOpen)
            return;

        const QString LATEST_LINE = latestOutputLine();
        if (!looksLikeYesNoPrompt(LATEST_LINE) && !looksLikeInputPrompt(LATEST_LINE))
            return;

        handleInputRequest(QStringLiteral("read"), LATEST_LINE, true);
    }

    void handleInputRequest(const QString &requestType, QString prompt, bool promptAlreadyDisplayed)
    {
        if (process.state() == QProcess::NotRunning || promptDialogOpen)
            return;

        promptDialogOpen = true;
        promptTimer.stop();

        const bool IS_SUDO = requestType == QStringLiteral("sudo");
        const bool IS_PASSWORD = IS_SUDO || requestType == QStringLiteral("password");
        const bool PROMPT_WAS_INFERRED = prompt.trimmed().isEmpty();
        if (PROMPT_WAS_INFERRED) {
            prompt = latestOutputLine();
            if (prompt.trimmed().isEmpty())
                prompt = QStringLiteral("The script is waiting for input. Enter a value:");
        }

        if (!promptAlreadyDisplayed && !PROMPT_WAS_INFERRED && !IS_SUDO && !prompt.trimmed().isEmpty())
            appendOutput(prompt);

        QString answer;
        bool accepted = false;
        if (IS_SUDO) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("sudo password"));
            dialog.setLabelText(prompt + QStringLiteral("\n\nYour password will not be shown in the output."));
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setTextEchoMode(QLineEdit::Password);
            dialog.setOkButtonText(QStringLiteral("Authenticate"));
            dialog.setCancelButtonText(QStringLiteral("Cancel"));
            accepted = dialog.exec() == QDialog::Accepted;
            answer = dialog.textValue();
        } else if (requestType == QStringLiteral("password")) {
            QInputDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Script input"));
            dialog.setLabelText(prompt);
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setTextEchoMode(QLineEdit::Password);
            accepted = dialog.exec() == QDialog::Accepted;
            answer = dialog.textValue();
        } else if (looksLikeYesNoPrompt(prompt)) {
            const bool DEFAULT_YES = prompt.contains(QRegularExpression(
                QString::fromLatin1(R"(\[\s*Y\s*/\s*n\s*\]|\(\s*Y\s*/\s*n\s*\))")));
            const auto DEFAULT_BUTTON = DEFAULT_YES ? QMessageBox::Yes : QMessageBox::No;
            const auto REPLY = QMessageBox::question(
                this,
                QStringLiteral("Script confirmation"),
                prompt,
                QMessageBox::Yes | QMessageBox::No,
                DEFAULT_BUTTON);
            accepted = true;
            answer = REPLY == QMessageBox::Yes ? QStringLiteral("y") : QStringLiteral("n");
        } else {
            QInputDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Script input"));
            dialog.setLabelText(prompt);
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setTextEchoMode(QLineEdit::Normal);
            accepted = dialog.exec() == QDialog::Accepted;
            answer = dialog.textValue();
        }

        if (!accepted) {
            promptDialogOpen = false;
            appendOutput(QStringLiteral("\n[Input cancelled; stopping script]\n"));
            stopScript();
            return;
        }

        const QByteArray RESPONSE = answer.toUtf8() + '\n';
        if (process.state() != QProcess::NotRunning)
            process.write(RESPONSE);

        if (IS_SUDO || IS_PASSWORD) {
            appendOutput(QStringLiteral("\n"));
        } else {
            // Mimic terminal input echo for an intelligible output transcript.
            appendOutput(answer + QStringLiteral("\n"));
        }
        promptDialogOpen = false;
    }
};

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("org.winterrose.shellscratchpad"));
    QCoreApplication::setOrganizationName(QStringLiteral("WinterRose"));
    QCoreApplication::setApplicationName(QStringLiteral("ShellScratchpad"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2.0.0-dev.1"));
    application.setWindowIcon(QIcon::fromTheme(QStringLiteral("shell-scratchpad")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Run one-off Bash scripts without creating script files."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption workingDirectoryOption(
        {QStringLiteral("w"), QStringLiteral("working-directory")},
        QStringLiteral("Set the initial working directory for scripts."),
        QStringLiteral("directory"));
    parser.addOption(workingDirectoryOption);
    parser.addPositionalArgument(QStringLiteral("script"),
                                 QStringLiteral("Open a Bash script file."));
    parser.process(application);

    const QStringList POSITIONAL_ARGUMENTS = parser.positionalArguments();
    const QString INITIAL_SCRIPT_PATH = POSITIONAL_ARGUMENTS.isEmpty()
        ? QString() : QFileInfo(POSITIONAL_ARGUMENTS.first()).absoluteFilePath();

    QString initialWorkingDirectory = parser.value(workingDirectoryOption).trimmed();
    if (initialWorkingDirectory.isEmpty()) {
        initialWorkingDirectory = INITIAL_SCRIPT_PATH.isEmpty()
            ? QDir::homePath() : QFileInfo(INITIAL_SCRIPT_PATH).absolutePath();
    } else {
        if (initialWorkingDirectory == QStringLiteral("~")) {
            initialWorkingDirectory = QDir::homePath();
        } else if (initialWorkingDirectory.startsWith(QStringLiteral("~/"))) {
            initialWorkingDirectory = QDir::home().filePath(initialWorkingDirectory.mid(2));
        }
        initialWorkingDirectory = QDir::cleanPath(QDir::isRelativePath(initialWorkingDirectory)
            ? QDir::current().absoluteFilePath(initialWorkingDirectory)
            : initialWorkingDirectory);
    }

    auto *window = new ShellScratchpadWindow(initialWorkingDirectory, INITIAL_SCRIPT_PATH, true);
    window->show();
    return application.exec();
}
