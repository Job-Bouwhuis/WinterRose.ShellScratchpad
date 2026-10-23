#pragma once

#include <QMainWindow>
#include <QStringList>

class QComboBox;
class QAction;
class QLineEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QProcess;
class QPoint;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;
class QStackedWidget;
class QTextDocument;
class QTextEdit;

namespace ShellScratchpad::Completion { class CompletionController; }
namespace ShellScratchpad { class CodeEditor; }
namespace ShellScratchpad::Output { class TerminalOutput; }

namespace ShellScratchpad::Git
{
class GitWorkspaceWindow final : public QMainWindow
{
public:
    explicit GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent = nullptr);
    static bool isGitRepository(const QString &path);
    static bool showCloneDialog(const QString &parentDirectory, QWidget *parent = nullptr);

private:
    QString repoPath;
    QTreeWidget *changesTree = nullptr;
    QTabWidget *rightTabs = nullptr;
    QToolButton *branchButton = nullptr;
    QToolButton *commitButton = nullptr;
    QToolButton *commitPushButton = nullptr;
    QToolButton *stageAllButton = nullptr;
    QToolButton *syncButton = nullptr;
    QToolButton *pullButton = nullptr;
    QToolButton *pushButton = nullptr;
    QToolButton *moreButton = nullptr;
    QToolButton *conflictsButton = nullptr;
    QAction *stageAllShortcutAction = nullptr;
    QAction *commitShortcutAction = nullptr;
    QAction *pushShortcutAction = nullptr;
    QAction *pullShortcutAction = nullptr;
    QAction *fetchShortcutAction = nullptr;
    QAction *stashShortcutAction = nullptr;
    QAction *resolveShortcutAction = nullptr;
    QLabel *branchStatusLabel = nullptr;
    QListWidget *branchesList = nullptr;
    QListWidget *historyList = nullptr;
    QListWidget *remotesList = nullptr;
    QListWidget *changeGroupsList = nullptr;
    QPlainTextEdit *detailsView = nullptr;
    QStackedWidget *detailsStack = nullptr;
    QTextEdit *originalFileView = nullptr;
    QTextEdit *workingFileView = nullptr;
    QListWidget *diffFilesList = nullptr;
    QLabel *diffSummaryLabel = nullptr;
    QLabel *originalDiffLabel = nullptr;
    QLabel *changedDiffLabel = nullptr;
    CodeEditor *commandEditor = nullptr;
    QLineEdit *userNameEdit = nullptr;
    QLineEdit *emailEdit = nullptr;
    QComboBox *credentialHelperCombo = nullptr;
    QPlainTextEdit *commandOutput = nullptr;
    QLineEdit *commandInput = nullptr;
    QTextDocument *commandOutputDocument = nullptr;
    Output::TerminalOutput *terminalOutputRenderer = nullptr;
    Completion::CompletionController *completionController = nullptr;
    QProcess *commandProcess = nullptr;
    bool actionCommandActive = false;
    bool hasStagedChanges = false;
    bool hasUnstagedChanges = false;
    bool hasStashes = false;
    bool hasRemote = false;
    bool hasUpstream = false;
    bool hasUnpushedCommits = false;
    bool hasConflicts = false;
    bool hadConflictsBeforeCommand = false;
    bool commitPushPending = false;
    bool pushAfterSyncPending = false;
    QStringList deferredPushArguments;
    int stagedPathCount = 0;
    int unstagedPathCount = 0;
    QString actionOutput;
    QString lastObservedStatus;
    QString lastObservedDiffFile;
    QString currentDiffCommitHash;
    QStringList conflictedFiles;

    void refreshAll();
    QString readStatusSnapshot() const;
    void pollRepositoryStatus();
    void refreshStatus();
    void refreshBranches();
    void refreshHistory();
    void refreshRemotes();
    void refreshIdentity();
    void switchToBranch(const QString &branch, bool remote);
    void showBranchContextMenu(const QString &branch, bool remote, const QPoint &globalPosition);
    void showHistoryContextMenu(QListWidgetItem *item, const QPoint &globalPosition);
    void resolveConflictFile(const QString &path);
    void showMergeConflictsDialog();
    void showCommitDialog();
    void addRemote();
    void changeRemoteUrl(const QString &name);
    void removeRemote(const QString &name);
    void pullCurrentBranch();
    void pushCurrentBranch();
    void runGit(const QStringList &arguments);
    void runRawCommand();
    QString gitOutput(const QStringList &arguments, int *exitCode = nullptr) const;
    QString selectedPath() const;
    void showDiff(const QString &path);
    void showCommit(const QString &hash);
    void showCommitFile(const QString &path, const QString &oldPath = QString());
    void addCredential();
    void saveIdentity();
    void configureCredentialHelper();
    void appendOutput(const QString &text);
    void sendCommandInput();
    void updateToolbarState();
    void updateStatusBar();
};
}
