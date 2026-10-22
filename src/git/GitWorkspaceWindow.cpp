#include "GitWorkspaceWindow.h"

#include "completion/CompletionController.h"
#include "editor/CodeEditor.h"
#include "syntax/SyntaxHighlighter.h"

#include <QComboBox>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ShellScratchpad::Git
{
namespace
{
QString gitExecutable()
{
    return QStringLiteral("git");
}

}

bool GitWorkspaceWindow::isGitRepository(const QString &path)
{
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("-C"), QDir(path).absolutePath(), QStringLiteral("rev-parse"),
                          QStringLiteral("--is-inside-work-tree")});
    process.start();
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    return process.exitCode() == 0 && process.readAllStandardOutput().trimmed() == QByteArray("true");
}

GitWorkspaceWindow::GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent)
    : QMainWindow(parent), repoPath(QDir(repositoryPath).absolutePath())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Git Workspace — %1").arg(QDir(repoPath).dirName()));
    resize(1100, 760);

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *toolbar = new QHBoxLayout;
    const QList<QPair<QString, std::function<void()>>> buttons = {
        {QStringLiteral("Refresh"), [this] { refreshAll(); }},
        {QStringLiteral("Stage All"), [this] { runGit({QStringLiteral("add"), QStringLiteral("-A")}); }},
        {QStringLiteral("Commit…"), [this] {
             bool ok = false;
             const QString message = QInputDialog::getText(this, QStringLiteral("Commit"),
                 QStringLiteral("Commit message:"), QLineEdit::Normal, QString(), &ok).trimmed();
             if (ok && !message.isEmpty()) runGit({QStringLiteral("commit"), QStringLiteral("-m"), message});
         }},
        {QStringLiteral("New Branch…"), [this] {
             bool ok = false;
             const QString name = QInputDialog::getText(this, QStringLiteral("New branch"),
                 QStringLiteral("Branch name:"), QLineEdit::Normal, QString(), &ok).trimmed();
             if (ok && !name.isEmpty()) runGit({QStringLiteral("switch"), QStringLiteral("-c"), name});
         }},
        {QStringLiteral("Stash"), [this] { runGit({QStringLiteral("stash"), QStringLiteral("push")}); }},
        {QStringLiteral("Pop Stash"), [this] { runGit({QStringLiteral("stash"), QStringLiteral("pop")}); }},
        {QStringLiteral("Fetch"), [this] { runGit({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")}); }},
        {QStringLiteral("Pull"), [this] { runGit({QStringLiteral("pull"), QStringLiteral("--rebase")}); }},
        {QStringLiteral("Push"), [this] { runGit({QStringLiteral("push")}); }}
    };
    for (const auto &entry : buttons) {
        auto *button = new QPushButton(entry.first, root);
        toolbar->addWidget(button);
        connect(button, &QPushButton::clicked, this, entry.second);
    }
    toolbar->addStretch(1);
    toolbar->addWidget(new QLabel(repoPath, root));
    layout->addLayout(toolbar);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, root);
    auto *leftTabs = new QTabWidget(mainSplitter);
    changesTree = new QTreeWidget(leftTabs);
    changesTree->setHeaderLabels({QStringLiteral("Working tree changes"), QStringLiteral("Status")});
    changesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    changesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    leftTabs->addTab(changesTree, QStringLiteral("Changes"));
    branchesList = new QListWidget(leftTabs);
    leftTabs->addTab(branchesList, QStringLiteral("Branches"));
    historyList = new QListWidget(leftTabs);
    leftTabs->addTab(historyList, QStringLiteral("History"));

    rightTabs = new QTabWidget(mainSplitter);
    detailsView = new QPlainTextEdit(rightTabs);
    detailsView->setReadOnly(true);
    detailsView->setPlaceholderText(QStringLiteral("Select a changed file, branch, or commit to inspect it."));
    rightTabs->addTab(detailsView, QStringLiteral("Details / Diff"));

    auto *commandPanel = new QWidget(rightTabs);
    auto *commandLayout = new QVBoxLayout(commandPanel);
    commandLayout->setContentsMargins(4, 4, 4, 4);
    commandLayout->addWidget(new QLabel(QStringLiteral("Run a Git command in this repository. Suggestions include subcommands and common options."), commandPanel));
    commandEditor = new CodeEditor(commandPanel);
    commandEditor->setMaximumBlockCount(2);
    commandEditor->setFixedHeight(72);
    commandEditor->setPlaceholderText(QStringLiteral("git status --short --branch"));
    commandEditor->setPlainText(QStringLiteral("git status --short --branch"));
    new ShellScratchpad::Syntax::SyntaxHighlighter(commandEditor);
    completionController = new ShellScratchpad::Completion::CompletionController(commandEditor, commandEditor);
    completionController->setWorkingDirectory(repoPath);
    commandLayout->addWidget(commandEditor);
    auto *commandRow = new QHBoxLayout;
    auto *runButton = new QPushButton(QStringLiteral("Run Command"), commandPanel);
    runButton->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    auto *rawHelp = new QLabel(QStringLiteral("Ctrl+Enter runs raw Git arguments; no shell expansion."), commandPanel);
    commandRow->addWidget(runButton);
    commandRow->addWidget(rawHelp, 1);
    commandLayout->addLayout(commandRow);
    commandOutput = new QPlainTextEdit(commandPanel);
    commandOutput->setReadOnly(true);
    commandOutput->setMaximumBlockCount(3000);
    commandLayout->addWidget(commandOutput, 1);
    rightTabs->addTab(commandPanel, QStringLiteral("Git Command"));

    auto *credentialsPanel = new QWidget(rightTabs);
    auto *credentialsForm = new QFormLayout(credentialsPanel);
    userNameEdit = new QLineEdit(credentialsPanel);
    emailEdit = new QLineEdit(credentialsPanel);
    credentialHelperCombo = new QComboBox(credentialsPanel);
    credentialHelperCombo->addItem(QStringLiteral("Keep current helper"), QString());
    credentialHelperCombo->addItem(QStringLiteral("Git Credential Manager"), QStringLiteral("manager"));
    credentialHelperCombo->addItem(QStringLiteral("GNOME Keyring / libsecret"), QStringLiteral("/usr/lib/git-core/git-credential-libsecret"));
    credentialHelperCombo->addItem(QStringLiteral("Temporary in-memory cache"), QStringLiteral("cache --timeout=3600"));
    credentialHelperCombo->addItem(QStringLiteral("Plain-text store (not recommended)"), QStringLiteral("store"));
    credentialsForm->addRow(QStringLiteral("Commit name (global)"), userNameEdit);
    credentialsForm->addRow(QStringLiteral("Commit email (global)"), emailEdit);
    credentialsForm->addRow(QStringLiteral("Credential storage (global)"), credentialHelperCombo);
    auto *saveIdentityButton = new QPushButton(QStringLiteral("Save Git Identity"), credentialsPanel);
    auto *configureHelperButton = new QPushButton(QStringLiteral("Configure Helper"), credentialsPanel);
    auto *addCredentialButton = new QPushButton(QStringLiteral("Add / Update Credential…"), credentialsPanel);
    credentialsForm->addRow(saveIdentityButton, configureHelperButton);
    credentialsForm->addRow(addCredentialButton);
    credentialsForm->addRow(new QLabel(QStringLiteral("Credentials are passed to Git’s configured credential helper. The app does not save passwords in its own settings."), credentialsPanel));
    rightTabs->addTab(credentialsPanel, QStringLiteral("Identity & Credentials"));

    mainSplitter->addWidget(leftTabs);
    mainSplitter->addWidget(rightTabs);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 2);
    layout->addWidget(mainSplitter, 1);
    setCentralWidget(root);

    commandProcess = new QProcess(this);
    commandProcess->setWorkingDirectory(repoPath);
    connect(commandProcess, &QProcess::readyReadStandardOutput, this, [this] {
        appendOutput(QString::fromLocal8Bit(commandProcess->readAllStandardOutput()));
    });
    connect(commandProcess, &QProcess::readyReadStandardError, this, [this] {
        appendOutput(QString::fromLocal8Bit(commandProcess->readAllStandardError()));
    });
    connect(commandProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        appendOutput(QStringLiteral("\n[git exited with code %1]\n").arg(code));
        refreshAll();
    });
    connect(runButton, &QPushButton::clicked, this, [this] { runRawCommand(); });
    connect(commandEditor, &QPlainTextEdit::textChanged, this, [this] {
        if (commandEditor->toPlainText().contains(QLatin1Char('\n'))) {
            QTextCursor cursor = commandEditor->textCursor();
            cursor.movePosition(QTextCursor::End);
            commandEditor->setTextCursor(cursor);
            commandEditor->textCursor().deletePreviousChar();
        }
    });
    connect(changesTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!selectedPath().isEmpty()) {
            showDiff(selectedPath());
            rightTabs->setCurrentIndex(0);
        }
    });
    connect(branchesList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item) return;
        QString branch = item->text().trimmed();
        if (branch.startsWith(QStringLiteral("* "))) branch.remove(0, 2);
        else if (branch.startsWith(QStringLiteral("remotes/"))) {
            QMessageBox::information(this, QStringLiteral("Remote branch"),
                QStringLiteral("Choose a local branch or create a tracking branch with the Git Command tab."));
            return;
        }
        runGit({QStringLiteral("switch"), branch});
    });
    connect(historyList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item) detailsView->setPlainText(gitOutput({QStringLiteral("show"), QStringLiteral("--stat"), item->data(Qt::UserRole).toString()}));
    });
    connect(saveIdentityButton, &QPushButton::clicked, this, [this] { saveIdentity(); });
    connect(configureHelperButton, &QPushButton::clicked, this, [this] { configureCredentialHelper(); });
    connect(addCredentialButton, &QPushButton::clicked, this, [this] { addCredential(); });
    changesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(changesTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = changesTree->itemAt(position);
        if (!item) return;
        changesTree->setCurrentItem(item);
        const QString path = item->data(0, Qt::UserRole).toString();
        const QString status = item->text(1);
        QMenu menu(this);
        if (status.contains(QLatin1Char('?')) || status.contains(QLatin1Char('A')) ||
            status.contains(QLatin1Char('M')) || status.contains(QLatin1Char('D')) || status.contains(QLatin1Char('R'))) {
            QAction *stage = menu.addAction(QStringLiteral("Stage"));
            connect(stage, &QAction::triggered, this, [this, path] { runGit({QStringLiteral("add"), QStringLiteral("--"), path}); });
        }
        const bool hasStagedChanges = status.size() >= 2 && status.at(0) != QLatin1Char(' ') && status.at(0) != QLatin1Char('?');
        if (hasStagedChanges) {
            QAction *unstage = menu.addAction(QStringLiteral("Unstage"));
            connect(unstage, &QAction::triggered, this, [this, path] {
                runGit({QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--"), path});
            });
        }
        menu.addSeparator();
        QAction *discard = menu.addAction(QStringLiteral("Discard working tree changes…"));
        const bool hasUnstagedChanges = status.size() >= 2 && status.at(1) != QLatin1Char(' ') && status != QStringLiteral("??");
        discard->setEnabled(hasUnstagedChanges);
        connect(discard, &QAction::triggered, this, [this, path] {
            if (QMessageBox::question(this, QStringLiteral("Discard changes"),
                    QStringLiteral("Discard unstaged changes to %1?").arg(path),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                runGit({QStringLiteral("restore"), QStringLiteral("--worktree"), QStringLiteral("--"), path});
        });
        menu.exec(changesTree->viewport()->mapToGlobal(position));
    });
    refreshAll();
}

QString GitWorkspaceWindow::gitOutput(const QStringList &arguments, int *exitCode) const
{
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments(arguments);
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        if (exitCode) *exitCode = -1;
        return QStringLiteral("Git command timed out.");
    }
    if (exitCode) *exitCode = process.exitCode();
    const QByteArray output = process.readAllStandardOutput() + process.readAllStandardError();
    return QString::fromLocal8Bit(output).trimmed();
}

void GitWorkspaceWindow::refreshAll()
{
    refreshStatus();
    refreshBranches();
    refreshHistory();
    refreshIdentity();
}

void GitWorkspaceWindow::refreshStatus()
{
    changesTree->clear();
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z")});
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForFinished(5000) || process.exitCode() != 0) {
        changesTree->addTopLevelItem(new QTreeWidgetItem({gitOutput({QStringLiteral("status")}), QString()}));
        return;
    }
    const QList<QByteArray> records = process.readAllStandardOutput().split('\0');
    for (qsizetype recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
        const QByteArray &record = records.at(recordIndex);
        if (record.size() < 4) continue;
        const QString xy = QString::fromLocal8Bit(record.left(2));
        const QString path = QString::fromLocal8Bit(record.mid(3));
        const bool hasSecondaryRenamePath = xy.contains(QLatin1Char('R')) || xy.contains(QLatin1Char('C'));
        const QString label = hasSecondaryRenamePath ? QStringLiteral("%1 (from %2)").arg(
            path, recordIndex + 1 < records.size() ? QString::fromLocal8Bit(records.at(recordIndex + 1)) : QString()) : path;
        auto *item = new QTreeWidgetItem({label, xy});
        item->setData(0, Qt::UserRole, path);
        changesTree->addTopLevelItem(item);
        if (hasSecondaryRenamePath)
            ++recordIndex;
    }
    if (changesTree->topLevelItemCount() == 0)
        changesTree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("Working tree clean"), QString()}));
}

