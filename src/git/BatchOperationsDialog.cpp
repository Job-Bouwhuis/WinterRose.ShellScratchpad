#include "BatchOperationsDialog.h"

#include "RepositoryOperationCoordinator.h"

#include <QComboBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>

namespace ShellScratchpad::Git
{
BatchOperationsDialog::BatchOperationsDialog(const QVector<WorkspaceRepository> &repositories, QWidget *parent,
                                             const QString &initialOperation)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Workspace Operations"));
    resize(820, 560);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Select repositories and preview their state before running an operation. Each repository receives its own Git operation."), this));
    repositoryTree = new QTreeWidget(this);
    repositoryTree->setHeaderLabels({QStringLiteral("Repository"), QStringLiteral("Branch"), QStringLiteral("Staged"), QStringLiteral("Unstaged"), QStringLiteral("State"), QStringLiteral("Commit message override")});
    repositoryTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
        repositoryTree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    repositoryTree->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    layout->addWidget(repositoryTree, 1);
    auto *operationRow = new QHBoxLayout;
    operationRow->addWidget(new QLabel(QStringLiteral("Operation:"), this));
    operationCombo = new QComboBox(this);
    operationCombo->addItem(QStringLiteral("Stage all unstaged changes"), QStringLiteral("stage"));
    operationCombo->addItem(QStringLiteral("Commit staged changes"), QStringLiteral("commit"));
    operationCombo->addItem(QStringLiteral("Synchronize with remotes"), QStringLiteral("sync"));
    operationCombo->addItem(QStringLiteral("Pull with rebase"), QStringLiteral("pull"));
    operationCombo->addItem(QStringLiteral("Push selected branches"), QStringLiteral("push"));
    if (!initialOperation.isEmpty()) {
        const int index = operationCombo->findData(initialOperation);
        if (index >= 0) {
            operationCombo->setCurrentIndex(index);
            operationCombo->setEnabled(false);
        }
    }
    operationRow->addWidget(operationCombo);
    operationRow->addStretch(1);
    layout->addLayout(operationRow);
    commitMessageEdit = new QPlainTextEdit(this);
    commitMessageEdit->setPlaceholderText(QStringLiteral("Shared commit message for the selected repositories"));
    commitMessageEdit->setMaximumHeight(100);
    commitMessageEdit->hide();
    layout->addWidget(commitMessageEdit);
    summaryLabel = new QLabel(QStringLiteral("Checking repository state…"), this);
    layout->addWidget(summaryLabel);
    progressBar = new QProgressBar(this);
    progressBar->setTextVisible(true);
    progressBar->hide();
    layout->addWidget(progressBar);
    outputView = new QPlainTextEdit(this);
    outputView->setReadOnly(true);
    outputView->setMaximumHeight(130);
    outputView->setPlaceholderText(QStringLiteral("Per-repository operation output"));
    layout->addWidget(outputView);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    runButton = new QPushButton(QStringLiteral("Run Stage"), this);
    cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    buttons->addWidget(runButton);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);

    for (const WorkspaceRepository &repository : repositories) {
        RepositoryRow row;
        row.repository = repository;
        row.item = new QTreeWidgetItem(repositoryTree);
        row.item->setFlags(row.item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        row.item->setCheckState(0, Qt::Checked);
        row.item->setText(0, QFileInfo(repository.path).fileName());
        row.item->setToolTip(0, repository.path);
        row.messageOverride = new QLineEdit(repositoryTree);
        row.messageOverride->setPlaceholderText(QStringLiteral("Use shared message"));
        row.messageOverride->hide();
        repositoryTree->setItemWidget(row.item, 5, row.messageOverride);
        rows.append(row);
        connect(row.messageOverride, &QLineEdit::textChanged, this, [this] { updateOperationControls(); });
        auto *syncProcess = new QProcess(this);
        rows.last().syncProcess = syncProcess;
        const int rowIndex = rows.size() - 1;
        connect(syncProcess, &QProcess::readyReadStandardOutput, this, [this, rowIndex] {
            const QString output = QString::fromLocal8Bit(rows[rowIndex].syncProcess->readAllStandardOutput());
            rows[rowIndex].syncOutput += output;
            rows[rowIndex].syncCommandOutput += output;
        });
        connect(syncProcess, &QProcess::readyReadStandardError, this, [this, rowIndex] {
            const QString output = QString::fromLocal8Bit(rows[rowIndex].syncProcess->readAllStandardError());
            rows[rowIndex].syncOutput += output;
            rows[rowIndex].syncCommandOutput += output;
            static const QRegularExpression percent(QStringLiteral("([0-9]{1,3}%)"));
            const auto match = percent.match(output);
            if (match.hasMatch())
                rows[rowIndex].item->setText(4, QStringLiteral("%1 %2").arg(rows[rowIndex].syncPhase, match.captured(1)));
        });
        connect(syncProcess, &QProcess::finished, this, [this, rowIndex](int code, QProcess::ExitStatus) {
            handleSyncCommand(rowIndex, code, rows[rowIndex].syncProcess->readAllStandardOutput() + rows[rowIndex].syncProcess->readAllStandardError());
        });
        connect(syncProcess, &QProcess::errorOccurred, this, [this, rowIndex](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                handleSyncCommand(rowIndex, -1, rows[rowIndex].syncProcess->errorString().toLocal8Bit(), true);
        });
    }
    process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const QByteArray output = process->readAllStandardOutput() + process->readAllStandardError();
        if (operationRunning) finishCurrentOperation(code, QString::fromLocal8Bit(output));
        else finishInspection(code, output);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        if (operationRunning) finishCurrentOperation(-1, process->errorString(), true);
        else {
            finishInspection(-1, process->errorString().toLocal8Bit(), true);
        }
    });
    connect(operationCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateOperationControls(); });
    connect(commitMessageEdit, &QPlainTextEdit::textChanged, this, [this] { updateOperationControls(); });
    connect(runButton, &QPushButton::clicked, this, [this] { startBatch(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (operationRunning) requestStop();
        else reject();
    });
    connect(repositoryTree, &QTreeWidget::itemChanged, this, [this] { updateOperationControls(); });
    updateOperationControls();
    inspectNextRepository();
}

