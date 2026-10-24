#pragma once

#include "RepositoryWorkspace.h"

#include <QDialog>
#include <QByteArray>
#include <QVector>
#include <QQueue>
#include <functional>

class QComboBox;
class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QProgressBar;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace ShellScratchpad::Git
{
class BatchOperationsDialog final : public QDialog
{
public:
    explicit BatchOperationsDialog(const QVector<WorkspaceRepository> &repositories, QWidget *parent = nullptr,
                                   const QString &initialOperation = QString());

private:
    struct RepositoryRow
    {
        WorkspaceRepository repository;
        QTreeWidgetItem *item = nullptr;
        QLineEdit *messageOverride = nullptr;
        QString branch;
        QString upstream;
        QStringList remotes;
        QString syncPhase;
        QString syncOutput;
        QString syncCommandOutput;
        QString remotePushTarget;
        int staged = 0;
        int unstaged = 0;
        int ahead = 0;
        int behind = 0;
        bool available = false;
        bool conflicts = false;
        bool inspected = false;
        bool running = false;
        bool syncFinished = false;
        bool syncPushAfterPull = false;
        bool syncFailureHandled = false;
        QProcess *syncProcess = nullptr;
        QString output;
    };

    QVector<RepositoryRow> rows;
    QTreeWidget *repositoryTree = nullptr;
    QComboBox *operationCombo = nullptr;
    QPlainTextEdit *commitMessageEdit = nullptr;
    QPlainTextEdit *outputView = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *summaryLabel = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QProcess *process = nullptr;
    int inspectIndex = 0;
    int runIndex = 0;
    int finishedCount = 0;
    int successfulCount = 0;
    bool preflightComplete = false;
    bool operationRunning = false;
    bool stopAfterCurrent = false;
    bool processErrorHandled = false;
    int inspectStage = 0;
    int syncRunningCount = 0;
    int syncSucceededCount = 0;
    int syncFailedCount = 0;
    int syncSkippedCount = 0;
    bool syncPromptActive = false;
    QQueue<std::function<void()>> syncPromptQueue;

    void inspectNextRepository();
    void finishInspection(int exitCode, const QByteArray &output, bool failedToStart = false);
    void updateRow(RepositoryRow &row, const QString &status = QString());
    void updateOperationControls();
    void startBatch();
    void startNextOperation();
    void finishCurrentOperation(int exitCode, const QString &output, bool failedToStart = false);
    void finishBatch();
    void requestStop();
    void startSyncOperations();
    void launchSyncCommand(int rowIndex, const QString &phase, const QStringList &arguments);
    void handleSyncCommand(int rowIndex, int exitCode, const QByteArray &output, bool failedToStart = false);
    void finishSyncRepository(int rowIndex, const QString &status, bool succeeded, bool skipped = false);
    void enqueueSyncPrompt(const std::function<void()> &prompt);
    void showNextSyncPrompt();
    void done(int result) override;
    void closeEvent(QCloseEvent *event) override;
};
}
