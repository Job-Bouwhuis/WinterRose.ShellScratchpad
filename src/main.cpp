#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <QApplication>
#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QKeyEvent>
#include <QList>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "syntax/SyntaxHighlighter.h"
#include "syntax/SyntaxDebugDialog.h"

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
const QByteArray INPUT_MARKER_PREFIX = QByteArray(1, '\x1e') + "SSPAD_INPUT:";
constexpr char INPUT_MARKER_END = '\x1f';

QByteArray buildRuntimeScript(const QByteArray &userScript)
{
    static const QByteArray RUNTIME_PRELUDE = R"SSPAD(
# Keep a separate duplicate of the app's input stream for prompts and sudo.
exec 199<&0
SSPAD_ORIGINAL_STDIN="$(readlink "/proc/${BASHPID}/fd/0" 2>/dev/null || true)"

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

    # If credentials are already available, preserve sudo's normal stdin behavior.
    if command sudo -n -v >/dev/null 2>&1; then
        command sudo "$@"
        return $?
    fi

    local password=""
    local sudo_prompt="Authentication required for: sudo $*"
    local xtrace_enabled=0
    local proxy_pid=0
    local command_status=0
    case "$-" in *x*) xtrace_enabled=1; set +x ;; esac

    shell_scratchpad_emit_request "sudo" "$sudo_prompt"
    local IFS=""
    if ! builtin read -r -s -u 199 password; then
        unset password
        if ((xtrace_enabled)); then set -x; fi
        return 1
    fi

    # Feed sudo its password first, then forward later dialog answers to the command.
    exec 197< <(builtin printf '%s\n' "$password"; exec cat <&199)
    proxy_pid=$!
    if command sudo -S -p '' "$@" <&197; then
        command_status=0
    else
        command_status=$?
    fi
    kill "$proxy_pid" 2>/dev/null || true
    wait "$proxy_pid" 2>/dev/null || true
    exec 197<&-

    # The password is never printed or passed as a command-line argument.
    unset password
    if ((xtrace_enabled)); then set -x; fi
    return "$command_status"
}
)SSPAD";

    QByteArray runtimeScript = RUNTIME_PRELUDE;
    runtimeScript.append('\n');
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
}

