#include "git/RepositoryWorkspace.h"
#include "git/RepositoryOperationCoordinator.h"
#include "git/BatchOperationsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTreeWidget>
#include <QTimer>

#include <iostream>
#include <functional>

using ShellScratchpad::Git::RepositoryWorkspace;
using ShellScratchpad::Git::RepositoryWorkspaceStore;
using ShellScratchpad::Git::RepositoryOperationCoordinator;
using ShellScratchpad::Git::BatchOperationsDialog;

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool runGit(const QString &path, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    QStringList command{QStringLiteral("-C"), path};
    command.append(arguments);
    process.setArguments(command);
    process.start();
    return process.waitForFinished(5000) && process.exitCode() == 0;
}

bool makeRepository(const QString &path)
{
    return QDir().mkpath(path) && runGit(path, {QStringLiteral("init"), QStringLiteral("-q")});
}
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), "temporary directory is available")) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("ShellScratchpadTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RepositoryWorkspaceTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.filePath(QStringLiteral("settings")));
    QSettings().clear();

    const QString firstPath = temporary.filePath(QStringLiteral("Engine.Core"));
    const QString secondPath = temporary.filePath(QStringLiteral("Engine.Rendering"));
    if (!require(makeRepository(firstPath), "first local Git repository initializes")) return 1;
    if (!require(makeRepository(secondPath), "second local Git repository initializes")) return 1;

    RepositoryWorkspace workspace = RepositoryWorkspaceStore::workspaceForRepository(firstPath, true);
    if (!require(workspace.repositories.size() == 1, "opening a repository creates its single-repository workspace")) return 1;
    QString error;
    if (!require(RepositoryWorkspaceStore::addRepository(workspace, secondPath, &error), "second repository is added")) return 1;
    if (!require(workspace.repositories.size() == 2, "workspace contains two repositories")) return 1;
    workspace.actionRepositoryIds = {workspace.repositories.last().id};
    RepositoryWorkspaceStore::saveWorkspace(workspace);

    const QString nestedPath = QDir(secondPath).filePath(QStringLiteral("nested"));
    QDir().mkpath(nestedPath);
    if (!require(!RepositoryWorkspaceStore::addRepository(workspace, nestedPath, &error), "a path inside an existing repository is rejected as duplicate")) return 1;
    const QString aliasPath = temporary.filePath(QStringLiteral("RenderingAlias"));
    if (QFile::link(secondPath, aliasPath))
        if (!require(!RepositoryWorkspaceStore::addRepository(workspace, aliasPath, &error), "a symbolic-link alias is rejected as duplicate")) return 1;

    RepositoryWorkspace restored = RepositoryWorkspaceStore::workspaceForRepository(secondPath, false);
    if (!require(restored.id == workspace.id && restored.repositories.size() == 2
        && restored.actionRepositoryIds == workspace.actionRepositoryIds,
        "opening any member restores workspace action targets")) return 1;
    const QString movedPath = temporary.filePath(QStringLiteral("moved/Engine.Rendering"));
    QDir().mkpath(QFileInfo(movedPath).absolutePath());
    if (!require(QDir().rename(secondPath, movedPath), "repository can be moved in the local fixture")) return 1;
    if (!require(RepositoryWorkspaceStore::repositoryRoot(restored.repositories.last().path).isEmpty(), "a moved repository is recognized as unavailable at its old path")) return 1;
    if (!require(RepositoryWorkspaceStore::updateRepositoryPath(restored, restored.repositories.last().id, movedPath, &error), "a located moved repository updates its stored path")) return 1;
    if (!require(RepositoryWorkspaceStore::removeRepository(restored, restored.repositories.last().id), "repository membership can be removed")) return 1;
    if (!require(restored.actionRepositoryIds.isEmpty(), "removing a repository also removes its saved action selection")) return 1;
    if (!require(QFileInfo::exists(movedPath) && RepositoryWorkspaceStore::repositoryRoot(movedPath) == QFileInfo(movedPath).canonicalFilePath(),
                 "removing membership leaves the repository files intact")) return 1;

    if (!require(RepositoryOperationCoordinator::tryAcquire(firstPath), "repository operation lock can be acquired")) return 1;
    if (!require(!RepositoryOperationCoordinator::tryAcquire(firstPath), "a conflicting operation for the same repository is rejected")) return 1;
    if (!require(RepositoryOperationCoordinator::tryAcquire(movedPath), "an unrelated repository remains independently operable")) return 1;
    RepositoryOperationCoordinator::release(firstPath);
    RepositoryOperationCoordinator::release(movedPath);

    const QString batchPath = temporary.filePath(QStringLiteral("Batch.Repository"));
    if (!require(makeRepository(batchPath), "batch-operation fixture initializes")) return 1;
    if (!require(runGit(batchPath, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")} ), "batch fixture has a commit author name")) return 1;
    if (!require(runGit(batchPath, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")} ), "batch fixture has a commit author email")) return 1;
    QFile batchFile(QDir(batchPath).filePath(QStringLiteral("change.txt")));
    if (!require(batchFile.open(QIODevice::WriteOnly | QIODevice::Text), "batch fixture file opens")) return 1;
    batchFile.write("workspace batch change\n");
    batchFile.close();
    ShellScratchpad::Git::WorkspaceRepository batchRepository;
    batchRepository.id = QStringLiteral("batch-test");
    batchRepository.path = batchPath;

    const auto waitFor = [&application](const std::function<bool()> &predicate) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 10000) {
            application.processEvents();
            QThread::msleep(5);
        }
        application.processEvents();
        return predicate();
    };
    BatchOperationsDialog stageDialog({batchRepository});
    stageDialog.show();
    const auto findRunButton = [](BatchOperationsDialog &dialog, const QString &text) {
        for (QPushButton *button : dialog.findChildren<QPushButton *>())
            if (button->text() == text) return button;
        return static_cast<QPushButton *>(nullptr);
    };
    QPushButton *runStage = nullptr;
    if (!require(waitFor([&] { runStage = findRunButton(stageDialog, QStringLiteral("Run Stage")); return runStage && runStage->isEnabled(); }),
                 "batch stage preview detects unstaged changes")) return 1;
    runStage->click();
    if (!require(waitFor([&] {
            for (QLabel *label : stageDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished:"))) return true;
            return false;
        }), "batch staging completes without blocking the UI")) return 1;
    const QString stagedFiles = QString::fromLocal8Bit([&] {
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        process.setArguments({QStringLiteral("-C"), batchPath, QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--name-only")});
        process.start();
        process.waitForFinished(5000);
        return process.readAllStandardOutput();
    }()).trimmed();
    if (!require(stagedFiles == QStringLiteral("change.txt"), "batch staging affects the selected repository")) return 1;
    stageDialog.accept();

    BatchOperationsDialog commitDialog({batchRepository});
    commitDialog.show();
    auto *operationCombo = commitDialog.findChild<QComboBox *>();
    if (!require(operationCombo != nullptr, "batch dialog exposes an operation selector")) return 1;
    operationCombo->setCurrentIndex(1);
    for (QPlainTextEdit *edit : commitDialog.findChildren<QPlainTextEdit *>())
        if (edit->placeholderText().contains(QStringLiteral("Shared commit message"))) edit->setPlainText(QStringLiteral("Batch commit from test"));
    QPushButton *runCommit = nullptr;
    if (!require(waitFor([&] { runCommit = findRunButton(commitDialog, QStringLiteral("Run Commit")); return runCommit && runCommit->isEnabled(); }),
                 "batch commit preview detects staged changes and accepts a shared message")) return 1;
    runCommit->click();
    if (!require(waitFor([&] {
            for (QLabel *label : commitDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished:"))) return true;
            return false;
        }), "batch commit completes and reports its result")) return 1;
    const QString commitSubject = [&] {
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        process.setArguments({QStringLiteral("-C"), batchPath, QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")});
        process.start();
        process.waitForFinished(5000);
        return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    }();
    if (!require(commitSubject == QStringLiteral("Batch commit from test"), "batch commit creates an independent repository commit")) return 1;
    commitDialog.accept();

    const QString remotePath = temporary.filePath(QStringLiteral("workspace-remote.git"));
    const QString seedPath = temporary.filePath(QStringLiteral("workspace-seed"));
    if (!require(QDir().mkpath(remotePath) && runGit(remotePath, {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-q")}), "local bare remote initializes")) return 1;
    if (!require(makeRepository(seedPath), "remote seed repository initializes")) return 1;
    runGit(seedPath, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")});
    runGit(seedPath, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")});
    QFile seedFile(QDir(seedPath).filePath(QStringLiteral("remote.txt")));
    if (!require(seedFile.open(QIODevice::WriteOnly | QIODevice::Text), "remote seed file opens")) return 1;
    seedFile.write("initial remote state\n");
    seedFile.close();
    if (!require(runGit(seedPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(seedPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Initial remote commit")}), "remote seed commit succeeds")) return 1;
    if (!require(runGit(seedPath, {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), remotePath})
        && runGit(seedPath, {QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("HEAD")}), "seed publishes its branch")) return 1;
    const QString worktreePath = temporary.filePath(QStringLiteral("workspace-seed-worktree"));
    if (!require(runGit(seedPath, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), QStringLiteral("workspace-worktree"), worktreePath}),
                 "a second working tree can be created")) return 1;
    RepositoryWorkspace seedWorkspace = RepositoryWorkspaceStore::workspaceForRepository(seedPath, true);
    if (!require(RepositoryWorkspaceStore::addRepository(seedWorkspace, worktreePath, &error), "a distinct worktree can be added to the workspace")) return 1;
    if (!require(seedWorkspace.repositories.size() == 2
        && seedWorkspace.repositories.first().commonGitDirectory == seedWorkspace.repositories.last().commonGitDirectory
        && seedWorkspace.repositories.first().path != seedWorkspace.repositories.last().path,
        "distinct worktrees sharing a Git common directory stay distinct workspace entries")) return 1;
    const QString syncOnePath = temporary.filePath(QStringLiteral("workspace-sync-one"));
    const QString syncTwoPath = temporary.filePath(QStringLiteral("workspace-sync-two"));
    if (!require(runGit(seedPath, {QStringLiteral("clone"), remotePath, syncOnePath})
        && runGit(seedPath, {QStringLiteral("clone"), remotePath, syncTwoPath}), "two local repositories clone from the remote")) return 1;
    QFile updatedSeedFile(QDir(seedPath).filePath(QStringLiteral("remote.txt")));
    if (!require(updatedSeedFile.open(QIODevice::Append | QIODevice::Text), "remote file reopens for update")) return 1;
    updatedSeedFile.write("incoming update\n");
    updatedSeedFile.close();
    if (!require(runGit(seedPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(seedPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Incoming remote update")})
        && runGit(seedPath, {QStringLiteral("push")}), "remote receives incoming update")) return 1;
    ShellScratchpad::Git::WorkspaceRepository syncOne;
    syncOne.id = QStringLiteral("sync-one");
    syncOne.path = syncOnePath;
    ShellScratchpad::Git::WorkspaceRepository syncTwo;
    syncTwo.id = QStringLiteral("sync-two");
    syncTwo.path = syncTwoPath;
    const QString brokenRemotePath = temporary.filePath(QStringLiteral("not-present.git"));
    const QString brokenRepoPath = temporary.filePath(QStringLiteral("workspace-broken-remote"));
    if (!require(makeRepository(brokenRepoPath)
        && runGit(brokenRepoPath, {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), brokenRemotePath}),
        "broken remote fixture initializes")) return 1;
    ShellScratchpad::Git::WorkspaceRepository brokenRepo;
    brokenRepo.id = QStringLiteral("broken-remote");
    brokenRepo.path = brokenRepoPath;
    BatchOperationsDialog syncDialog({syncOne, syncTwo, brokenRepo});
    syncDialog.show();
    QComboBox *syncCombo = syncDialog.findChild<QComboBox *>();
    if (!require(syncCombo != nullptr, "batch sync exposes an operation selector")) return 1;
    syncCombo->setCurrentIndex(2);
    QPushButton *runSync = nullptr;
    if (!require(waitFor([&] { runSync = findRunButton(syncDialog, QStringLiteral("Run Sync")); return runSync && runSync->isEnabled(); }),
                 "sync preview recognizes selected repositories with remotes")) return 1;
    runSync->click();
    if (!require(waitFor([&] {
            for (QLabel *label : syncDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished: 2 synced, 1 failed"))) return true;
            return false;
        }), "batch sync reports independent successes and failures")) return 1;
    const QString syncedSubjectOne = [&] {
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        process.setArguments({QStringLiteral("-C"), syncOnePath, QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")});
        process.start(); process.waitForFinished(5000);
        return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    }();
    const QString syncedSubjectTwo = [&] {
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        process.setArguments({QStringLiteral("-C"), syncTwoPath, QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")});
        process.start(); process.waitForFinished(5000);
        return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    }();
    if (!require(syncedSubjectOne == QStringLiteral("Incoming remote update") && syncedSubjectTwo == syncedSubjectOne,
                 "batch sync fetches and rebases each repository independently")) return 1;
    syncDialog.accept();

    for (const QString &clonePath : {syncOnePath, syncTwoPath}) {
        runGit(clonePath, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")});
        runGit(clonePath, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")});
    }
    QFile localAheadFile(QDir(syncOnePath).filePath(QStringLiteral("local-only.txt")));
    if (!require(localAheadFile.open(QIODevice::WriteOnly | QIODevice::Text), "local ahead file opens")) return 1;
    localAheadFile.write("local ahead\n");
    localAheadFile.close();
    if (!require(runGit(syncOnePath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(syncOnePath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Local ahead commit")}), "local ahead commit succeeds")) return 1;
    QFile remoteAheadFile(QDir(seedPath).filePath(QStringLiteral("remote-only.txt")));
    if (!require(remoteAheadFile.open(QIODevice::WriteOnly | QIODevice::Text), "remote ahead file opens")) return 1;
    remoteAheadFile.write("remote ahead\n");
    remoteAheadFile.close();
    if (!require(runGit(seedPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(seedPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Remote ahead commit")})
        && runGit(seedPath, {QStringLiteral("push")}), "remote advances independently")) return 1;

    BatchOperationsDialog divergenceDialog({syncOne});
    divergenceDialog.show();
    QComboBox *divergenceCombo = divergenceDialog.findChild<QComboBox *>();
    divergenceCombo->setCurrentIndex(2);
    QPushButton *runDivergenceSync = nullptr;
    if (!require(waitFor([&] { runDivergenceSync = findRunButton(divergenceDialog, QStringLiteral("Run Sync")); return runDivergenceSync && runDivergenceSync->isEnabled(); }),
                 "diverged repository is eligible for synchronized push")) return 1;
    runDivergenceSync->click();
    bool choseSyncThenPush = false;
    if (!require(waitFor([&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *prompt = qobject_cast<QMessageBox *>(widget);
                if (!prompt) continue;
                for (QPushButton *button : prompt->findChildren<QPushButton *>()) {
                    if (button->text() == QStringLiteral("Sync Then Push")) {
                        choseSyncThenPush = true;
                        button->click();
                        return true;
                    }
                }
            }
            return false;
        }), "batch sync preserves the pull-before-push confirmation")) return 1;
    if (!require(choseSyncThenPush && waitFor([&] {
            for (QLabel *label : divergenceDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished:"))) return true;
            return false;
        }), "diverged repository completes its selected sync workflow")) return 1;
    runGit(syncTwoPath, {QStringLiteral("fetch"), QStringLiteral("origin")});
    const auto gitText = [](const QString &path, const QStringList &arguments) {
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        QStringList args{QStringLiteral("-C"), path};
        args.append(arguments);
        process.setArguments(args);
        process.start(); process.waitForFinished(5000);
        return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    };
    if (!require(gitText(syncOnePath, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")})
        == gitText(syncTwoPath, {QStringLiteral("rev-parse"), QStringLiteral("@{upstream}")}),
                 "batch sync pulls incoming commits before pushing local commits")) return 1;
    divergenceDialog.accept();

    const QString secondRemotePath = temporary.filePath(QStringLiteral("workspace-remote-two.git"));
    if (!require(runGit(syncTwoPath, {QStringLiteral("clone"), QStringLiteral("--bare"), syncTwoPath, secondRemotePath}),
                 "second independent remote initializes")) return 1;
    if (!require(runGit(syncTwoPath, {QStringLiteral("remote"), QStringLiteral("set-url"), QStringLiteral("origin"), secondRemotePath}),
                 "second sync repository uses an independent remote")) return 1;
    for (const auto &entry : {qMakePair(syncOnePath, QStringLiteral("parallel-one.txt")), qMakePair(syncTwoPath, QStringLiteral("parallel-two.txt"))}) {
        QFile file(QDir(entry.first).filePath(entry.second));
        if (!require(file.open(QIODevice::WriteOnly | QIODevice::Text), "parallel push fixture file opens")) return 1;
        file.write("parallel push\n");
        file.close();
        if (!require(runGit(entry.first, {QStringLiteral("add"), QStringLiteral("-A")})
            && runGit(entry.first, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Parallel push")}), "parallel push fixture commit succeeds")) return 1;
    }
    const QString firstStarted = temporary.filePath(QStringLiteral("first-push-started"));
    const QString secondStarted = temporary.filePath(QStringLiteral("second-push-started"));
    const auto installBarrierHook = [](const QString &repositoryPath, const QString &ownMarker, const QString &otherMarker) {
        const QString hookPath = QDir(repositoryPath).filePath(QStringLiteral(".git/hooks/pre-push"));
        QFile hook(hookPath);
        if (!require(hook.open(QIODevice::WriteOnly | QIODevice::Text), "parallel push barrier hook opens")) return false;
        hook.write(QStringLiteral("#!/bin/sh\ntouch '%1'\ni=0\nwhile [ ! -f '%2' ]; do\n  sleep 0.05\n  i=$((i + 1))\n  [ \"$i\" -lt 100 ] || exit 9\ndone\nexit 0\n").arg(ownMarker, otherMarker).toUtf8());
        hook.close();
        return hook.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    };
    if (!require(installBarrierHook(syncOnePath, firstStarted, secondStarted)
        && installBarrierHook(syncTwoPath, secondStarted, firstStarted), "parallel push barrier hooks install")) return 1;
    BatchOperationsDialog parallelSyncDialog({syncOne, syncTwo});
    parallelSyncDialog.show();
    QComboBox *parallelSyncCombo = parallelSyncDialog.findChild<QComboBox *>();
    parallelSyncCombo->setCurrentIndex(2);
    QPushButton *runParallelSync = nullptr;
    if (!require(waitFor([&] { runParallelSync = findRunButton(parallelSyncDialog, QStringLiteral("Run Sync")); return runParallelSync && runParallelSync->isEnabled(); }),
                 "parallel push repositories are eligible for sync")) return 1;
    runParallelSync->click();
    if (!require(waitFor([&] {
            for (QLabel *label : parallelSyncDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished: 2 synced"))) return true;
            return false;
        }), "independent pushes run concurrently within the batch scheduler limit")) {
        for (QTreeWidget *tree : parallelSyncDialog.findChildren<QTreeWidget *>())
            for (int index = 0; index < tree->topLevelItemCount(); ++index)
                qWarning().noquote() << tree->topLevelItem(index)->text(0) << tree->topLevelItem(index)->text(4);
        qWarning().noquote() << parallelSyncDialog.findChild<QPlainTextEdit *>()->toPlainText();
        return 1;
    }
    if (!require(QFileInfo::exists(firstStarted) && QFileInfo::exists(secondStarted),
                 "both independent push processes were active at the same time")) return 1;
    parallelSyncDialog.accept();

    const QString cancelOnePath = temporary.filePath(QStringLiteral("workspace-cancel-one"));
    const QString cancelTwoPath = temporary.filePath(QStringLiteral("workspace-cancel-two"));
    if (!require(makeRepository(cancelOnePath) && makeRepository(cancelTwoPath), "cancellation repositories initialize")) return 1;
    QVector<ShellScratchpad::Git::WorkspaceRepository> cancelRepositories;
    for (const auto &entry : {qMakePair(cancelOnePath, QStringLiteral("cancel-one")), qMakePair(cancelTwoPath, QStringLiteral("cancel-two"))}) {
        runGit(entry.first, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")});
        runGit(entry.first, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")});
        QFile file(QDir(entry.first).filePath(QStringLiteral("change.txt")));
        if (!require(file.open(QIODevice::WriteOnly | QIODevice::Text), "cancellation fixture file opens")) return 1;
        file.write("cancel test\n");
        file.close();
        if (!require(runGit(entry.first, {QStringLiteral("add"), QStringLiteral("-A")}), "cancellation fixture stages a change")) return 1;
        ShellScratchpad::Git::WorkspaceRepository repository;
        repository.path = entry.first;
        repository.id = entry.second;
        cancelRepositories.append(repository);
    }
    QFile commitHook(QDir(cancelOnePath).filePath(QStringLiteral(".git/hooks/pre-commit")));
    if (!require(commitHook.open(QIODevice::WriteOnly | QIODevice::Text), "slow commit hook opens")) return 1;
    commitHook.write("#!/bin/sh\nsleep 1\nexit 0\n");
    commitHook.close();
    if (!require(commitHook.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "slow commit hook becomes executable")) return 1;
    BatchOperationsDialog cancellationDialog(cancelRepositories);
    cancellationDialog.show();
    QComboBox *cancellationCombo = cancellationDialog.findChild<QComboBox *>();
    cancellationCombo->setCurrentIndex(1);
    for (QPlainTextEdit *edit : cancellationDialog.findChildren<QPlainTextEdit *>())
        if (edit->placeholderText().contains(QStringLiteral("Shared commit message"))) edit->setPlainText(QStringLiteral("Cancellation fixture"));
    QPushButton *runCancelableCommit = nullptr;
    if (!require(waitFor([&] { runCancelableCommit = findRunButton(cancellationDialog, QStringLiteral("Run Commit")); return runCancelableCommit && runCancelableCommit->isEnabled(); }),
                 "cancellation preview identifies staged commits")) return 1;
    runCancelableCommit->click();
    QPushButton *stopAfterCurrent = nullptr;
    if (!require(waitFor([&] {
            for (QPushButton *button : cancellationDialog.findChildren<QPushButton *>())
                if (button->text() == QStringLiteral("Stop After Current")) stopAfterCurrent = button;
            for (QTreeWidget *tree : cancellationDialog.findChildren<QTreeWidget *>())
                if (tree->topLevelItemCount() && tree->topLevelItem(0)->text(4) == QStringLiteral("Committing…")) return stopAfterCurrent != nullptr;
            return false;
        }), "first commit starts before cancellation")) return 1;
    stopAfterCurrent->click();
    if (!require(waitFor([&] {
            for (QLabel *label : cancellationDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished:"))) return true;
            return false;
        }), "batch cancellation waits for the active commit to finish")) return 1;
    bool secondCancelled = false;
    for (QTreeWidget *tree : cancellationDialog.findChildren<QTreeWidget *>())
        if (tree->topLevelItemCount() > 1)
            secondCancelled = tree->topLevelItem(1)->text(4) == QStringLiteral("Cancelled: not started");
    if (!require(secondCancelled, "cancellation prevents starting later repository commits")) return 1;
    cancellationDialog.accept();

    QVector<RepositoryWorkspace> configuredWorkspaces = RepositoryWorkspaceStore::load();
    RepositoryWorkspace duplicateWorkspace = configuredWorkspaces.first();
    duplicateWorkspace.id = QStringLiteral("duplicate-workspace-fixture");
    configuredWorkspaces.append(duplicateWorkspace);
    RepositoryWorkspaceStore::save(configuredWorkspaces);
    if (!require(RepositoryWorkspaceStore::workspacesForRepository(firstPath).size() == 2
        && RepositoryWorkspaceStore::workspaceForRepository(firstPath, false).id.isEmpty(),
        "ambiguous workspace membership is reported instead of silently choosing one")) return 1;

    QVariantMap malformedRepository;
    malformedRepository.insert(QStringLiteral("path"), QStringLiteral("missing-id-is-invalid"));
    QVariantMap retainedRepository;
    retainedRepository.insert(QStringLiteral("id"), QStringLiteral("retained-repository"));
    retainedRepository.insert(QStringLiteral("path"), firstPath);
    QVariantMap partiallyValidWorkspace;
    partiallyValidWorkspace.insert(QStringLiteral("id"), QStringLiteral("partially-valid-workspace"));
    partiallyValidWorkspace.insert(QStringLiteral("primaryRepositoryId"), QStringLiteral("retained-repository"));
    partiallyValidWorkspace.insert(QStringLiteral("repositories"), QVariantList{malformedRepository, retainedRepository});
    QVariantMap invalidWorkspace;
    invalidWorkspace.insert(QStringLiteral("repositories"), QVariantList{retainedRepository});
    QSettings settings;
    settings.setValue(QStringLiteral("gitWorkspace/managedWorkspaces"), QVariantList{partiallyValidWorkspace, invalidWorkspace});
    const QVector<RepositoryWorkspace> recoveredConfiguration = RepositoryWorkspaceStore::load();
    if (!require(recoveredConfiguration.size() == 1 && recoveredConfiguration.first().repositories.size() == 1
        && recoveredConfiguration.first().repositories.first().id == QStringLiteral("retained-repository"),
        "malformed configuration entries do not discard valid workspace members")) return 1;

    const QString actionSeedPath = temporary.filePath(QStringLiteral("action-mode-seed"));
    const QString actionPushRemotePath = temporary.filePath(QStringLiteral("action-mode-push.git"));
    const QString actionPullRemotePath = temporary.filePath(QStringLiteral("action-mode-pull.git"));
    if (!require(makeRepository(actionSeedPath)
        && runGit(actionSeedPath, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")})
        && runGit(actionSeedPath, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")}),
        "targeted pull/push fixture initializes")) return 1;
    for (const QString &bareRemote : {actionPushRemotePath, actionPullRemotePath})
        if (!require(QDir().mkpath(bareRemote) && runGit(bareRemote, {QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-q")}),
                     "targeted action bare remote initializes")) return 1;
    QFile actionSeedFile(QDir(actionSeedPath).filePath(QStringLiteral("base.txt")));
    if (!require(actionSeedFile.open(QIODevice::WriteOnly | QIODevice::Text), "targeted action seed file opens")) return 1;
    actionSeedFile.write("base\n");
    actionSeedFile.close();
    if (!require(runGit(actionSeedPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(actionSeedPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Action mode base")}),
        "targeted action base commit succeeds")) return 1;
    const QString actionBranch = gitText(actionSeedPath, {QStringLiteral("branch"), QStringLiteral("--show-current")});
    for (const QString &bareRemote : {actionPushRemotePath, actionPullRemotePath})
        if (!require(runGit(actionSeedPath, {QStringLiteral("push"), bareRemote, QStringLiteral("HEAD:refs/heads/") + actionBranch}),
                     "targeted action baseline publishes")) return 1;
    const QString actionPushPath = temporary.filePath(QStringLiteral("action-push-repo"));
    const QString actionPullPath = temporary.filePath(QStringLiteral("action-pull-repo"));
    if (!require(runGit(actionSeedPath, {QStringLiteral("clone"), actionPushRemotePath, actionPushPath})
        && runGit(actionSeedPath, {QStringLiteral("clone"), actionPullRemotePath, actionPullPath}),
        "targeted push and pull repositories clone")) return 1;
    for (const QString &repo : {actionPushPath, actionPullPath}) {
        if (!require(runGit(repo, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Workspace Test")})
            && runGit(repo, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("workspace@example.invalid")}),
            "targeted action clone gets a local author")) return 1;
    }
    QFile actionPushFile(QDir(actionPushPath).filePath(QStringLiteral("outgoing.txt")));
    if (!require(actionPushFile.open(QIODevice::WriteOnly | QIODevice::Text), "targeted push file opens")) return 1;
    actionPushFile.write("outgoing\n");
    actionPushFile.close();
    if (!require(runGit(actionPushPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(actionPushPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Targeted push")}),
        "targeted push commit is created")) return 1;
    ShellScratchpad::Git::WorkspaceRepository actionPushRepository;
    actionPushRepository.id = QStringLiteral("targeted-push");
    actionPushRepository.path = actionPushPath;
    BatchOperationsDialog targetedPushDialog({actionPushRepository}, nullptr, QStringLiteral("push"));
    targetedPushDialog.show();
    QPushButton *runTargetedPush = nullptr;
    if (!require(waitFor([&] { runTargetedPush = findRunButton(targetedPushDialog, QStringLiteral("Run Push")); return runTargetedPush && runTargetedPush->isEnabled(); }),
                 "targeted push mode recognizes outgoing commits")) return 1;
    runTargetedPush->click();
    if (!require(waitFor([&] {
            for (QLabel *label : targetedPushDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished: 1 pushed"))) return true;
            return false;
        }), "selected repository completes a push-only batch action")) return 1;
    if (!require(gitText(actionPushRemotePath, {QStringLiteral("rev-parse"), QStringLiteral("refs/heads/") + actionBranch})
        == gitText(actionPushPath, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}),
        "targeted push publishes the selected repository's commit")) return 1;
    targetedPushDialog.accept();

    QFile incomingActionFile(QDir(actionSeedPath).filePath(QStringLiteral("incoming.txt")));
    if (!require(incomingActionFile.open(QIODevice::WriteOnly | QIODevice::Text), "targeted pull file opens")) return 1;
    incomingActionFile.write("incoming\n");
    incomingActionFile.close();
    if (!require(runGit(actionSeedPath, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(actionSeedPath, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("Targeted pull")})
        && runGit(actionSeedPath, {QStringLiteral("push"), actionPullRemotePath, QStringLiteral("HEAD:refs/heads/") + actionBranch}),
        "targeted pull remote advances")) return 1;
    ShellScratchpad::Git::WorkspaceRepository actionPullRepository;
    actionPullRepository.id = QStringLiteral("targeted-pull");
    actionPullRepository.path = actionPullPath;
    BatchOperationsDialog targetedPullDialog({actionPullRepository}, nullptr, QStringLiteral("pull"));
    targetedPullDialog.show();
    QPushButton *runTargetedPull = nullptr;
    if (!require(waitFor([&] { runTargetedPull = findRunButton(targetedPullDialog, QStringLiteral("Run Pull")); return runTargetedPull && runTargetedPull->isEnabled(); }),
                 "targeted pull mode recognizes the configured upstream")) return 1;
    runTargetedPull->click();
    if (!require(waitFor([&] {
            for (QLabel *label : targetedPullDialog.findChildren<QLabel *>())
                if (label->text().startsWith(QStringLiteral("Finished: 1 pulled"))) return true;
            return false;
        }), "selected repository completes a pull-only batch action")) return 1;
    if (!require(gitText(actionPullPath, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")})
        == gitText(actionPullPath, {QStringLiteral("rev-parse"), QStringLiteral("@{upstream}")}),
        "targeted pull updates from the selected repository's upstream")) return 1;
    targetedPullDialog.accept();

    return 0;
}