void BatchOperationsDialog::inspectNextRepository()
{
    if (process->state() != QProcess::NotRunning) return;
    if (inspectIndex >= rows.size()) {
        preflightComplete = true;
        summaryLabel->setText(QStringLiteral("Preview ready. Stage adds all changes to each selected index; Commit creates separate commits in each selected repository."));
        updateOperationControls();
        return;
    }
    RepositoryRow &row = rows[inspectIndex++];
    inspectStage = 0;
    if (!QFileInfo(row.repository.path).isDir()) {
        row.inspected = true;
        updateRow(row, QStringLiteral("Missing"));
        inspectNextRepository();
        return;
    }
    row.item->setText(4, QStringLiteral("Checking…"));
    process->setProgram(QStringLiteral("git"));
    process->setArguments({QStringLiteral("-C"), row.repository.path, QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("--branch"), QStringLiteral("-z")});
    process->start();
}

void BatchOperationsDialog::finishInspection(int exitCode, const QByteArray &output, bool failedToStart)
{
    if (inspectIndex <= 0 || inspectIndex > rows.size()) return;
    RepositoryRow &row = rows[inspectIndex - 1];
    if (inspectStage == 0) {
        if (failedToStart || exitCode != 0) {
            row.inspected = true;
            row.available = false;
            updateRow(row, QStringLiteral("Unavailable"));
            inspectNextRepository();
            return;
        }
        row.available = true;
        const QList<QByteArray> fields = output.split('\0');
        for (qsizetype index = 0; index < fields.size(); ++index) {
            const QByteArray field = fields.at(index);
            if (field.startsWith("## ")) {
                row.branch = QString::fromLocal8Bit(field.mid(3));
                if (row.branch.startsWith(QStringLiteral("No commits yet on "))) row.branch.remove(0, QStringLiteral("No commits yet on ").size());
                const qsizetype upstreamStart = row.branch.indexOf(QStringLiteral("..."));
                if (upstreamStart >= 0) row.branch = row.branch.left(upstreamStart);
                continue;
            }
            if (field.size() < 2) continue;
            const QByteArray status = field.left(2);
            if (status == "??") ++row.unstaged;
            else {
                if (status.at(0) != ' ') ++row.staged;
                if (status.at(1) != ' ') ++row.unstaged;
                if (status.contains('U') || status == "AA" || status == "DD") row.conflicts = true;
                if (status.at(0) == 'R' || status.at(0) == 'C') ++index;
            }
        }
        inspectStage = 1;
        process->setArguments({QStringLiteral("-C"), row.repository.path, QStringLiteral("remote")});
        process->start();
        return;
    }
    if (inspectStage == 1) {
        row.remotes = QString::fromLocal8Bit(output).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        inspectStage = 2;
        process->setArguments({QStringLiteral("-C"), row.repository.path, QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
            QStringLiteral("--symbolic-full-name"), QStringLiteral("@{upstream}")});
        process->start();
        return;
    }
    if (!failedToStart && exitCode == 0) row.upstream = QString::fromLocal8Bit(output).trimmed();
    row.inspected = true;
    updateRow(row);
    inspectNextRepository();
}

