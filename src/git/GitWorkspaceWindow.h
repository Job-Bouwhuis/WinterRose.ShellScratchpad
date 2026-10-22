#pragma once

#include <QMainWindow>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;

namespace ShellScratchpad::Completion { class CompletionController; }
namespace ShellScratchpad { class CodeEditor; }

namespace ShellScratchpad::Git
{
class GitWorkspaceWindow final : public QMainWindow
{
public:
    explicit GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent = nullptr);
    static bool isGitRepository(const QString &path);

private:
    QString repoPath;
    QTreeWidget *changesTree = nullptr;
    QTabWidget *rightTabs = nullptr;
    QListWidget *branchesList = nullptr;
    QListWidget *historyList = nullptr;
    QPlainTextEdit *detailsView = nullptr;
    CodeEditor *commandEditor = nullptr;
    QLineEdit *userNameEdit = nullptr;
    QLineEdit *emailEdit = nullptr;
    QComboBox *credentialHelperCombo = nullptr;
    QPlainTextEdit *commandOutput = nullptr;
    Completion::CompletionController *completionController = nullptr;
    QProcess *commandProcess = nullptr;

    void refreshAll();
    void refreshStatus();
    void refreshBranches();
    void refreshHistory();
    void refreshIdentity();
    void runGit(const QStringList &arguments);
    void runRawCommand();
    QString gitOutput(const QStringList &arguments, int *exitCode = nullptr) const;
    QString selectedPath() const;
    void showDiff(const QString &path);
    void addCredential();
    void saveIdentity();
    void configureCredentialHelper();
    void appendOutput(const QString &text);
};
}