class ShellScratchpadWindow final : public QMainWindow
{
public:
    explicit ShellScratchpadWindow(const QString &initialWorkingDirectory)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QStringLiteral("Shell Scratchpad"));
        resize(980, 760);
        setMinimumSize(640, 480);
        openScratchpadWindows().append(this);

        auto *central = new QWidget(this);
        auto *pageLayout = new QVBoxLayout(central);
        pageLayout->setContentsMargins(18, 16, 18, 14);
        pageLayout->setSpacing(10);

        auto *title = new QLabel(QStringLiteral("Shell Scratchpad"), central);
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 5);
        titleFont.setWeight(QFont::DemiBold);
        title->setFont(titleFont);
        pageLayout->addWidget(title);

        auto *directoryRow = new QHBoxLayout();
        directoryRow->setSpacing(8);
        auto *directoryLabel = new QLabel(QStringLiteral("Working directory"), central);
        directoryEdit = new QLineEdit(
            initialWorkingDirectory.isEmpty() ? QDir::homePath() : initialWorkingDirectory, central);
        directoryEdit->setClearButtonEnabled(true);
        directoryEdit->setPlaceholderText(QStringLiteral("Directory used by the script, e.g. ~/Projects"));
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
        pageLayout->addLayout(directoryRow);

        splitter = new QSplitter(Qt::Vertical, central);
        splitter->setChildrenCollapsible(false);
        splitter->setHandleWidth(7);

        auto *scriptPanel = new QWidget(splitter);
        auto *scriptLayout = new QVBoxLayout(scriptPanel);
        scriptLayout->setContentsMargins(0, 0, 0, 0);
        scriptLayout->setSpacing(6);
        auto *scriptHeading = new QLabel(QStringLiteral("Bash script"), scriptPanel);
        scriptEdit = new QPlainTextEdit(scriptPanel);
        scriptEdit->setObjectName(QStringLiteral("scriptEdit"));
        scriptEdit->setPlaceholderText(
            QStringLiteral("#!/usr/bin/env bash\nset -euo pipefail\n\n# Paste or write your script here…"));
        scriptEdit->setTabChangesFocus(false);
        scriptEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        scriptEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        scriptEdit->setPlainText(QStringLiteral(
            "#!/usr/bin/env bash\nset -euo pipefail\n\necho \"Hello from Bash!\"\nprintf 'Working directory: %s\\n' \"$PWD\"\n"));
        new ShellScratchpad::Syntax::SyntaxHighlighter(scriptEdit);
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
        copyButton->setToolTip(QStringLiteral("Copy all output to the clipboard"));
        connect(copyButton, &QPushButton::clicked, this, [this]() {
            QApplication::clipboard()->setText(outputEdit->toPlainText());
            statusBar()->showMessage(QStringLiteral("Output copied to clipboard"), 2500);
        });
        auto *clearButton = new QPushButton(QStringLiteral("Clear output"), outputPanel);
        connect(clearButton, &QPushButton::clicked, this, [this]() { outputEdit->clear(); });
        outputHeadingRow->addWidget(outputHeading);
        outputHeadingRow->addStretch(1);
        outputHeadingRow->addWidget(clearOutputCheck);
        outputHeadingRow->addWidget(copyButton);
        outputHeadingRow->addWidget(clearButton);

        outputEdit = new QPlainTextEdit(outputPanel);
        outputEdit->setReadOnly(true);
        outputEdit->setObjectName(QStringLiteral("outputEdit"));
        outputEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        outputEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        outputEdit->setPlaceholderText(QStringLiteral("Script output and errors will appear here."));
        outputLayout->addLayout(outputHeadingRow);
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
        auto *saveButton = new QPushButton(QStringLiteral("Save script"), central);
        saveButton->setToolTip(QStringLiteral("Save the script and make it executable. Ctrl+S is intentionally not bound."));
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveScript(); });
        auto *newButton = new QPushButton(QStringLiteral("New script"), central);
        connect(newButton, &QPushButton::clicked, this, [this]() {
            if (process.state() != QProcess::NotRunning)
                return;
            scriptEdit->clear();
            scriptEdit->setFocus();
        });
        buttonRow->addWidget(runButton);
        buttonRow->addWidget(stopButton);
        buttonRow->addStretch(1);
        buttonRow->addWidget(saveButton);
        buttonRow->addWidget(newButton);
        pageLayout->addLayout(buttonRow);

        setCentralWidget(central);

        createMenus();
        statusBar()->showMessage(QStringLiteral("Ready  •  Ctrl+Enter to run"));

        process.setProcessChannelMode(QProcess::MergedChannels);
        connect(runButton, &QPushButton::clicked, this, [this]() { runScript(); });
        connect(stopButton, &QPushButton::clicked, this, [this]() { stopScript(); });
        scriptEdit->installEventFilter(this);

        connect(&process, &QProcess::readyReadStandardOutput, this, [this]() {
            processOutputAvailable();
        });
        connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
            processOutputAvailable();
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

            if (exitStatus == QProcess::NormalExit) {
                appendOutput(QStringLiteral("\n[Finished with exit code %1]\n").arg(exitCode));
                statusBar()->showMessage(exitCode == 0
                    ? QStringLiteral("Finished successfully")
                    : QStringLiteral("Finished with exit code %1").arg(exitCode));
            } else {
                appendOutput(QStringLiteral("\n[Script process crashed or was terminated]\n"));
                statusBar()->showMessage(QStringLiteral("Script terminated"));
            }
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                appendOutput(QStringLiteral("\n[Could not start Bash: %1]\n").arg(process.errorString()));
                promptTimer.stop();
                killTimer.stop();
                runButton->setEnabled(true);
                stopButton->setEnabled(false);
                directoryEdit->setEnabled(true);
                closeScriptFileDescriptor();
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
    }

    ~ShellScratchpadWindow() override
    {
        openScratchpadWindows().removeAll(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == scriptEdit && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Return && keyEvent->modifiers().testFlag(Qt::ControlModifier)) {
                runScript();
                return true;
            }
            // Ctrl+S intentionally has no save binding; users must click Save script.
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
        closeScriptFileDescriptor();
        event->accept();
    }