void BatchOperationsDialog::updateRow(RepositoryRow &row, const QString &status)
{
    row.item->setText(1, row.branch.isEmpty() ? QStringLiteral("—") : row.branch);
    row.item->setText(2, QString::number(row.staged));
    row.item->setText(3, QString::number(row.unstaged));
    row.item->setText(4, !status.isEmpty() ? status : (RepositoryOperationCoordinator::isBusy(row.repository.path)
        ? QStringLiteral("Busy") : (row.conflicts ? QStringLiteral("Conflict") : (row.available ? QStringLiteral("Ready") : QStringLiteral("Unavailable")))));
    row.item->setCheckState(0, row.available && !RepositoryOperationCoordinator::isBusy(row.repository.path)
        ? row.item->checkState(0) : Qt::Unchecked);
}

void BatchOperationsDialog::updateOperationControls()
{
    const bool commit = operationCombo->currentData().toString() == QLatin1String("commit");
    const QString mode = operationCombo->currentData().toString();
    const bool sync = mode == QLatin1String("sync") || mode == QLatin1String("pull") || mode == QLatin1String("push");
    commitMessageEdit->setVisible(commit);
    repositoryTree->setColumnHidden(5, !commit);
    for (RepositoryRow &row : rows)
        if (row.messageOverride) {
            row.messageOverride->setVisible(commit);
            row.messageOverride->setEnabled(!operationRunning);
        }
    repositoryTree->setEnabled(!operationRunning);
    operationCombo->setEnabled(!operationRunning);
    commitMessageEdit->setEnabled(!operationRunning);
    const QString runLabel = mode == QLatin1String("pull") ? QStringLiteral("Run Pull")
        : (mode == QLatin1String("push") ? QStringLiteral("Run Push")
        : (commit ? QStringLiteral("Run Commit") : (sync ? QStringLiteral("Run Sync") : QStringLiteral("Run Stage"))));
    runButton->setText(runLabel);
    if (operationRunning || !preflightComplete) {
        runButton->setEnabled(false);
        return;
    }
    bool eligible = false;
    for (const RepositoryRow &row : rows) {
        if (row.item->checkState(0) != Qt::Checked || !row.available || row.conflicts || RepositoryOperationCoordinator::isBusy(row.repository.path)) continue;
        const bool canCommit = row.staged > 0 && (!commitMessageEdit->toPlainText().trimmed().isEmpty() || !row.messageOverride->text().trimmed().isEmpty());
        const bool canSync = row.available && !row.conflicts;
        if (commit ? canCommit : (sync ? canSync : row.unstaged > 0)) { eligible = true; break; }
    }
    runButton->setEnabled(eligible);
}

void BatchOperationsDialog::startBatch()
{
    if (operationRunning || !preflightComplete) return;
    int selected = 0;
    for (const RepositoryRow &row : rows)
        if (row.item->checkState(0) == Qt::Checked) ++selected;
    if (selected == 0) {
        summaryLabel->setText(QStringLiteral("Select at least one repository."));
        return;
    }
    operationRunning = true;
    stopAfterCurrent = false;
    finishedCount = 0;
    successfulCount = 0;
    runIndex = 0;
    progressBar->setRange(0, selected);
    progressBar->setValue(0);
    progressBar->show();
    outputView->clear();
    runButton->setEnabled(false);
    cancelButton->setText(QStringLiteral("Stop After Current"));
    summaryLabel->setText(QStringLiteral("Starting selected repositories…"));
    const QString mode = operationCombo->currentData().toString();
    if (mode == QLatin1String("sync") || mode == QLatin1String("pull") || mode == QLatin1String("push")) {
        syncRunningCount = 0;
        syncSucceededCount = 0;
        syncFailedCount = 0;
        syncSkippedCount = 0;
        for (RepositoryRow &row : rows) {
            row.syncFinished = false;
            row.syncOutput.clear();
            row.syncFailureHandled = false;
            row.syncPushAfterPull = false;
        }
        startSyncOperations();
        return;
    }
    startNextOperation();
}

