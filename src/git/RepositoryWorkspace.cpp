#include "RepositoryWorkspace.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QUuid>
#include <algorithm>

namespace ShellScratchpad::Git
{
namespace
{
QString canonicalOrCleanPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString runGit(const QString &root, const QStringList &arguments)
{
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    QStringList args{QStringLiteral("-C"), root};
    args.append(arguments);
    process.setArguments(args);
    process.start();
    if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    if (process.exitCode() != 0) return {};
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVariantMap repositoryToVariant(const WorkspaceRepository &repository)
{
    return {{QStringLiteral("id"), repository.id},
            {QStringLiteral("path"), repository.path},
            {QStringLiteral("gitDirectory"), repository.gitDirectory},
            {QStringLiteral("commonGitDirectory"), repository.commonGitDirectory}};
}

QVariantMap workspaceToVariant(const RepositoryWorkspace &workspace)
{
    QVariantList repositories;
    for (const WorkspaceRepository &repository : workspace.repositories)
        repositories.append(repositoryToVariant(repository));
    return {{QStringLiteral("id"), workspace.id},
            {QStringLiteral("primaryRepositoryId"), workspace.primaryRepositoryId},
            {QStringLiteral("actionRepositoryIds"), workspace.actionRepositoryIds},
            {QStringLiteral("repositories"), repositories}};
}
}

QString RepositoryWorkspaceStore::repositoryRoot(const QString &path)
{
    const QString absolute = canonicalOrCleanPath(path);
    const QString root = runGit(absolute, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    return root.isEmpty() ? QString() : canonicalOrCleanPath(root);
}

QString RepositoryWorkspaceStore::gitDirectory(const QString &repositoryRoot, bool common)
{
    const QString flag = common ? QStringLiteral("--git-common-dir") : QStringLiteral("--absolute-git-dir");
    const QString value = runGit(repositoryRoot, {QStringLiteral("rev-parse"), flag});
    if (value.isEmpty()) return {};
    return canonicalOrCleanPath(QDir::isRelativePath(value) ? QDir(repositoryRoot).filePath(value) : value);
}

QVector<RepositoryWorkspace> RepositoryWorkspaceStore::load()
{
    QVector<RepositoryWorkspace> workspaces;
    const QVariantList raw = QSettings().value(QStringLiteral("gitWorkspace/managedWorkspaces")).toList();
    for (const QVariant &workspaceValue : raw) {
        const QVariantMap map = workspaceValue.toMap();
        RepositoryWorkspace workspace;
        workspace.id = map.value(QStringLiteral("id")).toString();
        workspace.primaryRepositoryId = map.value(QStringLiteral("primaryRepositoryId")).toString();
        workspace.actionRepositoryIds = map.value(QStringLiteral("actionRepositoryIds")).toStringList();
        if (workspace.id.isEmpty()) continue;
        const QVariantList repositories = map.value(QStringLiteral("repositories")).toList();
        for (const QVariant &repositoryValue : repositories) {
            const QVariantMap repositoryMap = repositoryValue.toMap();
            WorkspaceRepository repository;
            repository.id = repositoryMap.value(QStringLiteral("id")).toString();
            repository.path = repositoryMap.value(QStringLiteral("path")).toString();
            repository.gitDirectory = repositoryMap.value(QStringLiteral("gitDirectory")).toString();
            repository.commonGitDirectory = repositoryMap.value(QStringLiteral("commonGitDirectory")).toString();
            if (repository.id.isEmpty() || repository.path.isEmpty()) continue;
            workspace.repositories.append(repository);
        }
        if (!workspace.repositories.isEmpty()) workspaces.append(workspace);
    }
    return workspaces;
}

void RepositoryWorkspaceStore::save(const QVector<RepositoryWorkspace> &workspaces)
{
    QVariantList values;
    for (const RepositoryWorkspace &workspace : workspaces)
        values.append(workspaceToVariant(workspace));
    QSettings().setValue(QStringLiteral("gitWorkspace/managedWorkspaces"), values);
}

RepositoryWorkspace RepositoryWorkspaceStore::workspaceForRepository(const QString &path, bool createIfMissing)
{
    const QString root = repositoryRoot(path);
    if (root.isEmpty()) return {};
    const QVector<RepositoryWorkspace> matches = workspacesForRepository(root);
    if (matches.size() == 1) return matches.first();
    if (matches.size() > 1 || !createIfMissing) return {};
    RepositoryWorkspace workspace;
    workspace.id = uuid();
    WorkspaceRepository repository;
    repository.id = uuid();
    repository.path = root;
    repository.gitDirectory = gitDirectory(root);
    repository.commonGitDirectory = gitDirectory(root, true);
    workspace.primaryRepositoryId = repository.id;
    workspace.repositories.append(repository);
    QVector<RepositoryWorkspace> workspaces = load();
    workspaces.append(workspace);
    save(workspaces);
    return workspace;
}

QVector<RepositoryWorkspace> RepositoryWorkspaceStore::workspacesForRepository(const QString &path)
{
    QVector<RepositoryWorkspace> matches;
    const QString root = repositoryRoot(path);
    if (root.isEmpty()) return matches;
    for (const RepositoryWorkspace &workspace : load()) {
        const bool containsRepository = std::any_of(workspace.repositories.cbegin(), workspace.repositories.cend(), [&root](const WorkspaceRepository &repository) {
            return canonicalOrCleanPath(repository.path) == root;
        });
        if (containsRepository) matches.append(workspace);
    }
    return matches;
}

bool RepositoryWorkspaceStore::addRepository(RepositoryWorkspace &workspace, const QString &path, QString *error)
{
    const QString root = repositoryRoot(path);
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("The selected folder is not inside a Git working tree.");
        return false;
    }
    for (const WorkspaceRepository &repository : workspace.repositories) {
        if (canonicalOrCleanPath(repository.path) == root) {
            if (error) *error = QStringLiteral("This repository is already in the workspace.");
            return false;
        }
    }
    const QVector<RepositoryWorkspace> storedWorkspaces = load();
    for (const RepositoryWorkspace &storedWorkspace : storedWorkspaces) {
        for (const WorkspaceRepository &storedRepository : storedWorkspace.repositories) {
            if (storedWorkspace.id != workspace.id && canonicalOrCleanPath(storedRepository.path) == root) {
                if (error) *error = QStringLiteral("This repository already belongs to another configured workspace.");
                return false;
            }
        }
    }
    WorkspaceRepository repository;
    repository.id = uuid();
    repository.path = root;
    repository.gitDirectory = gitDirectory(root);
    repository.commonGitDirectory = gitDirectory(root, true);
    workspace.repositories.append(repository);
    QVector<RepositoryWorkspace> workspaces = load();
    bool replaced = false;
    for (RepositoryWorkspace &entry : workspaces) {
        if (entry.id == workspace.id) { entry = workspace; replaced = true; break; }
    }
    if (!replaced) workspaces.append(workspace);
    save(workspaces);
    return true;
}

bool RepositoryWorkspaceStore::removeRepository(RepositoryWorkspace &workspace, const QString &repositoryId)
{
    if (workspace.repositories.size() <= 1) return false;
    for (qsizetype index = 0; index < workspace.repositories.size(); ++index) {
        if (workspace.repositories.at(index).id != repositoryId) continue;
        workspace.repositories.removeAt(index);
        workspace.actionRepositoryIds.removeAll(repositoryId);
        if (workspace.primaryRepositoryId == repositoryId)
            workspace.primaryRepositoryId = workspace.repositories.first().id;
        QVector<RepositoryWorkspace> workspaces = load();
        for (RepositoryWorkspace &entry : workspaces) {
            if (entry.id == workspace.id) { entry = workspace; break; }
        }
        save(workspaces);
        return true;
    }
    return false;
}

void RepositoryWorkspaceStore::saveWorkspace(const RepositoryWorkspace &workspace)
{
    QVector<RepositoryWorkspace> workspaces = load();
    bool replaced = false;
    for (RepositoryWorkspace &entry : workspaces) {
        if (entry.id == workspace.id) { entry = workspace; replaced = true; break; }
    }
    if (!replaced) workspaces.append(workspace);
    save(workspaces);
}

bool RepositoryWorkspaceStore::updateRepositoryPath(RepositoryWorkspace &workspace, const QString &repositoryId, const QString &path, QString *error)
{
    const QString root = repositoryRoot(path);
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("The selected folder is not inside a Git working tree.");
        return false;
    }
    for (qsizetype index = 0; index < workspace.repositories.size(); ++index) {
        WorkspaceRepository &repository = workspace.repositories[index];
        if (repository.id == repositoryId) {
            repository.path = root;
            repository.gitDirectory = gitDirectory(root);
            repository.commonGitDirectory = gitDirectory(root, true);
            saveWorkspace(workspace);
            return true;
        }
        if (canonicalOrCleanPath(workspace.repositories.at(index).path) == root) {
            if (error) *error = QStringLiteral("That repository is already in the workspace.");
            return false;
        }
    }
    return false;
}
}