private:
    QPlainTextEdit *scriptEdit = nullptr;
    QPlainTextEdit *outputEdit = nullptr;
    QWidget *outputPanel = nullptr;
    QSplitter *splitter = nullptr;
    QLineEdit *directoryEdit = nullptr;
    QCheckBox *clearOutputCheck = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *stopButton = nullptr;
    QProcess process;
    QPointer<ShellScratchpad::Syntax::SyntaxDebugDialog> syntaxDebugDialog;
    QPointer<QAction> showOutputAction;
    QTimer killTimer;
    QTimer promptTimer;
    QByteArray pendingOutput;
    int scriptFileDescriptor = -1;
    bool promptDialogOpen = false;
    int zoomLevel = 0;

    void createMenus()
    {
        QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        QAction *newScriptAction = fileMenu->addAction(QStringLiteral("New Script"));
        newScriptAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
        connect(newScriptAction, &QAction::triggered, this, [this]() {
            scriptEdit->clear();
            setWindowTitle(QStringLiteral("Shell Scratchpad"));
            scriptEdit->setFocus();
        });

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
        connect(saveAction, &QAction::triggered, this, [this]() { saveScript(); });
        fileMenu->addSeparator();
        QAction *closeAction = fileMenu->addAction(QStringLiteral("Close Window"));
        closeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
        connect(closeAction, &QAction::triggered, this, &QWidget::close);

        QMenu *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        QAction *undoAction = editMenu->addAction(QStringLiteral("Undo"));
        undoAction->setShortcut(QKeySequence::Undo);
        connect(undoAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::undo);
        QAction *redoAction = editMenu->addAction(QStringLiteral("Redo"));
        redoAction->setShortcut(QKeySequence::Redo);
        connect(redoAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::redo);
        editMenu->addSeparator();
        QAction *cutAction = editMenu->addAction(QStringLiteral("Cut"));
        cutAction->setShortcut(QKeySequence::Cut);
        connect(cutAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::cut);
        QAction *copyAction = editMenu->addAction(QStringLiteral("Copy"));
        copyAction->setShortcut(QKeySequence::Copy);
        connect(copyAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::copy);
        QAction *pasteAction = editMenu->addAction(QStringLiteral("Paste"));
        pasteAction->setShortcut(QKeySequence::Paste);
        connect(pasteAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::paste);
        QAction *selectAllAction = editMenu->addAction(QStringLiteral("Select All"));
        selectAllAction->setShortcut(QKeySequence::SelectAll);
        connect(selectAllAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::selectAll);
        editMenu->addSeparator();
        QAction *findAction = editMenu->addAction(QStringLiteral("Find…"));
        findAction->setShortcut(QKeySequence::Find);
        connect(findAction, &QAction::triggered, this, [this]() { findInScript(); });

        QMenu *scriptMenu = menuBar()->addMenu(QStringLiteral("&Script"));
        QAction *runAction = scriptMenu->addAction(QStringLiteral("Run Script"));
        runAction->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
        connect(runAction, &QAction::triggered, this, [this]() { runScript(); });
        QAction *stopAction = scriptMenu->addAction(QStringLiteral("Stop Script"));
        stopAction->setShortcut(QKeySequence(QStringLiteral("Shift+Alt+Return")));
        connect(stopAction, &QAction::triggered, this, [this]() { stopScript(); });
        scriptMenu->addSeparator();
        QAction *clearOutputAction = scriptMenu->addAction(QStringLiteral("Clear Output"));
        connect(clearOutputAction, &QAction::triggered, outputEdit, &QPlainTextEdit::clear);
        QAction *clearScriptAction = scriptMenu->addAction(QStringLiteral("Clear Script"));
        connect(clearScriptAction, &QAction::triggered, scriptEdit, &QPlainTextEdit::clear);
        QAction *setWorkingDirectoryAction = scriptMenu->addAction(QStringLiteral("Set Working Directory…"));
        connect(setWorkingDirectoryAction, &QAction::triggered, this, [this]() {
            const QString DIRECTORY = QFileDialog::getExistingDirectory(
                this, QStringLiteral("Choose working directory"), expandedWorkingDirectory());
            if (!DIRECTORY.isEmpty())
                directoryEdit->setText(QDir::cleanPath(DIRECTORY));
        });

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
        QAction *zoomInAction = viewMenu->addAction(QStringLiteral("Zoom In"));
        zoomInAction->setShortcut(QKeySequence(QStringLiteral("Ctrl++")));
        connect(zoomInAction, &QAction::triggered, this, [this]() { changeZoom(1); });
        QAction *zoomOutAction = viewMenu->addAction(QStringLiteral("Zoom Out"));
        zoomOutAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+-")));
        connect(zoomOutAction, &QAction::triggered, this, [this]() { changeZoom(-1); });
        QAction *resetZoomAction = viewMenu->addAction(QStringLiteral("Reset Zoom"));
        resetZoomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
        connect(resetZoomAction, &QAction::triggered, this, [this]() { resetZoom(); });

        QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
        QAction *codeStyleAction = toolsMenu->addAction(QStringLiteral("Code Style…"));
        codeStyleAction->setToolTip(QStringLiteral("Code style preferences are not implemented yet."));
        QAction *debuggerAction = toolsMenu->addAction(QStringLiteral("Syntax Debugger…"));
        connect(debuggerAction, &QAction::triggered, this, [this]() {
            if (!syntaxDebugDialog)
                syntaxDebugDialog = new ShellScratchpad::Syntax::SyntaxDebugDialog(scriptEdit, this);
            syntaxDebugDialog->show();
            syntaxDebugDialog->raise();
            syntaxDebugDialog->activateWindow();
        });
        QAction *refreshCommandCacheAction = toolsMenu->addAction(QStringLiteral("Refresh Command Cache"));
        refreshCommandCacheAction->setEnabled(false);
        refreshCommandCacheAction->setToolTip(QStringLiteral("Command lookup caching has not been implemented yet."));

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

    void openScript()
    {
        const QString PATH = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Bash Script"), expandedWorkingDirectory(),
            QStringLiteral("Bash scripts (*.sh);;All files (*)"));
        if (PATH.isEmpty())
            return;
        QFile file(PATH);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("Could not open script"),
                                 QStringLiteral("Could not read the script:\n%1").arg(file.errorString()));
            return;
        }
        scriptEdit->setPlainText(QString::fromUtf8(file.readAll()));
        setWindowTitle(QStringLiteral("%1 — Shell Scratchpad").arg(QFileInfo(PATH).fileName()));
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(PATH), 5000);
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
                           "Ctrl+S is intentionally unbound."));
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

        QFile scriptFile(SELECTED_PATH);
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

        QFile::Permissions scriptPermissions = QFile::permissions(SELECTED_PATH);
        scriptPermissions |= QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
        if (!QFile::setPermissions(SELECTED_PATH, scriptPermissions)) {
            QMessageBox::warning(this, QStringLiteral("Script saved without execute permission"),
                                 QStringLiteral("The script was saved, but its executable permission could not be set."));
            statusBar()->showMessage(QStringLiteral("Script saved; could not set executable permission"), 5000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Saved executable script: %1").arg(SELECTED_PATH), 5000);
    }

    void runScript()
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

        const QByteArray RUNTIME_SCRIPT = buildRuntimeScript(scriptEdit->toPlainText().toUtf8());
        scriptFileDescriptor = createAnonymousScriptFile(RUNTIME_SCRIPT);
        if (scriptFileDescriptor < 0) {
            QMessageBox::critical(this, QStringLiteral("Could not prepare script"),
                                  QStringLiteral("Could not prepare an in-memory script file:\n%1")
                                      .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }

        if (clearOutputCheck->isChecked())
            outputEdit->clear();
        pendingOutput.clear();
        appendOutput(QStringLiteral("$ bash (cwd: %1)\n").arg(DIRECTORY_INFO.absoluteFilePath()));

        process.setWorkingDirectory(DIRECTORY_INFO.absoluteFilePath());
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setArguments({QStringLiteral("--noprofile"), QStringLiteral("--norc"),
                              QStringLiteral("/dev/fd/%1").arg(SCRIPT_DESCRIPTOR)});
        const int CHILD_SCRIPT_DESCRIPTOR = scriptFileDescriptor;
        process.setChildProcessModifier([CHILD_SCRIPT_DESCRIPTOR]() {
            if (::setsid() < 0)
                ::_exit(126);
            if (::dup2(CHILD_SCRIPT_DESCRIPTOR, SCRIPT_DESCRIPTOR) < 0)
                ::_exit(126);
            const int DESCRIPTOR_FLAGS = ::fcntl(SCRIPT_DESCRIPTOR, F_GETFD);
            if (DESCRIPTOR_FLAGS < 0 ||
                ::fcntl(SCRIPT_DESCRIPTOR, F_SETFD, DESCRIPTOR_FLAGS & ~FD_CLOEXEC) < 0)
                ::_exit(126);
        });

        runButton->setEnabled(false);
        stopButton->setEnabled(true);
        directoryEdit->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Running…"));
        promptTimer.stop();
        process.start();
    }

    void stopScript()
    {
        if (process.state() == QProcess::NotRunning)
            return;
        promptTimer.stop();
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
        QTextCursor cursor = outputEdit->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
        outputEdit->setTextCursor(cursor);
        outputEdit->ensureCursorVisible();
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
    QCoreApplication::setApplicationName(QStringLiteral("Shell Scratchpad"));
    QCoreApplication::setOrganizationName(QStringLiteral("WinterRose"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2.0.0-dev.1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Run one-off Bash scripts without creating script files."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption workingDirectoryOption(
        {QStringLiteral("w"), QStringLiteral("working-directory")},
        QStringLiteral("Set the initial working directory for scripts."),
        QStringLiteral("directory"));
    parser.addOption(workingDirectoryOption);
    parser.process(application);

    QString initialWorkingDirectory = parser.value(workingDirectoryOption).trimmed();
    if (initialWorkingDirectory.isEmpty()) {
        initialWorkingDirectory = QDir::homePath();
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

    auto *window = new ShellScratchpadWindow(initialWorkingDirectory);
    window->show();
    return application.exec();
}