void BatchOperationsDialog::startNextOperation()
{
    if (stopAfterCurrent) {
        while (runIndex < rows.size()) {
            RepositoryRow &row = rows[runIndex++];
            if (row.item->checkState(0) != Qt::Checked) continue;
            row.item->setText(4, QStringLiteral("Cancelled: not started"));
            ++finishedCount;
            progressBar->setValue(finishedCount);
        }
        finishBatch();
        return;
    }
    while (runIndex < rows.size() && rows.at(runIndex).item->checkState(0) != Qt::Checked) ++runIndex;
    if (runIndex >= rows.size()) { finishBatch(); return; }
    RepositoryRow &row = rows[runIndex++];
    const bool commit = operationCombo->currentData().toString() == QLatin1String("commit");
    const int relevantChanges = commit ? row.staged : row.unstaged;
    if (!row.available || row.conflicts || relevantChanges == 0) {
        row.item->setText(4, !row.available ? QStringLiteral("Skipped: unavailable") : (row.conflicts ? QStringLiteral("Skipped: resolve conflicts") : QStringLiteral("Skipped: nothing to do")));
        ++finishedCount;
        progressBar->setValue(finishedCount);
        startNextOperation();
        return;
    }
    if (!RepositoryOperationCoordinator::tryAcquire(row.repository.path)) {
        row.item->setText(4, QStringLiteral("Skipped: busy"));
        ++finishedCount;
        progressBar->setValue(finishedCount);
        startNextOperation();
        return;
    }
    row.running = true;
    row.item->setText(4, commit ? QStringLiteral("Committing…") : QStringLiteral("Staging…"));
    processErrorHandled = false;
    process->setProgram(QStringLiteral("git"));
    if (commit) {
        const QString message = row.messageOverride->text().trimmed().isEmpty()
            ? commitMessageEdit->toPlainText().trimmed() : row.messageOverride->text().trimmed();
        if (message.isEmpty()) {
            RepositoryOperationCoordinator::release(row.repository.path);
            row.running = false;
            row.item->setText(4, QStringLiteral("Skipped: no commit message"));
            ++finishedCount;
            progressBar->setValue(finishedCount);
            startNextOperation();
            return;
        }
        process->setArguments({QStringLiteral("-C"), row.repository.path, QStringLiteral("commit"), QStringLiteral("-m"), message});
    } else {
        process->setArguments({QStringLiteral("-C"), row.repository.path, QStringLiteral("add"), QStringLiteral("-A")});
    }
    process->start();
    summaryLabel->setText(QStringLiteral("%1 of %2 repositories complete").arg(finishedCount).arg(progressBar->maximum()));
}

