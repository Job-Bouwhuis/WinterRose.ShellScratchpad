#include "CompletionController.h"

#include "editor/CodeEditor.h"
#include "syntax/SyntaxTokenizer.h"

#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QListWidget>
#include <QMap>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolTip>
#include <QTimer>

namespace ShellScratchpad::Completion
{
namespace
{
const QStringList KEYWORDS = {
    QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("elif"),
    QStringLiteral("fi"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("until"),
    QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
    QStringLiteral("function"), QStringLiteral("select"), QStringLiteral("time"), QStringLiteral("coproc")
};

const QStringList BUILTINS = {
    QStringLiteral("alias"), QStringLiteral("bg"), QStringLiteral("bind"), QStringLiteral("break"),
    QStringLiteral("builtin"), QStringLiteral("cd"), QStringLiteral("command"), QStringLiteral("continue"),
    QStringLiteral("declare"), QStringLiteral("echo"), QStringLiteral("eval"), QStringLiteral("exec"),
    QStringLiteral("exit"), QStringLiteral("export"), QStringLiteral("false"), QStringLiteral("fg"),
    QStringLiteral("getopts"), QStringLiteral("hash"), QStringLiteral("help"), QStringLiteral("history"),
    QStringLiteral("jobs"), QStringLiteral("kill"), QStringLiteral("local"), QStringLiteral("printf"),
    QStringLiteral("pwd"), QStringLiteral("read"), QStringLiteral("readonly"), QStringLiteral("return"),
    QStringLiteral("set"), QStringLiteral("shift"), QStringLiteral("source"), QStringLiteral("test"),
    QStringLiteral("times"), QStringLiteral("trap"), QStringLiteral("true"), QStringLiteral("type"),
    QStringLiteral("ulimit"), QStringLiteral("umask"), QStringLiteral("unalias"), QStringLiteral("unset")
};

QHash<QString, QStringList> argumentCatalog()
{
    return {
        {QStringLiteral("ls"), {QStringLiteral("-a"), QStringLiteral("-l"), QStringLiteral("-h"), QStringLiteral("-R"), QStringLiteral("--color=auto")}},
        {QStringLiteral("grep"), {QStringLiteral("-i"), QStringLiteral("-n"), QStringLiteral("-r"), QStringLiteral("-v"), QStringLiteral("-E"), QStringLiteral("-F"), QStringLiteral("--color=auto")}},
        {QStringLiteral("find"), {QStringLiteral("-name"), QStringLiteral("-iname"), QStringLiteral("-type"), QStringLiteral("-mtime"), QStringLiteral("-maxdepth"), QStringLiteral("-exec")}},
        {QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("add"), QStringLiteral("commit"), QStringLiteral("diff"), QStringLiteral("log"), QStringLiteral("switch"), QStringLiteral("restore"), QStringLiteral("-C")}},
        {QStringLiteral("cmake"), {QStringLiteral("-S"), QStringLiteral("-B"), QStringLiteral("--build"), QStringLiteral("--install"), QStringLiteral("-D")}},
        {QStringLiteral("systemctl"), {QStringLiteral("status"), QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("restart"), QStringLiteral("enable"), QStringLiteral("disable"), QStringLiteral("--user")}},
        {QStringLiteral("journalctl"), {QStringLiteral("-b"), QStringLiteral("-k"), QStringLiteral("-u"), QStringLiteral("-n"), QStringLiteral("--since"), QStringLiteral("--no-pager")}},
        {QStringLiteral("apt"), {QStringLiteral("update"), QStringLiteral("upgrade"), QStringLiteral("install"), QStringLiteral("remove"), QStringLiteral("purge"), QStringLiteral("--yes")}},
        {QStringLiteral("dnf"), {QStringLiteral("check-update"), QStringLiteral("upgrade"), QStringLiteral("install"), QStringLiteral("remove"), QStringLiteral("search"), QStringLiteral("--refresh")}},
        {QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("-E"), QStringLiteral("-u"), QStringLiteral("--preserve-env")}}
    };
}

QString optionDescription(const QString &command, const QString &option)
{
    static const QHash<QString, QString> descriptions = {
        {QStringLiteral("-a"), QStringLiteral("Include hidden entries.")},
        {QStringLiteral("-l"), QStringLiteral("Use the long listing format.")},
        {QStringLiteral("-h"), QStringLiteral("Print human-readable sizes or help, depending on the command.")},
        {QStringLiteral("-i"), QStringLiteral("Ignore case distinctions.")},
        {QStringLiteral("-n"), QStringLiteral("Show line numbers or limit output, depending on the command.")},
        {QStringLiteral("-r"), QStringLiteral("Process directories recursively.")},
        {QStringLiteral("-v"), QStringLiteral("Invert the match or increase verbosity, depending on the command.")},
        {QStringLiteral("-E"), QStringLiteral("Interpret the pattern as an extended regular expression.")},
        {QStringLiteral("-F"), QStringLiteral("Interpret the pattern as a fixed string.")},
        {QStringLiteral("--color=auto"), QStringLiteral("Use color when output is connected to a terminal.")},
        {QStringLiteral("-S"), QStringLiteral("Set the CMake source directory.")},
        {QStringLiteral("-B"), QStringLiteral("Set the CMake build directory.")},
        {QStringLiteral("--build"), QStringLiteral("Build a configured CMake build directory.")},
        {QStringLiteral("-D"), QStringLiteral("Set a CMake cache variable.")},
        {QStringLiteral("--no-pager"), QStringLiteral("Do not pipe output through a pager.")},
        {QStringLiteral("--since"), QStringLiteral("Show journal entries newer than a time or date.")},
        {QStringLiteral("--user"), QStringLiteral("Operate on the calling user's service manager.")},
        {QStringLiteral("--yes"), QStringLiteral("Answer yes to prompts automatically.")},
        {QStringLiteral("-u"), QStringLiteral("Select a user or a service unit, depending on the command.")},
        {QStringLiteral("-C"), QStringLiteral("Run Git as if started in the specified directory.")}
    };
    const QString DESCRIPTION = descriptions.value(option);
    return DESCRIPTION.isEmpty()
        ? QStringLiteral("Suggested for %1. Confirm the option with the command's manual or --help.").arg(command)
        : DESCRIPTION;
}

const QHash<QString, QString> &curatedCommandDescriptions()
{
    static const QHash<QString, QString> DESCRIPTIONS = {
        {QStringLiteral("bash"), QStringLiteral("Run commands with the Bash shell.")},
        {QStringLiteral("cat"), QStringLiteral("Print or concatenate files.")},
        {QStringLiteral("cd"), QStringLiteral("Change the current shell working directory.")},
        {QStringLiteral("cmake"), QStringLiteral("Configure or build a project using CMake.")},
        {QStringLiteral("cp"), QStringLiteral("Copy files and directories.")},
        {QStringLiteral("curl"), QStringLiteral("Transfer data to or from a URL.")},
        {QStringLiteral("dnf"), QStringLiteral("Install, update, remove, or inspect Fedora packages.")},
        {QStringLiteral("echo"), QStringLiteral("Print text to standard output.")},
        {QStringLiteral("find"), QStringLiteral("Search for files in a directory hierarchy.")},
        {QStringLiteral("git"), QStringLiteral("Track changes and manage source code repositories.")},
        {QStringLiteral("grep"), QStringLiteral("Search input for lines matching a pattern.")},
        {QStringLiteral("journalctl"), QStringLiteral("Query and display systemd journal logs.")},
        {QStringLiteral("ls"), QStringLiteral("List directory contents.")},
        {QStringLiteral("mkdir"), QStringLiteral("Create directories.")},
        {QStringLiteral("mv"), QStringLiteral("Move or rename files and directories.")},
        {QStringLiteral("pacman"), QStringLiteral("Install, update, remove, or inspect Arch Linux packages.")},
        {QStringLiteral("printf"), QStringLiteral("Format and print text.")},
        {QStringLiteral("pwd"), QStringLiteral("Print the current working directory.")},
        {QStringLiteral("rm"), QStringLiteral("Remove files or directories.")},
        {QStringLiteral("sed"), QStringLiteral("Filter and transform text streams.")},
        {QStringLiteral("sudo"), QStringLiteral("Run a command with another user's privileges.")},
        {QStringLiteral("systemctl"), QStringLiteral("Inspect and control systemd services and units.")},
        {QStringLiteral("tar"), QStringLiteral("Create or extract archive files.")},
        {QStringLiteral("touch"), QStringLiteral("Create files or update file timestamps.")},
        {QStringLiteral("ubuntu-drivers"), QStringLiteral("Manage additional drivers on Ubuntu systems.")},
        {QStringLiteral("which"), QStringLiteral("Locate an executable in the current PATH.")}
    };
    return DESCRIPTIONS;
}

QString builtinDescription(const QString &name)
{
    static const QHash<QString, QString> DESCRIPTIONS = {
        {QStringLiteral("alias"), QStringLiteral("Define or display shell aliases.")},
        {QStringLiteral("bg"), QStringLiteral("Resume a suspended job in the background.")},
        {QStringLiteral("bind"), QStringLiteral("Read or change Readline key bindings and settings.")},
        {QStringLiteral("break"), QStringLiteral("Exit a loop or a specified number of enclosing loops.")},
        {QStringLiteral("builtin"), QStringLiteral("Run a shell builtin, bypassing a same-named function.")},
        {QStringLiteral("cd"), QStringLiteral("Change the current shell working directory.")},
        {QStringLiteral("command"), QStringLiteral("Run a command while bypassing a same-named shell function.")},
        {QStringLiteral("continue"), QStringLiteral("Resume the next iteration of a loop.")},
        {QStringLiteral("declare"), QStringLiteral("Declare variables and set their attributes or scope.")},
        {QStringLiteral("echo"), QStringLiteral("Print text to standard output.")},
        {QStringLiteral("eval"), QStringLiteral("Parse and execute text as shell commands.")},
        {QStringLiteral("exec"), QStringLiteral("Replace the shell with a command, or change shell redirections.")},
        {QStringLiteral("exit"), QStringLiteral("Exit the shell with an optional status code.")},
        {QStringLiteral("export"), QStringLiteral("Mark variables to be passed to child processes.")},
        {QStringLiteral("false"), QStringLiteral("Return an unsuccessful exit status.")},
        {QStringLiteral("fg"), QStringLiteral("Resume a job in the foreground.")},
        {QStringLiteral("getopts"), QStringLiteral("Parse positional parameters as command-line options.")},
        {QStringLiteral("hash"), QStringLiteral("Remember or display command locations found through PATH.")},
        {QStringLiteral("help"), QStringLiteral("Display help for Bash builtins.")},
        {QStringLiteral("history"), QStringLiteral("Display or edit the interactive shell command history.")},
        {QStringLiteral("jobs"), QStringLiteral("List active jobs started by this shell.")},
        {QStringLiteral("kill"), QStringLiteral("Send a signal to a process or job.")},
        {QStringLiteral("local"), QStringLiteral("Declare variables local to a shell function.")},
        {QStringLiteral("printf"), QStringLiteral("Format and print text.")},
        {QStringLiteral("pwd"), QStringLiteral("Print the current working directory.")},
        {QStringLiteral("read"), QStringLiteral("Read a line from standard input into shell variables.")},
        {QStringLiteral("readonly"), QStringLiteral("Mark variables or functions as read-only.")},
        {QStringLiteral("return"), QStringLiteral("Return from a shell function or sourced script.")},
        {QStringLiteral("set"), QStringLiteral("Change shell options or positional parameters.")},
        {QStringLiteral("shift"), QStringLiteral("Remove positional parameters from the start of the list.")},
        {QStringLiteral("source"), QStringLiteral("Read and execute a script in the current shell.")},
        {QStringLiteral("test"), QStringLiteral("Evaluate a conditional expression and return its result.")},
        {QStringLiteral("times"), QStringLiteral("Display accumulated user and system CPU times for the shell and its children.")},
        {QStringLiteral("trap"), QStringLiteral("Run commands when the shell receives signals or other conditions.")},
        {QStringLiteral("true"), QStringLiteral("Return a successful exit status.")},
        {QStringLiteral("type"), QStringLiteral("Describe how Bash interprets a command name.")},
        {QStringLiteral("ulimit"), QStringLiteral("Display or change resource limits for the shell and its children.")},
        {QStringLiteral("umask"), QStringLiteral("Display or set the default permission mask for new files.")},
        {QStringLiteral("unalias"), QStringLiteral("Remove one or more shell aliases.")},
        {QStringLiteral("unset"), QStringLiteral("Remove variables or functions from the shell environment.")}
    };
    return DESCRIPTIONS.value(name, QStringLiteral("Bash builtin command."));
}

bool insideSingleQuotedString(const QString &source, qsizetype end)
{
    bool singleQuoted = false;
    bool doubleQuoted = false;
    bool comment = false;
    bool escaped = false;
    for (qsizetype index = 0; index < end; ++index) {
        const QChar CHARACTER = source.at(index);
        if (comment) {
            if (CHARACTER == QLatin1Char('\n'))
                comment = false;
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (CHARACTER == QLatin1Char('\\') && !singleQuoted) {
            escaped = true;
            continue;
        }
        if (!singleQuoted && !doubleQuoted && CHARACTER == QLatin1Char('#') &&
            (index == 0 || source.at(index - 1).isSpace() || QStringLiteral(";|&(").contains(source.at(index - 1)))) {
            comment = true;
            continue;
        }
        if (CHARACTER == QLatin1Char('\'') && !doubleQuoted)
            singleQuoted = !singleQuoted;
        else if (CHARACTER == QLatin1Char('"') && !singleQuoted)
            doubleQuoted = !doubleQuoted;
    }
    return singleQuoted;
}
}

CompletionController::CompletionController(QPlainTextEdit *targetEditor, QObject *parent)
    : QObject(parent), editor(targetEditor)
{
    popup = new QListWidget(editor);
    popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setFocusPolicy(Qt::NoFocus);
    popup->setMouseTracking(true);
    popup->setMaximumHeight(240);
    popup->setMinimumWidth(340);
    popup->hide();
    popup->setStyleSheet(QStringLiteral("QListWidget { background: #25282d; color: #e5e7eb; border: 1px solid #697586; } QListWidget::item:selected { background: #46566b; }"));
    popup->setStyleSheet(QStringLiteral("QListWidget { background: #25282d; color: #e5e7eb; border: 1px solid #697586; } QListWidget::item:selected { background: #46566b; } QToolTip { background: #25282d; color: #e5e7eb; border: 1px solid #697586; padding: 6px; }"));

    descriptionLookupProcess = new QProcess(this);
    connect(descriptionLookupProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        if (activeDescriptionLookupGeneration == descriptionLookupGeneration)
            consumeDescriptionLookupOutput();
        else
            descriptionLookupProcess->readAllStandardOutput();
    });
    connect(descriptionLookupProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        if (activeDescriptionLookupGeneration == descriptionLookupGeneration)
            consumeDescriptionLookupOutput();
        startNextDescriptionLookupBatch();
        updateSuggestions();
    });
    connect(descriptionLookupProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart &&
            activeDescriptionLookupGeneration == descriptionLookupGeneration) {
            descriptionLookupOffset = descriptionLookupNames.size();
        }
    });

    connect(editor, &QPlainTextEdit::textChanged, this, [this]() { updateSuggestions(); });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this]() { updateSuggestions(); });
    connect(popup, &QListWidget::currentRowChanged, this, [this](int) { showSelectedDescription(); });
    connect(popup, &QListWidget::itemClicked, this, [this](QListWidgetItem *) { acceptCurrentSuggestion(); });
    editor->installEventFilter(this);
    refreshCommandCache();
    refreshSnippets();
}