void GitWorkspaceWindow::refreshBranches()
{
    branchesList->clear();
    const QString text = gitOutput({QStringLiteral("branch"), QStringLiteral("--all"), QStringLiteral("--no-color")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        branchesList->addItem(line.trimmed());
}

void GitWorkspaceWindow::refreshHistory()
{
    historyList->clear();
    const QString text = gitOutput({QStringLiteral("log"), QStringLiteral("-30"), QStringLiteral("--format=%h%x09%s")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList columns = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        auto *item = new QListWidgetItem(line);
        if (!columns.isEmpty()) item->setData(Qt::UserRole, columns.first());
        historyList->addItem(item);
    }
}

void GitWorkspaceWindow::refreshIdentity()
{
    userNameEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.name")}));
    emailEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.email")}));
}

void GitWorkspaceWindow::runGit(const QStringList &arguments)
{
    if (commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git is busy"), QStringLiteral("Wait for the current Git command to finish."));
        return;
    }
    QStringList fullArguments{QStringLiteral("-C"), repoPath};
    fullArguments.append(arguments);
    commandProcess->setArguments(fullArguments);
    commandProcess->setProgram(gitExecutable());
    commandProcess->setWorkingDirectory(repoPath);
    commandOutput->appendPlainText(QStringLiteral("$ git %1").arg(arguments.join(QLatin1Char(' '))));
    rightTabs->setCurrentIndex(1);
    commandProcess->start();
}

void GitWorkspaceWindow::runRawCommand()
{
    if (commandProcess->state() != QProcess::NotRunning) return;
    QStringList args = QProcess::splitCommand(commandEditor->toPlainText().trimmed());
    if (!args.isEmpty() && args.first() == QLatin1String("git")) args.removeFirst();
    if (args.isEmpty()) return;
    commandOutput->appendPlainText(QStringLiteral("$ git %1").arg(args.join(QLatin1Char(' '))));
    commandProcess->setProgram(gitExecutable());
    commandProcess->setArguments(args);
    commandProcess->setWorkingDirectory(repoPath);
    commandProcess->start();
}

QString GitWorkspaceWindow::selectedPath() const
{
    const auto items = changesTree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
}

void GitWorkspaceWindow::showDiff(const QString &path)
{
    if (path.isEmpty()) return;
    const QString unstagedDiff = gitOutput({QStringLiteral("diff"), QStringLiteral("--"), path});
    const QString stagedDiff = gitOutput({QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--"), path});
    QString diff = stagedDiff;
    if (!unstagedDiff.isEmpty())
        diff += (diff.isEmpty() ? QString() : QStringLiteral("\n\n")) + unstagedDiff;
    if (diff.isEmpty()) {
        const QStringList status = gitOutput({QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("--"), path})
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!status.isEmpty() && status.first().startsWith(QStringLiteral("??")))
            diff = gitOutput({QStringLiteral("diff"), QStringLiteral("--no-index"), QStringLiteral("--"),
                              QStringLiteral("/dev/null"), path});
    }
    detailsView->setPlainText(diff.isEmpty() ? QStringLiteral("No textual diff is available for this path.") : diff);
}

void GitWorkspaceWindow::saveIdentity()
{
    const QString name = userNameEdit->text().trimmed();
    const QString email = emailEdit->text().trimmed();
    if (name.isEmpty() || email.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Git identity required"), QStringLiteral("Enter both a commit name and email."));
        return;
    }
    int nameCode = -1;
    int emailCode = -1;
    const QString nameOutput = gitOutput({QStringLiteral("config"), QStringLiteral("--global"),
        QStringLiteral("user.name"), name}, &nameCode);
    const QString emailOutput = gitOutput({QStringLiteral("config"), QStringLiteral("--global"),
        QStringLiteral("user.email"), email}, &emailCode);
    if (nameCode != 0 || emailCode != 0)
        QMessageBox::warning(this, QStringLiteral("Could not save Git identity"), nameOutput + QLatin1Char('\n') + emailOutput);
    else
        statusBar()->showMessage(QStringLiteral("Git identity saved globally"), 3000);
}

void GitWorkspaceWindow::configureCredentialHelper()
{
    const QString helper = credentialHelperCombo->currentData().toString();
    if (helper.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Credential helper"),
            gitOutput({QStringLiteral("config"), QStringLiteral("--show-origin"), QStringLiteral("--get-all"), QStringLiteral("credential.helper")}));
        return;
    }
    int code = -1;
    const QString output = gitOutput({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("credential.helper"), helper}, &code);
    statusBar()->showMessage(code == 0 ? QStringLiteral("Credential helper configured") : output, 5000);
}

void GitWorkspaceWindow::addCredential()
{
    const QString helpers = gitOutput({QStringLiteral("config"), QStringLiteral("--get-all"),
                                       QStringLiteral("credential.helper")});
    if (helpers.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Credential helper not configured"),
            QStringLiteral("Choose and configure a credential helper first. Git will only store this credential through that helper."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Git credential"));
    auto *form = new QFormLayout(&dialog);
    auto *host = new QLineEdit(&dialog);
    auto *username = new QLineEdit(&dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Host (e.g. github.com)"), host);
    form->addRow(QStringLiteral("Username"), username);
    form->addRow(QStringLiteral("Token / password"), password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (host->text().trimmed().isEmpty() || username->text().isEmpty() || password->text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Credential incomplete"), QStringLiteral("Enter host, username, and token."));
        return;
    }
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("credential"), QStringLiteral("approve")});
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForStarted()) {
        QMessageBox::warning(this, QStringLiteral("Git unavailable"), process.errorString());
        return;
    }
    const QByteArray input = QByteArray("protocol=https\nhost=") + host->text().trimmed().toUtf8()
        + QByteArray("\nusername=") + username->text().toUtf8()
        + QByteArray("\npassword=") + password->text().toUtf8() + QByteArray("\n\n");
    process.write(input);
    process.closeWriteChannel();
    password->clear();
    if (!process.waitForFinished(10000) || process.exitCode() != 0)
        QMessageBox::warning(this, QStringLiteral("Credential was not stored"), QString::fromLocal8Bit(process.readAllStandardError()));
    else
        statusBar()->showMessage(QStringLiteral("Credential passed to Git’s configured helper"), 5000);
}

void GitWorkspaceWindow::appendOutput(const QString &text)
{
    if (!text.isEmpty()) commandOutput->moveCursor(QTextCursor::End), commandOutput->insertPlainText(text);
}
}
