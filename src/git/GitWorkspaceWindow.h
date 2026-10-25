#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QHash>
#include "RepositoryWorkspace.h"

class QComboBox;
class QAction;
class QCloseEvent;
class QEvent;
class QLineEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QWidget;
class QTimer;
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
class ImagePreviewLabel;
class HistoryGraphWidget;
class GitWorkspaceWindow final : public QMainWindow
{
public:
    explicit GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent = nullptr);
    static bool isGitRepository(const QString &path);
    static bool showCloneDialog(const QString &parentDirectory, QWidget *parent = nullptr);
    static bool showInitDialog(const QString &directory, QWidget *parent = nullptr);

private:
    QString repoPath;
    QString operationLockPath;
    bool operationLockHeld = false;
    QTreeWidget *changesTree = nullptr;
    QTabWidget *rightTabs = nullptr;
    QToolButton *branchButton = nullptr;
    QToolButton *commitButton = nullptr;
    QToolButton *stageAllButton = nullptr;
    QToolButton *syncButton = nullptr;
    QToolButton *pullButton = nullptr;
    QToolButton *pushButton = nullptr;
    QToolButton *moreButton = nullptr;
    QToolButton *applyStashButton = nullptr;
    QToolButton *conflictsButton = nullptr;
    QToolButton *actionRepositoriesButton = nullptr;
    QAction *stageAllShortcutAction = nullptr;
    QAction *commitShortcutAction = nullptr;
    QAction *pushShortcutAction = nullptr;
    QAction *pullShortcutAction = nullptr;
    QAction *fetchShortcutAction = nullptr;
    QAction *stashShortcutAction = nullptr;
    QAction *resolveShortcutAction = nullptr;
    QLabel *branchStatusLabel = nullptr;
    QProgressBar *gitProgressBar = nullptr;
    QTimer *lastRepositoryFocusTimer = nullptr;
    QMenu *gitActionsMenu = nullptr;
    QMenu *recentRepositoriesMenu = nullptr;
    QVector<QPointer<QWidget>> busyDisabledWidgets;
    QTreeWidget *branchesList = nullptr;
    QTreeWidget *workspaceRepositoryList = nullptr;
    RepositoryWorkspace workspace;
    bool workspaceAmbiguous = false;
    QListWidget *historyList = nullptr;
    QStackedWidget *historyViews = nullptr;
    QPushButton *historyOverviewButton = nullptr;
    HistoryGraphWidget *historyGraph = nullptr;
    QListWidget *remotesList = nullptr;
    QTreeWidget *repositorySettingsTree = nullptr;
    QHash<QString, QComboBox *> repositorySettingControls;
    QPushButton *applyRepositorySettingsButton = nullptr;
    QWidget *commandPanel = nullptr;
    QWidget *remotesPanel = nullptr;
    QWidget *credentialsPanel = nullptr;
    QWidget *repositorySettingsPanel = nullptr;
    QWidget *previousRightPage = nullptr;
    QListWidget *changeGroupsList = nullptr;
    QPlainTextEdit *detailsView = nullptr;
    QPlainTextEdit *commitMessageView = nullptr;
    QStackedWidget *detailsStack = nullptr;
    QTextEdit *originalFileView = nullptr;
    QTextEdit *workingFileView = nullptr;
    ImagePreviewLabel *originalImageView = nullptr;
    ImagePreviewLabel *workingImageView = nullptr;
    QStackedWidget *originalViewStack = nullptr;
    QStackedWidget *workingViewStack = nullptr;
    QWidget *svgViewModePanel = nullptr;
    QComboBox *svgViewModeCombo = nullptr;
    QListWidget *diffFilesList = nullptr;
    QLabel *diffSummaryLabel = nullptr;
    QLabel *originalDiffLabel = nullptr;
    QLabel *changedDiffLabel = nullptr;
    CodeEditor *commandEditor = nullptr;
    QLineEdit *userNameEdit = nullptr;
    QLineEdit *emailEdit = nullptr;
    QComboBox *credentialHelperCombo = nullptr;
    QLineEdit *credentialWalletEdit = nullptr;
    QLineEdit *credentialFolderEdit = nullptr;
    QPushButton *createCredentialWalletButton = nullptr;
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
    QString currentImageDiffPath;
    QByteArray originalImageDiffBytes;
    QByteArray workingImageDiffBytes;
    bool currentDiffIsImage = false;
    bool currentDiffIsSvg = false;
    bool originalImageMissing = false;
    bool workingImageMissing = false;
    QStringList conflictedFiles;

    void refreshAll();
    QString readStatusSnapshot() const;
    void pollRepositoryStatus();
    void refreshStatus();
    void refreshBranches();
    void refreshHistory();
    void refreshHistoryGraph();
    void showHistoryOverview();
    void refreshRemotes();
    void refreshIdentity();
    void createBranchFromHere(const QString &branch, bool remote, bool temporaryMerge = false);
    void acceptTemporaryMergeBranch(const QString &branch);
    void switchToBranch(const QString &branch, bool remote);
    void showBranchContextMenu(const QString &branch, bool remote, const QPoint &globalPosition);
    void showHistoryContextMenu(QListWidgetItem *item, const QPoint &globalPosition);
    void resolveConflictFile(const QString &path);
    void showMergeConflictsDialog();
    void showCommitDialog();
    void addRemote();
    void createRemoteRepository();
    void openRemoteInBrowser(const QString &name);
    void changeRemoteUrl(const QString &name);
    void removeRemote(const QString &name);
    void pullCurrentBranch();
    void pushCurrentBranch();
    void runGit(const QStringList &arguments);
    void runRawCommand();
    QString gitOutput(const QStringList &arguments, int *exitCode = nullptr) const;
    QString selectedPath() const;
    void showDiff(const QString &path);
    void showImageDiff(const QString &path, const QByteArray &originalBytes, const QByteArray &workingBytes,
                       bool originalMissing, bool workingMissing);
    void displayImageDiff();
    void showCommit(const QString &hash);
    void showCommitFile(const QString &path, const QString &oldPath = QString());
    void addCredential();
    void manageSshKeys();
    void saveIdentity();
    void configureCredentialHelper();
    void createCredentialWallet();
    void refreshRepositorySettings();
    void applyRepositorySettings();
    void editRepositorySetting(QTreeWidgetItem *item);
    void addRepositorySetting();
    void removeRepositorySetting(QTreeWidgetItem *item);
    void refreshWorkspaceRepositoryList();
    void recordRecentWorkspace();
    void refreshRecentRepositoriesMenu();
    void addWorkspaceRepository();
    void removeWorkspaceRepository();
    void locateWorkspaceRepository();
    void switchRepository(const QString &repositoryId);
    void showBatchOperations(const QString &initialOperation = QString());
    QVector<WorkspaceRepository> actionRepositories() const;
    bool actionsTargetOnlyActiveRepository() const;
    void updateActionRepositoriesButton();
    void appendOutput(const QString &text);
    void sendCommandInput();
    void updateToolbarState();
    void updateStatusBar();
    void setGitBusy(bool busy, bool showProgress = false);
    void updateGitProgress(const QString &output);
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
};
}