bool CompletionController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == editor && event->type() == QEvent::KeyPress && popup->isVisible()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int DIRECTION = key->key() == Qt::Key_Down ? 1 : -1;
            const int COUNT = popup->count();
            if (COUNT > 0) {
                int row = popup->currentRow();
                row = (row + DIRECTION + COUNT) % COUNT;
                popup->setCurrentRow(row);
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            acceptCurrentSuggestion();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            popup->hide();
            QToolTip::hideText();
            return true;
        }
        if (key->key() == Qt::Key_Tab && popup->count() > 0) {
            acceptCurrentSuggestion();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void CompletionController::refreshCommandCache()
{
    ++descriptionLookupGeneration;
    if (descriptionLookupProcess->state() != QProcess::NotRunning)
        descriptionLookupProcess->kill();
    systemCommands.clear();
    commandDescriptions.clear();
    descriptionLookupNames.clear();
    descriptionLookupOffset = 0;
    QSet<QString> seen;
    const QString PATH = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
    for (const QString &directory : PATH.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        const QDir dir(directory);
        const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Executable | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString NAME = entry.fileName();
            if (!seen.contains(NAME)) {
                seen.insert(NAME);
                systemCommands.append(NAME);
            }
        }
    }
    systemCommands.sort(Qt::CaseInsensitive);
    descriptionLookupNames = systemCommands;
    descriptionLookupProgram = QStandardPaths::findExecutable(QStringLiteral("whatis"));
    descriptionLookupArguments.clear();
    if (descriptionLookupProgram.isEmpty()) {
        descriptionLookupProgram = QStandardPaths::findExecutable(QStringLiteral("man"));
        descriptionLookupArguments = {QStringLiteral("-f")};
    }
    if (!descriptionLookupProgram.isEmpty())
        startNextDescriptionLookupBatch();
    updateSuggestions();
}