void BatchOperationsDialog::startSyncOperations()
{
    constexpr int maxConcurrent = 3;
    if (stopAfterCurrent) {
        while (runIndex < rows.size()) {
            RepositoryRow &row = rows[runIndex++];
            if (row.item->checkState(0) != Qt::Checked) continue;
            row.item->setText(4, QStringLiteral("Cancelled: not started"));
            ++syncSkippedCount;
            ++finishedCount;
            progressBar->setValue(finishedCount);
        }
    } else {
        while (syncRunningCount < maxConcurrent && runIndex < rows.size()) {
            const int index = runIndex++;
            RepositoryRow &row = rows[index];
            if (row.item->checkState(0) != Qt::Checked) continue;
            if (!row.available) { finishSyncRepository(index, QStringLiteral("Skipped: unavailable"), false, true); continue; }
            if (row.conflicts) { finishSyncRepository(index, QStringLiteral("Skipped: resolve conflicts first"), false, true); continue; }
            if (row.remotes.isEmpty()) { finishSyncRepository(index, QStringLiteral("Skipped: no remote"), false, true); continue; }
            if (row.branch.isEmpty() || row.branch.startsWith(QStringLiteral("HEAD "))) { finishSyncRepository(index, QStringLiteral("Needs a named branch"), false, true); continue; }
            if (!RepositoryOperationCoordinator::tryAcquire(row.repository.path)) { finishSyncRepository(index, QStringLiteral("Skipped: busy"), false, true); continue; }
            ++syncRunningCount;
            row.running = true;
            row.syncOutput.clear();
            row.item->setText(4, QStringLiteral("Fetching…"));
            launchSyncCommand(index, QStringLiteral("fetch"), {QStringLiteral("-C"), row.repository.path,
                QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune"), QStringLiteral("--progress")});
        }
    }
    if (syncRunningCount == 0 && runIndex >= rows.size()) finishBatch();
    else summaryLabel->setText(QStringLiteral("%1 of %2 repositories complete; %3 syncing in parallel")
        .arg(finishedCount).arg(progressBar->maximum()).arg(syncRunningCount));
}

void BatchOperationsDialog::launchSyncCommand(int rowIndex, const QString &phase, const QStringList &arguments)
{
    RepositoryRow &row = rows[rowIndex];
    row.syncPhase = phase;
    row.syncCommandOutput.clear();
    row.syncFailureHandled = false;
    row.syncProcess->setProgram(QStringLiteral("git"));
    row.syncProcess->setArguments(arguments);
    row.syncProcess->start();
}

void BatchOperationsDialog::handleSyncCommand(int rowIndex, int exitCode, const QByteArray &output, bool failedToStart)
{
    if (rowIndex < 0 || rowIndex >= rows.size()) return;
    RepositoryRow &row = rows[rowIndex];
    if (row.syncFailureHandled || row.syncFinished) return;
    row.syncFailureHandled = true;
    const QString text = QString::fromLocal8Bit(output);
    row.syncOutput += text;
    const QString commandText = row.syncCommandOutput + text;
    const bool succeeded = !failedToStart && exitCode == 0;
    if (stopAfterCurrent) {
        if (succeeded && (row.syncPhase == QLatin1String("push") || row.syncPhase == QLatin1String("push-no-upstream")))
            finishSyncRepository(rowIndex, QStringLiteral("Synced"), true);
        else if (!succeeded)
            finishSyncRepository(rowIndex, QStringLiteral("Failed during %1").arg(row.syncPhase), false);
        else
            finishSyncRepository(rowIndex, QStringLiteral("Cancelled after %1; later steps were not started").arg(row.syncPhase), false, true);
        return;
    }
    if (!succeeded) {
        if (row.syncPhase == QLatin1String("pull")) {
            launchSyncCommand(rowIndex, QStringLiteral("conflict-check"), {QStringLiteral("-C"), row.repository.path,
                QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")});
            return;
        }
        finishSyncRepository(rowIndex, QStringLiteral("Failed during %1").arg(row.syncPhase), false);
        return;
    }
    if (row.syncPhase == QLatin1String("fetch")) {
        row.item->setText(4, QStringLiteral("Checking upstream…"));
        launchSyncCommand(rowIndex, QStringLiteral("upstream-after-fetch"), {QStringLiteral("-C"), row.repository.path,
            QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("--symbolic-full-name"), QStringLiteral("@{upstream}")});
        return;
    }
    if (row.syncPhase == QLatin1String("upstream-after-fetch")) {
        row.upstream = succeeded ? commandText.trimmed() : QString();
        if (row.upstream.isEmpty() && operationCombo->currentData().toString() == QLatin1String("pull")) {
            finishSyncRepository(rowIndex, QStringLiteral("Needs upstream configuration"), false, true);
            return;
        }
        const QStringList args = row.upstream.isEmpty()
            ? QStringList{QStringLiteral("-C"), row.repository.path, QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD"), QStringLiteral("--not"), QStringLiteral("--remotes")}
            : QStringList{QStringLiteral("-C"), row.repository.path, QStringLiteral("rev-list"), QStringLiteral("--left-right"), QStringLiteral("--count"), QStringLiteral("HEAD...@{upstream}")};
        launchSyncCommand(rowIndex, row.upstream.isEmpty() ? QStringLiteral("outgoing") : QStringLiteral("counts"), args);
        return;
    }
    if (row.syncPhase == QLatin1String("counts")) {
        const QString mode = operationCombo->currentData().toString();
        const QStringList counts = commandText.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (counts.size() < 2) { finishSyncRepository(rowIndex, QStringLiteral("Needs upstream configuration"), false, true); return; }
        bool aheadOk = false;
        bool behindOk = false;
        row.ahead = counts.at(0).toInt(&aheadOk);
        row.behind = counts.at(1).toInt(&behindOk);
        if (!aheadOk || !behindOk) { finishSyncRepository(rowIndex, QStringLiteral("Could not read ahead/behind state"), false); return; }
        row.item->setText(4, QStringLiteral("Ahead %1 · behind %2").arg(row.ahead).arg(row.behind));
        if (row.behind > 0 && (row.staged > 0 || row.unstaged > 0)) {
            finishSyncRepository(rowIndex, QStringLiteral("Skipped: clean or commit local changes before pulling"), false, true);
            return;
        }
        if (mode == QLatin1String("pull")) {
            if (row.behind > 0) {
                row.item->setText(4, QStringLiteral("Pulling with rebase…"));
                launchSyncCommand(rowIndex, QStringLiteral("pull"), {QStringLiteral("-C"), row.repository.path,
                    QStringLiteral("pull"), QStringLiteral("--rebase"), QStringLiteral("--progress")});
            } else finishSyncRepository(rowIndex, QStringLiteral("Already up to date"), true);
            return;
        }
        if (mode == QLatin1String("push")) {
            if (row.behind > 0) {
                enqueueSyncPrompt([this, rowIndex] {
                    RepositoryRow &promptRow = rows[rowIndex];
                    auto *prompt = new QMessageBox(QMessageBox::Warning, QStringLiteral("Upstream Commits Need Pulling"),
                        QStringLiteral("%1 has %2 upstream commit(s) not in this repository. Pull with rebase before pushing?")
                            .arg(promptRow.item->text(0)).arg(promptRow.behind), QMessageBox::NoButton, this);
                    QPushButton *syncThenPush = prompt->addButton(QStringLiteral("Pull, Then Push"), QMessageBox::AcceptRole);
                    prompt->addButton(QStringLiteral("Do Nothing"), QMessageBox::RejectRole);
                    prompt->setDefaultButton(syncThenPush);
                    connect(prompt, &QMessageBox::finished, this, [this, prompt, syncThenPush, rowIndex](int) {
                        RepositoryRow &decisionRow = rows[rowIndex];
                        if (stopAfterCurrent) {
                            finishSyncRepository(rowIndex, QStringLiteral("Cancelled after fetch; pull was not started"), false, true);
                        } else if (prompt->clickedButton() == syncThenPush) {
                            decisionRow.syncPushAfterPull = true;
                            decisionRow.item->setText(4, QStringLiteral("Pulling with rebase…"));
                            launchSyncCommand(rowIndex, QStringLiteral("pull"), {QStringLiteral("-C"), decisionRow.repository.path,
                                QStringLiteral("pull"), QStringLiteral("--rebase"), QStringLiteral("--progress")});
                        } else finishSyncRepository(rowIndex, QStringLiteral("Skipped: incoming commits need syncing"), false, true);
                        prompt->deleteLater();
                        syncPromptActive = false;
                        showNextSyncPrompt();
                    });
                    prompt->open();
                });
                return;
            }
            if (row.ahead > 0) {
                row.item->setText(4, QStringLiteral("Pushing…"));
                launchSyncCommand(rowIndex, QStringLiteral("push"), {QStringLiteral("-C"), row.repository.path,
                    QStringLiteral("push"), QStringLiteral("--progress")});
            } else finishSyncRepository(rowIndex, QStringLiteral("Nothing to push"), true);
            return;
        }
        if (row.behind > 0 && row.ahead > 0) {
            enqueueSyncPrompt([this, rowIndex] {
                RepositoryRow &promptRow = rows[rowIndex];
                auto *prompt = new QMessageBox(QMessageBox::Warning, QStringLiteral("Upstream Commits Need Pulling"),
                    QStringLiteral("%1 has %2 upstream commit(s) not in this repository. Sync with the upstream using Pull with Rebase before pushing?")
                        .arg(promptRow.item->text(0)).arg(promptRow.behind), QMessageBox::NoButton, this);
                QPushButton *syncThenPush = prompt->addButton(QStringLiteral("Sync Then Push"), QMessageBox::AcceptRole);
                prompt->addButton(QStringLiteral("Do Nothing"), QMessageBox::RejectRole);
                prompt->setDefaultButton(syncThenPush);
                connect(prompt, &QMessageBox::finished, this, [this, prompt, syncThenPush, rowIndex](int) {
                    RepositoryRow &decisionRow = rows[rowIndex];
                    if (stopAfterCurrent) {
                        finishSyncRepository(rowIndex, QStringLiteral("Cancelled after fetch; pull was not started"), false, true);
                    } else if (prompt->clickedButton() == syncThenPush) {
                        decisionRow.syncPushAfterPull = true;
                        decisionRow.item->setText(4, QStringLiteral("Pulling with rebase…"));
                        launchSyncCommand(rowIndex, QStringLiteral("pull"), {QStringLiteral("-C"), decisionRow.repository.path,
                            QStringLiteral("pull"), QStringLiteral("--rebase"), QStringLiteral("--progress")});
                    } else finishSyncRepository(rowIndex, QStringLiteral("Skipped: incoming commits need syncing"), false, true);
                    prompt->deleteLater();
                    syncPromptActive = false;
                    showNextSyncPrompt();
                });
                prompt->open();
            });
            return;
        }
        if (row.behind > 0) {
            row.item->setText(4, QStringLiteral("Pulling with rebase…"));
            launchSyncCommand(rowIndex, QStringLiteral("pull"), {QStringLiteral("-C"), row.repository.path,
                QStringLiteral("pull"), QStringLiteral("--rebase"), QStringLiteral("--progress")});
        } else if (row.ahead > 0) {
            row.item->setText(4, QStringLiteral("Pushing…"));
            launchSyncCommand(rowIndex, QStringLiteral("push"), {QStringLiteral("-C"), row.repository.path, QStringLiteral("push"), QStringLiteral("--progress")});
        } else finishSyncRepository(rowIndex, QStringLiteral("Up to date"), true);
        return;
    }
    if (row.syncPhase == QLatin1String("outgoing")) {
        bool ok = false;
        const int outgoing = commandText.trimmed().toInt(&ok);
        if (!ok || outgoing <= 0) { finishSyncRepository(rowIndex, QStringLiteral("Needs upstream configuration"), false, true); return; }
        QString remote;
        if (row.remotes.size() == 1) remote = row.remotes.first();
        else {
            enqueueSyncPrompt([this, rowIndex] {
                RepositoryRow &promptRow = rows[rowIndex];
                auto *prompt = new QInputDialog(this);
                prompt->setWindowTitle(QStringLiteral("Choose Remote — %1").arg(promptRow.item->text(0)));
                prompt->setLabelText(QStringLiteral("Remote for this branch:"));
                prompt->setComboBoxItems(promptRow.remotes);
                prompt->setComboBoxEditable(false);
                connect(prompt, &QInputDialog::finished, this, [this, prompt, rowIndex](int result) {
                    RepositoryRow &decisionRow = rows[rowIndex];
                    if (stopAfterCurrent) {
                        finishSyncRepository(rowIndex, QStringLiteral("Cancelled after fetch; push was not started"), false, true);
                    } else if (result == QDialog::Accepted && !prompt->textValue().isEmpty()) {
                        decisionRow.remotePushTarget = prompt->textValue();
                        decisionRow.item->setText(4, QStringLiteral("Pushing…"));
                        launchSyncCommand(rowIndex, QStringLiteral("push-no-upstream"), {QStringLiteral("-C"), decisionRow.repository.path,
                            QStringLiteral("push"), QStringLiteral("--set-upstream"), decisionRow.remotePushTarget, decisionRow.branch, QStringLiteral("--progress")});
                    } else finishSyncRepository(rowIndex, QStringLiteral("Skipped: remote not selected"), false, true);
                    prompt->deleteLater();
                    syncPromptActive = false;
                    showNextSyncPrompt();
                });
                prompt->open();
            });
            return;
        }
        row.remotePushTarget = remote;
        row.item->setText(4, QStringLiteral("Pushing…"));
        launchSyncCommand(rowIndex, QStringLiteral("push-no-upstream"), {QStringLiteral("-C"), row.repository.path,
            QStringLiteral("push"), QStringLiteral("--set-upstream"), remote, row.branch, QStringLiteral("--progress")});
        return;
    }
    if (row.syncPhase == QLatin1String("pull")) {
        if (row.syncPushAfterPull && row.ahead > 0) {
            row.item->setText(4, QStringLiteral("Pushing…"));
            launchSyncCommand(rowIndex, QStringLiteral("push"), {QStringLiteral("-C"), row.repository.path, QStringLiteral("push"), QStringLiteral("--progress")});
        } else finishSyncRepository(rowIndex, QStringLiteral("Synced"), true);
        return;
    }
    if (row.syncPhase == QLatin1String("conflict-check")) {
        finishSyncRepository(rowIndex, commandText.trimmed().isEmpty() ? QStringLiteral("Failed during pull --rebase") : QStringLiteral("Needs conflict resolution"), false);
        return;
    }
    if (row.syncPhase == QLatin1String("push") || row.syncPhase == QLatin1String("push-no-upstream")) {
        finishSyncRepository(rowIndex, QStringLiteral("Synced"), true);
        return;
    }
}

void BatchOperationsDialog::finishSyncRepository(int rowIndex, const QString &status, bool succeeded, bool skipped)
{
    RepositoryRow &row = rows[rowIndex];
    if (row.syncFinished) return;
    row.syncFinished = true;
    if (row.running) {
        row.running = false;
        RepositoryOperationCoordinator::release(row.repository.path);
        if (syncRunningCount > 0) --syncRunningCount;
    }
    if (succeeded) ++syncSucceededCount;
    else if (skipped) ++syncSkippedCount;
    else ++syncFailedCount;
    row.item->setText(4, status);
    outputView->appendPlainText(QStringLiteral("[%1] %2\n%3").arg(row.item->text(0), status,
        row.syncOutput.trimmed().isEmpty() ? QStringLiteral("(no output)") : row.syncOutput.trimmed()));
    ++finishedCount;
    progressBar->setValue(finishedCount);
    startSyncOperations();
}

void BatchOperationsDialog::enqueueSyncPrompt(const std::function<void()> &prompt)
{
    syncPromptQueue.enqueue(prompt);
    showNextSyncPrompt();
}

void BatchOperationsDialog::showNextSyncPrompt()
{
    if (syncPromptActive || syncPromptQueue.isEmpty()) return;
    syncPromptActive = true;
    const std::function<void()> prompt = syncPromptQueue.dequeue();
    QTimer::singleShot(0, this, [this, prompt] {
        prompt();
    });
}

void BatchOperationsDialog::finishCurrentOperation(int exitCode, const QString &output, bool failedToStart)
{
    if (processErrorHandled) return;
    processErrorHandled = true;
    RepositoryRow &row = rows[runIndex - 1];
    row.running = false;
    RepositoryOperationCoordinator::release(row.repository.path);
    row.output = output;
    const bool succeeded = !failedToStart && exitCode == 0;
    row.item->setText(4, succeeded ? QStringLiteral("Completed") : QStringLiteral("Failed"));
    if (succeeded) ++successfulCount;
    outputView->appendPlainText(QStringLiteral("[%1] %2\n%3").arg(row.item->text(0), succeeded ? QStringLiteral("Completed") : QStringLiteral("Failed"),
        output.isEmpty() ? QStringLiteral("(no output)") : output));
    ++finishedCount;
    progressBar->setValue(finishedCount);
    startNextOperation();
}

void BatchOperationsDialog::finishBatch()
{
    operationRunning = false;
    const int selected = progressBar->maximum();
    const QString mode = operationCombo->currentData().toString();
    if (mode == QLatin1String("sync") || mode == QLatin1String("pull") || mode == QLatin1String("push")) {
        const QString verb = mode == QLatin1String("pull") ? QStringLiteral("pulled")
            : (mode == QLatin1String("push") ? QStringLiteral("pushed") : QStringLiteral("synced"));
        summaryLabel->setText(QStringLiteral("Finished: %1 %2, %3 failed, %4 skipped%5.")
            .arg(syncSucceededCount).arg(verb).arg(syncFailedCount).arg(syncSkippedCount)
            .arg(stopAfterCurrent ? QStringLiteral("; remaining repositories were not started") : QString()));
    } else {
        summaryLabel->setText(QStringLiteral("Finished: %1 succeeded, %2 skipped or failed%3.")
            .arg(successfulCount).arg(qMax(0, selected - successfulCount))
            .arg(stopAfterCurrent ? QStringLiteral("; remaining repositories were not started") : QString()));
    }
    runButton->setText(QStringLiteral("Finished"));
    runButton->setEnabled(false);
    cancelButton->setText(QStringLiteral("Close"));
    cancelButton->setEnabled(true);
    disconnect(cancelButton, nullptr, this, nullptr);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::accept);
}

void BatchOperationsDialog::requestStop()
{
    stopAfterCurrent = true;
    cancelButton->setEnabled(false);
    summaryLabel->setText(QStringLiteral("Waiting for active Git steps to finish; queued repositories will not start."));
}

void BatchOperationsDialog::done(int result)
{
    if (operationRunning) {
        requestStop();
        return;
    }
    QDialog::done(result);
}

void BatchOperationsDialog::closeEvent(QCloseEvent *event)
{
    if (operationRunning) {
        requestStop();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
}
