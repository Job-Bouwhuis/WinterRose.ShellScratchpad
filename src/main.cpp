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
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QKeySequence>
#include <QKeyCombination>
#include <QLabel>
#include <QKeyEvent>
#include <QList>
#include <QListWidget>
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
#include <QSyntaxHighlighter>
#include <QStringList>
#include <QStringView>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QThreadPool>
#include <QRunnable>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <atomic>
#include <memory>

#include "syntax/SyntaxHighlighter.h"
#include "syntax/SyntaxDebugDialog.h"
#include "syntax/SyntaxColorDialog.h"
#include "editor/CodeEditor.h"
#include "completion/CompletionController.h"
#include "snippets/SnippetManagerDialog.h"
#include "snippets/SnippetCreateDialog.h"
#include "snippets/SnippetStore.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
constexpr int SCRIPT_DESCRIPTOR = 198;
constexpr int HISTORY_DESCRIPTOR = 197;
const QByteArray INPUT_MARKER_PREFIX = QByteArray(1, '\x1e') + "SSPAD_INPUT:";
constexpr char INPUT_MARKER_END = '\x1f';

QByteArray buildRuntimeScript(const QByteArray &userScript, bool handleSudoWithDialog,
                              bool reuseSudoPassword, bool captureHistory)
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
    if (captureHistory) {
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
    runtimeScript.append("SSPAD_USER_SCRIPT_START_LINE=$((LINENO + 1))\n");
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

    void setQuery(const QString &newQuery)
    {
        if (query == newQuery)
            return;
        query = newQuery;
    }

protected:
    void highlightBlock(const QString &text) override
    {
        if (query.isEmpty())
            return;
        qsizetype start = 0;
        while (start < text.size()) {
            const qsizetype MATCH = text.indexOf(query, start, Qt::CaseInsensitive);
            if (MATCH < 0)
                break;
            setFormat(static_cast<int>(MATCH), query.size(), matchFormat);
            start = MATCH + qMax(query.size(), 1);
        }
    }

private:
    QString query;
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
        });

        splitter = new QSplitter(Qt::Vertical, central);
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(7);

        auto *scriptPanel = new QWidget(splitter);
        auto *scriptLayout = new QVBoxLayout(scriptPanel);
        scriptLayout->setContentsMargins(0, 0, 0, 0);
        scriptLayout->setSpacing(6);
        auto *scriptHeading = new QLabel(QStringLiteral("Bash script"), scriptPanel);
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
        scriptLayout->addWidget(scriptHeading);
        scriptLayout->addWidget(scriptEdit, 1);

        outputPanel = new QWidget(splitter);
        auto *outputLayout = new QVBoxLayout(outputPanel);
        outputLayout->setContentsMargins(0, 0, 0, 0);
        outputLayout->setSpacing(6);
        auto *outputHeadingRow = new QHBoxLayout();
        auto *outputHeading = new QLabel(QStringLiteral("Output"), outputPanel);
        clearOutputCheck = new QCheckBox(QStringLiteral("Clear before each run"), outputPanel);
        clearOutputCheck->setChecked(true);
        auto *copyButton = new QPushButton(QStringLiteral("Copy output"), outputPanel);
        copyButton->setToolTip(QStringLiteral("Copy selected output, or all output when nothing is selected"));
        connect(copyButton, &QPushButton::clicked, this, [this]() {
            const QTextCursor CURSOR = outputEdit->textCursor();
            QString text = CURSOR.hasSelection() ? CURSOR.selectedText() : outputEdit->toPlainText();
            text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            QApplication::clipboard()->setText(text);
            statusBar()->showMessage(CURSOR.hasSelection()
                ? QStringLiteral("Selected output copied to clipboard")
                : QStringLiteral("Output copied to clipboard"), 2500);
        });
        auto *clearButton = new QPushButton(QStringLiteral("Clear output"), outputPanel);
        connect(clearButton, &QPushButton::clicked, this, [this]() {
            clearOutput();
        });
        auto *bookmarkButton = new QPushButton(QStringLiteral("Bookmark line"), outputPanel);
        bookmarkButton->setToolTip(QStringLiteral("Save the selected output line as a bookmark"));
        connect(bookmarkButton, &QPushButton::clicked, this, [this]() { addOutputBookmark(); });
        outputHeadingRow->addWidget(outputHeading);
        outputHeadingRow->addStretch(1);
        outputHeadingRow->addWidget(clearOutputCheck);
        outputHeadingRow->addWidget(copyButton);
        outputHeadingRow->addWidget(clearButton);
        outputHeadingRow->addWidget(bookmarkButton);

        auto *outputSearchRow = new QHBoxLayout();
        outputSearchRow->addWidget(new QLabel(QStringLiteral("Find in output"), outputPanel));
        outputFindEdit = new QLineEdit(outputPanel);
        outputFindEdit->setClearButtonEnabled(true);
        outputFindEdit->setPlaceholderText(QStringLiteral("Type to show matching lines…"));
        outputFindEdit->setToolTip(QStringLiteral("Filter output to lines containing this text; matching characters are highlighted."));
        outputSearchRow->addWidget(outputFindEdit, 1);

        outputEdit = new QPlainTextEdit(outputPanel);
        outputEdit->setReadOnly(true);
        outputEdit->setObjectName(QStringLiteral("outputEdit"));
        outputEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        outputEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        outputEdit->setPlaceholderText(QStringLiteral("Script output and errors will appear here."));
        outputMatchHighlighter = new OutputMatchHighlighter(outputEdit->document());
        outputLayout->addLayout(outputHeadingRow);
        outputLayout->addLayout(outputSearchRow);
        outputLayout->addWidget(outputEdit, 1);

        splitter->addWidget(scriptPanel);
        splitter->addWidget(outputPanel);
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

        guiSudoCheck = new QCheckBox(QStringLiteral("GUI sudo prompts"), this);
        guiSudoCheck->setChecked(true);
        guiSudoCheck->setToolTip(QStringLiteral("Show a masked dialog when the script needs sudo authentication. Turn this off only when the script or environment handles sudo input itself."));
        reuseSudoPasswordCheck = new QCheckBox(QStringLiteral("Reuse sudo password for this run"), this);
        reuseSudoPasswordCheck->setChecked(true);
        reuseSudoPasswordCheck->setToolTip(QStringLiteral("Keep the password only in the running script process so sudo is prompted once per run."));
        auto *sudoOptions = new QWidget(this);
        auto *sudoOptionsLayout = new QHBoxLayout(sudoOptions);
        sudoOptionsLayout->setContentsMargins(0, 0, 0, 0);
        sudoOptionsLayout->addWidget(guiSudoCheck);
        sudoOptionsLayout->addWidget(reuseSudoPasswordCheck);
        sudoOptionsLayout->addStretch(1);
        pageLayout->insertWidget(2, sudoOptions);
        connect(guiSudoCheck, &QCheckBox::toggled, reuseSudoPasswordCheck, &QWidget::setEnabled);
        outputFilterPool.setMaxThreadCount(1);
        outputFilterTimer.setSingleShot(true);
        outputFilterTimer.setInterval(90);
        connect(&outputFilterTimer, &QTimer::timeout, this, [this]() { filterOutputInBackground(); });
        connect(scriptEdit->document(), &QTextDocument::modificationChanged, this,
                [this](bool) { updateWindowTitle(); });
        connect(scriptEdit, &QPlainTextEdit::textChanged, this, [this]() {
            updateWindowTitle();
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
        scriptEdit->installEventFilter(this);

        connect(&process, &QProcess::readyReadStandardOutput, this, [this]() {
            processOutputAvailable();
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
            processOutputAvailable();
            readRunHistoryPipe();
            closeRunHistoryPipe();
            if (!pendingOutput.isEmpty()) {
                appendOutput(QString::fromLocal8Bit(pendingOutput));
                pendingOutput.clear();
            }

            promptTimer.stop();
            killTimer.stop();
            runButton->setEnabled(true);
            stopButton->setEnabled(false);
            directoryEdit->setEnabled(true);
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
        openScratchpadWindows().removeAll(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == scriptEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const Qt::KeyboardModifiers MODIFIERS = keyEvent->modifiers();
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
    QPlainTextEdit *outputEdit = nullptr;
    QLineEdit *outputFindEdit = nullptr;
    QWidget *outputPanel = nullptr;
    QSplitter *splitter = nullptr;
    QLineEdit *directoryEdit = nullptr;
    QString defaultWorkingDirectory;
    QComboBox *runProfileCombo = nullptr;
    QCheckBox *clearOutputCheck = nullptr;
    QCheckBox *guiSudoCheck = nullptr;
    QCheckBox *reuseSudoPasswordCheck = nullptr;
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
    bool applyingScriptRunProfile = false;
    int currentHistoryLineOffset = 0;
    QString latestDraftSnapshot;
    bool recoveryEnabled = false;
    bool recoveryDraftOwned = false;
    int zoomLevel = 0;

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
                outputEdit->copy();
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
                outputFindEdit->setFocus();
                outputFindEdit->selectAll();
            } else {
                findInScript();
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
        QAction *stopAction = scriptMenu->addAction(QStringLiteral("Stop Script"));
        stopAction->setShortcut(QKeySequence(QStringLiteral("Shift+Alt+Return")));
        connect(stopAction, &QAction::triggered, this, [this]() { stopScript(); });
        scriptMenu->addSeparator();
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
        dialog.setMinimumSize(540, 560);
        auto *layout = new QVBoxLayout(&dialog);
        auto *quickSavesHeading = new QLabel(QStringLiteral("Your Quick Saves"), &dialog);
        auto *quickSavesList = new QListWidget(&dialog);
        auto *diagnosticsHeading = new QLabel(QStringLiteral("Built-in Diagnostics"), &dialog);
        auto *diagnosticsScriptsList = new QListWidget(&dialog);
        layout->addWidget(quickSavesHeading);
        layout->addWidget(quickSavesList, 1);
        layout->addWidget(diagnosticsHeading);
        layout->addWidget(diagnosticsScriptsList, 1);

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
            diagnosticsDock->show();
            shellCheckCurrent = true;
            if (runAfterShellCheck) {
                runAfterShellCheck = false;
                runScript();
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
        if (DOCUMENT.isArray()) {
            for (const QJsonValue &value : DOCUMENT.array()) {
                const QJsonObject finding = value.toObject();
                const int LINE = finding.value(QStringLiteral("line")).toInt();
                const int COLUMN = finding.value(QStringLiteral("column")).toInt();
                const int CODE = finding.value(QStringLiteral("code")).toInt();
                const QString LEVEL = finding.value(QStringLiteral("level")).toString();
                const QString MESSAGE = finding.value(QStringLiteral("message")).toString();
                auto *item = new QListWidgetItem(QStringLiteral("%1  SC%2  Line %3:%4 — %5")
                    .arg(LEVEL.toUpper()).arg(CODE, 4, 10, QLatin1Char('0'))
                    .arg(LINE).arg(COLUMN).arg(MESSAGE), diagnosticsList);
                item->setData(Qt::UserRole, LINE);
                if (LEVEL == QLatin1String("error"))
                    item->setForeground(QColor(QStringLiteral("#e06c75")));
                else if (LEVEL == QLatin1String("warning"))
                    item->setForeground(QColor(QStringLiteral("#d6a84f")));
            }
        }
        if (diagnosticsList->count() == 0) {
            diagnosticsDock->hide();
            statusBar()->showMessage(QStringLiteral("ShellCheck: no issues found"), 3000);
        } else {
            diagnosticsDock->show();
            statusBar()->showMessage(QStringLiteral("ShellCheck found %1 issue(s)").arg(diagnosticsList->count()), 4000);
        }
        if (runAfterShellCheck) {
            runAfterShellCheck = false;
            const bool SELECTION_ONLY = runSelectionAfterShellCheck;
            runSelectionAfterShellCheck = false;
            runScript(SELECTION_ONLY);
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
                runScript();
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

    void findInScript()
    {
        bool accepted = false;
        const QString query = QInputDialog::getText(
            this, QStringLiteral("Find in Script"), QStringLiteral("Text to find:"),
            QLineEdit::Normal, QString(), &accepted);
        if (!accepted || query.isEmpty())
            return;
        if (scriptEdit->find(query))
            return;
        QTextCursor cursor = scriptEdit->textCursor();
        cursor.movePosition(QTextCursor::Start);
        scriptEdit->setTextCursor(cursor);
        if (!scriptEdit->find(query))
            statusBar()->showMessage(QStringLiteral("Text not found: %1").arg(query), 3000);
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
    }

    void manageRunProfiles()
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


    void runScript(bool selectionOnly = false)
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
        const QByteArray RUNTIME_SCRIPT = buildRuntimeScript(
            sourceText.toUtf8(), guiSudoCheck->isChecked(),
            reuseSudoPasswordCheck->isChecked(), CAPTURE_HISTORY);
        currentHistoryLineOffset = selectionOnly ? selectedStartLine - 1 : 0;
        scriptFileDescriptor = createAnonymousScriptFile(RUNTIME_SCRIPT);
        if (scriptFileDescriptor < 0) {
            closeRunHistoryPipe();
            QMessageBox::critical(this, QStringLiteral("Could not prepare script"),
                                  QStringLiteral("Could not prepare an in-memory script file:\n%1")
                                      .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }

        if (clearOutputCheck->isChecked()) {
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
        process.setChildProcessModifier([CHILD_SCRIPT_DESCRIPTOR, CHILD_HISTORY_READ_DESCRIPTOR,
                                         CHILD_HISTORY_WRITE_DESCRIPTOR]() {
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
        outputFilterTimer.stop();
        outputFilterGeneration->fetch_add(1, std::memory_order_relaxed);
        if (QUERY.isEmpty()) {
            outputMatchHighlighter->setQuery(QString());
            outputEdit->setPlainText(outputContent);
            outputEdit->moveCursor(QTextCursor::End);
            outputEdit->ensureCursorVisible();
            return;
        }
        outputFilterTimer.start();
    }

    void filterOutputInBackground()
    {
        const QString QUERY = outputFindEdit->text();
        if (QUERY.isEmpty())
            return;
        const QString SOURCE = outputContent;
        const quint64 GENERATION = outputFilterGeneration->load(std::memory_order_relaxed);
        const auto GENERATION_STATE = outputFilterGeneration;
        const QPointer<ShellScratchpadWindow> SELF(this);
        QThreadPool *pool = &outputFilterPool;
        pool->start(QRunnable::create([SOURCE, QUERY, GENERATION, GENERATION_STATE, SELF]() {
            QString filtered;
            bool hasMatchingLine = false;
            qsizetype lineStart = 0;
            while (lineStart <= SOURCE.size()) {
                if (GENERATION_STATE->load(std::memory_order_relaxed) != GENERATION)
                    return;
                const qsizetype newline = SOURCE.indexOf(QLatin1Char('\n'), lineStart);
                const qsizetype lineEnd = newline < 0 ? SOURCE.size() : newline;
                const QStringView line(SOURCE.constData() + lineStart, lineEnd - lineStart);
                if (line.contains(QUERY, Qt::CaseInsensitive)) {
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
            QMetaObject::invokeMethod(app, [SELF, GENERATION_STATE, GENERATION, QUERY,
                                            FILTERED = std::move(filtered)]() mutable {
                if (!SELF || GENERATION_STATE->load(std::memory_order_relaxed) != GENERATION ||
                    SELF->outputFindEdit->text() != QUERY)
                    return;
                SELF->outputMatchHighlighter->setQuery(QUERY);
                SELF->outputEdit->setPlainText(FILTERED);
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