void CompletionController::refreshSnippets()
{
    snippets = Snippets::SnippetStore::loadAll();
    updateSuggestions();
}

void CompletionController::startNextDescriptionLookupBatch()
{
    if (!descriptionLookupProcess || descriptionLookupProcess->state() != QProcess::NotRunning ||
        descriptionLookupProgram.isEmpty() || descriptionLookupOffset >= descriptionLookupNames.size())
        return;
    const qsizetype BATCH_SIZE = 96;
    const qsizetype END = qMin(descriptionLookupNames.size(), descriptionLookupOffset + BATCH_SIZE);
    QStringList arguments = descriptionLookupArguments;
    for (qsizetype index = descriptionLookupOffset; index < END; ++index)
        arguments.append(descriptionLookupNames.at(index));
    descriptionLookupOffset = END;
    descriptionLookupBuffer.clear();
    activeDescriptionLookupGeneration = descriptionLookupGeneration;
    descriptionLookupProcess->setProgram(descriptionLookupProgram);
    descriptionLookupProcess->setArguments(arguments);
    descriptionLookupProcess->start();
}

void CompletionController::consumeDescriptionLookupOutput()
{
    descriptionLookupBuffer += QString::fromLocal8Bit(descriptionLookupProcess->readAllStandardOutput());
    qsizetype NEWLINE = -1;
    while ((NEWLINE = descriptionLookupBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString LINE = descriptionLookupBuffer.left(NEWLINE).trimmed();
        descriptionLookupBuffer.remove(0, NEWLINE + 1);
        parseDescriptionLookupLine(LINE);
    }
    if (descriptionLookupProcess->state() == QProcess::NotRunning && !descriptionLookupBuffer.isEmpty()) {
        parseDescriptionLookupLine(descriptionLookupBuffer.trimmed());
        descriptionLookupBuffer.clear();
    }
}

void CompletionController::parseDescriptionLookupLine(const QString &line)
{
    static const QRegularExpression WHATIS_LINE(
        QStringLiteral(R"(^([^\s,]+)\s+\([^)]*\)\s+-\s+(.+)$)"));
    const auto MATCH = WHATIS_LINE.match(line);
    if (MATCH.hasMatch())
        commandDescriptions.insert(MATCH.captured(1).toLower(), MATCH.captured(2).trimmed());
}

QString CompletionController::commandDescription(const QString &command) const
{
    const QString NAME = command.section(QLatin1Char('/'), -1).toLower();
    const QString MANUAL_DESCRIPTION = commandDescriptions.value(NAME);
    if (!MANUAL_DESCRIPTION.isEmpty())
        return MANUAL_DESCRIPTION;
    const QString CURATED_DESCRIPTION = curatedCommandDescriptions().value(NAME);
    if (!CURATED_DESCRIPTION.isEmpty())
        return CURATED_DESCRIPTION;
    return QStringLiteral("Executable found in the current PATH. No local manual-page summary was found.");
}

void CompletionController::updateSuggestions()
{
    if (!editor)
        return;
    const QString SOURCE = editor->toPlainText();
    const QTextCursor CURSOR = editor->textCursor();
    const qsizetype POSITION = CURSOR.position();
    if (CURSOR.hasSelection()) {
        popup->hide();
        QToolTip::hideText();
        return;
    }

    const QVector<Syntax::SyntaxToken> TOKENS = Syntax::SyntaxTokenizer::tokenize(SOURCE.left(POSITION));
    const Syntax::SyntaxToken *CONTEXT_TOKEN = nullptr;
    for (const Syntax::SyntaxToken &token : TOKENS) {
        if (POSITION > token.startOffset && POSITION <= token.startOffset + token.length) {
            CONTEXT_TOKEN = &token;
            break;
        }
    }
    if ((CONTEXT_TOKEN && (CONTEXT_TOKEN->kind == Syntax::TokenKind::Comment ||
                           CONTEXT_TOKEN->kind == Syntax::TokenKind::HereDocContent)) ||
        insideSingleQuotedString(SOURCE, POSITION)) {
        popup->hide();
        QToolTip::hideText();
        return;
    }

    qsizetype wordStart = POSITION;
    while (wordStart > 0) {
        const QChar CHAR = SOURCE.at(wordStart - 1);
        if (!(CHAR.isLetterOrNumber() || CHAR == QLatin1Char('_') || CHAR == QLatin1Char('-') ||
              CHAR == QLatin1Char('.') || CHAR == QLatin1Char('/') || CHAR == QLatin1Char(':')))
            break;
        --wordStart;
    }
    qsizetype wordEnd = POSITION;
    while (wordEnd < SOURCE.size()) {
        const QChar CHAR = SOURCE.at(wordEnd);
        if (!(CHAR.isLetterOrNumber() || CHAR == QLatin1Char('_') || CHAR == QLatin1Char('-') ||
              CHAR == QLatin1Char('.') || CHAR == QLatin1Char('/') || CHAR == QLatin1Char(':')))
            break;
        ++wordEnd;
    }
    const bool BRACED_VARIABLE = wordStart >= 2 && SOURCE.at(wordStart - 1) == QLatin1Char('{') &&
                                 SOURCE.at(wordStart - 2) == QLatin1Char('$');
    const bool SIMPLE_VARIABLE = wordStart >= 1 && SOURCE.at(wordStart - 1) == QLatin1Char('$');
    const bool SPECIAL_VARIABLE = POSITION >= 2 && SOURCE.at(POSITION - 2) == QLatin1Char('$') &&
        QStringLiteral("?#@*!$0123456789").contains(SOURCE.at(POSITION - 1));
    const bool VARIABLE_POSITION = BRACED_VARIABLE || SIMPLE_VARIABLE || SPECIAL_VARIABLE;
    if (CONTEXT_TOKEN && (CONTEXT_TOKEN->kind == Syntax::TokenKind::StringContent ||
                          CONTEXT_TOKEN->kind == Syntax::TokenKind::StringDelimiter) && !VARIABLE_POSITION) {
        popup->hide();
        QToolTip::hideText();
        return;
    }
    QString PREFIX = SOURCE.mid(wordStart, POSITION - wordStart);
    if (SPECIAL_VARIABLE)
        PREFIX = SOURCE.mid(POSITION - 1, 1);
    replacementStart = BRACED_VARIABLE ? wordStart - 2
        : (SPECIAL_VARIABLE ? POSITION - 2 : (SIMPLE_VARIABLE ? wordStart - 1 : wordStart));
    replacementEnd = SPECIAL_VARIABLE ? POSITION : wordEnd;
    if (BRACED_VARIABLE && replacementEnd < SOURCE.size() && SOURCE.at(replacementEnd) == QLatin1Char('}'))
        ++replacementEnd;
    const QTextBlock BLOCK = editor->document()->findBlock(POSITION);
    const QString BEFORE_WORD = BLOCK.text().left(wordStart - BLOCK.position()).trimmed();
    const bool ENDS_WITH_BOUNDARY = BEFORE_WORD.isEmpty() ||
        QRegularExpression(QStringLiteral(R"((?:^|&&|\|\||[;|])\s*$)" )).match(BEFORE_WORD).hasMatch();
    const bool AFTER_CONTROL = QRegularExpression(QStringLiteral(R"((?:^|\s)(?:if|then|else|elif|while|until|do|!|time)\s*$)"))
                                   .match(BEFORE_WORD).hasMatch();
    static const QRegularExpression SUDO_WITHOUT_COMMAND(
        QStringLiteral(R"(^\s*(?:(?:[A-Za-z_][A-Za-z0-9_]*=\S*)\s+)*sudo(?:\s+(?:(?:-u|-g|-h|-p|-C)\s+\S+|--?[A-Za-z][A-Za-z0-9-]*))*\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const bool AFTER_SUDO_PREFIX = SUDO_WITHOUT_COMMAND.match(BEFORE_WORD).hasMatch();
    commandPosition = ENDS_WITH_BOUNDARY || AFTER_CONTROL || AFTER_SUDO_PREFIX;
    currentCommand.clear();
    if (!commandPosition) {
        static const QRegularExpression COMMAND_MATCH(
            QStringLiteral(R"(^\s*(?:(?:[A-Za-z_][A-Za-z0-9_]*=\S*)\s+)*(?:command\s+|sudo\s+(?:(?:-u|-g|-h|-p|-C)\s+\S+\s+|--?[A-Za-z][A-Za-z0-9-]*\s+)*)?([^\s;&|]+))"),
            QRegularExpression::CaseInsensitiveOption);
        const auto MATCH = COMMAND_MATCH.match(BEFORE_WORD);
        if (MATCH.hasMatch())
            currentCommand = MATCH.captured(1);
    }

    currentSuggestions.clear();
    auto append = [this, &PREFIX](const QString &text, const QString &description, const QString &category) {
        if (!text.startsWith(PREFIX, Qt::CaseInsensitive))
            return;
        currentSuggestions.append(Suggestion{text, description, category, QString(), {}});
    };
    if (VARIABLE_POSITION) {
        QMap<QString, QString> variables;
        auto addVariable = [&variables](const QString &name, const QString &source) {
            QString description = variables.value(name);
            if (!description.isEmpty())
                description += QLatin1Char('\n');
            if (!description.contains(source))
                description += source;
            variables.insert(name, description);
        };
        const QProcessEnvironment ENVIRONMENT = QProcessEnvironment::systemEnvironment();
        for (const QString &name : ENVIRONMENT.keys())
            addVariable(name, QStringLiteral("Inherited environment variable."));

        const QVector<Syntax::SyntaxToken> ALL_TOKENS = Syntax::SyntaxTokenizer::tokenize(SOURCE);
        auto isInNonCodeText = [&ALL_TOKENS](qsizetype offset) {
            for (const Syntax::SyntaxToken &token : ALL_TOKENS) {
                if (offset >= token.startOffset && offset < token.startOffset + token.length)
                    return token.kind == Syntax::TokenKind::Comment ||
                           token.kind == Syntax::TokenKind::StringContent ||
                           token.kind == Syntax::TokenKind::StringDelimiter ||
                           token.kind == Syntax::TokenKind::HereDocContent;
            }
            return false;
        };
        static const QRegularExpression ASSIGNMENT(
            QStringLiteral(R"((?:^|[\s;({])([A-Za-z_][A-Za-z0-9_]*)\s*=)"));
        auto assignmentMatches = ASSIGNMENT.globalMatch(SOURCE);
        while (assignmentMatches.hasNext()) {
            const auto MATCH = assignmentMatches.next();
            if (!isInNonCodeText(MATCH.capturedStart(1)))
                addVariable(MATCH.captured(1), QStringLiteral("Assigned in this script."));
        }
        static const QRegularExpression DECLARATION(
            QStringLiteral(R"(\b(?:export|local|readonly|declare|typeset)\s+(?:-[A-Za-z]+\s+)*([A-Za-z_][A-Za-z0-9_]*)\b)"));
        auto declarationMatches = DECLARATION.globalMatch(SOURCE);
        while (declarationMatches.hasNext()) {
            const auto MATCH = declarationMatches.next();
            if (!isInNonCodeText(MATCH.capturedStart(1)))
                addVariable(MATCH.captured(1), QStringLiteral("Declared in this script."));
        }
        for (auto it = variables.cbegin(); it != variables.cend(); ++it) {
            const QString COMPLETION = BRACED_VARIABLE
                ? QStringLiteral("${%1}").arg(it.key()) : QStringLiteral("$%1").arg(it.key());
            if (it.key().startsWith(PREFIX, Qt::CaseInsensitive)) {
                const bool IS_ENVIRONMENT = it.value().contains(QStringLiteral("Inherited environment"));
                const QString CATEGORY = IS_ENVIRONMENT && it.value().contains(QStringLiteral("script."), Qt::CaseInsensitive)
                    ? QStringLiteral("Environment + Script")
                    : (IS_ENVIRONMENT ? QStringLiteral("Environment") : QStringLiteral("Script variable"));
                currentSuggestions.append(Suggestion{COMPLETION, it.value(), CATEGORY, QString(), {}});
            }
        }
        for (const QString &special : {QStringLiteral("$?"), QStringLiteral("$#"), QStringLiteral("$@"),
                                       QStringLiteral("$*"), QStringLiteral("$!"), QStringLiteral("$$")}) {
            if (PREFIX.isEmpty() || special.mid(1).startsWith(PREFIX, Qt::CaseInsensitive))
                currentSuggestions.append(Suggestion{special, QStringLiteral("Special Bash parameter."),
                                                     QStringLiteral("Special parameter"), QString(), {}});
        }
    } else if (commandPosition) {
        for (const Snippets::Snippet &snippet : snippets) {
            if (snippet.name.startsWith(PREFIX, Qt::CaseInsensitive))
                currentSuggestions.append(Suggestion{snippet.name, snippet.description,
                                                     QStringLiteral("Snippet"), snippet.code, snippet.highlightPoints});
        }
        if (!AFTER_SUDO_PREFIX) {
            for (const QString &word : KEYWORDS)
                append(word, QStringLiteral("Bash reserved word."), QStringLiteral("Keyword"));
            for (const QString &word : BUILTINS)
                append(word, builtinDescription(word), QStringLiteral("Builtin"));
        }
        for (const QString &word : systemCommands)
            if (!BUILTINS.contains(word))
                append(word, commandDescription(word), QStringLiteral("Command"));
    } else {
        const QString NORMALIZED_COMMAND = currentCommand.section(QLatin1Char('/'), -1).toLower();
        const QStringList OPTIONS = argumentCatalog().value(NORMALIZED_COMMAND);
        for (const QString &option : OPTIONS)
            append(option, optionDescription(NORMALIZED_COMMAND, option), QStringLiteral("Argument"));
        for (const Snippets::Snippet &snippet : snippets) {
            if (snippet.name.startsWith(PREFIX, Qt::CaseInsensitive))
                currentSuggestions.append(Suggestion{snippet.name, snippet.description,
                                                     QStringLiteral("Snippet"), snippet.code, snippet.highlightPoints});
        }
    }

    popup->clear();
    const int DISPLAY_LIMIT = 80;
    for (int index = 0; index < qMin(DISPLAY_LIMIT, currentSuggestions.size()); ++index) {
        const Suggestion &suggestion = currentSuggestions.at(index);
        auto *item = new QListWidgetItem(QStringLiteral("%1    %2").arg(suggestion.text, suggestion.category), popup);
        item->setData(Qt::UserRole, index);
    }
    if ((PREFIX.isEmpty() && !VARIABLE_POSITION) || popup->count() == 0) {
        popup->hide();
        QToolTip::hideText();
        return;
    }
    popup->setCurrentRow(0);
    popup->setFixedHeight(qMin(240, popup->sizeHintForRow(0) * qMin(popup->count(), 10) + 6));
    positionPopup();
    popup->show();
    popup->raise();
    QTimer::singleShot(0, this, [this]() { showSelectedDescription(); });
}

void CompletionController::positionPopup()
{
    const QRect CURSOR_RECT = editor->cursorRect();
    const QPoint BELOW = editor->viewport()->mapToGlobal(CURSOR_RECT.bottomLeft() + QPoint(0, 3));
    popup->move(BELOW);
}

void CompletionController::acceptCurrentSuggestion()
{
    if (popup->currentRow() < 0 || popup->currentRow() >= popup->count())
        return;
    const int SUGGESTION_INDEX = popup->currentItem()->data(Qt::UserRole).toInt();
    if (SUGGESTION_INDEX < 0 || SUGGESTION_INDEX >= currentSuggestions.size())
        return;
    const Suggestion SUGGESTION = currentSuggestions.at(SUGGESTION_INDEX);
    QTextCursor cursor(editor->document());
    cursor.setPosition(qMin<qsizetype>(replacementStart, editor->document()->characterCount() - 1));
    cursor.setPosition(qMin<qsizetype>(replacementEnd, editor->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    if (SUGGESTION.category == QLatin1String("Snippet")) {
        QVector<QPair<int, int>> points;
        points.reserve(SUGGESTION.highlightPoints.size());
        for (const Snippets::HighlightPoint &point : SUGGESTION.highlightPoints)
            points.append(qMakePair(static_cast<int>(point.offset), static_cast<int>(point.length)));
        if (auto *codeEditor = qobject_cast<ShellScratchpad::CodeEditor *>(editor)) {
            editor->setTextCursor(cursor);
            codeEditor->insertSnippet(SUGGESTION.insertText, points);
            popup->hide();
            QToolTip::hideText();
            editor->setFocus();
            return;
        }
    }
    cursor.insertText(SUGGESTION.insertText.isEmpty() ? SUGGESTION.text : SUGGESTION.insertText);
    editor->setTextCursor(cursor);
    popup->hide();
    QToolTip::hideText();
    editor->setFocus();
}

void CompletionController::showSelectedDescription()
{
    // setCurrentRow() fires before updateSuggestions() shows the popup. Do not
    // anchor a tooltip to that not-yet-visible list; the queued call after show()
    // will run once Qt has laid out and exposed the suggestion rows.
    if (!popup->isVisible()) {
        QToolTip::hideText();
        return;
    }
    if (popup->currentRow() < 0 || popup->currentRow() >= popup->count()) {
        QToolTip::hideText();
        return;
    }
    const int INDEX = popup->currentItem()->data(Qt::UserRole).toInt();
    if (INDEX < 0 || INDEX >= currentSuggestions.size()) {
        QToolTip::hideText();
        return;
    }
    const QString DESCRIPTION = currentSuggestions.at(INDEX).description;
    const QRect ROW_RECT = popup->visualItemRect(popup->currentItem());
    if (ROW_RECT.isEmpty())
        return;
    const QPoint GLOBAL_POSITION = popup->mapToGlobal(QPoint(popup->width() + 8, ROW_RECT.top()));
    QToolTip::showText(GLOBAL_POSITION, DESCRIPTION, popup, ROW_RECT);
}
}
