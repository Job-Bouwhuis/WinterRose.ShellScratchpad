#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ShellScratchpad::Git
{
struct WorkspaceRepository
{
    QString id;
    QString path;
    QString gitDirectory;
    QString commonGitDirectory;
};

struct RepositoryWorkspace
{
    QString id;
    QString primaryRepositoryId;
    QStringList actionRepositoryIds;
    QVector<WorkspaceRepository> repositories;
};

class RepositoryWorkspaceStore final
{
public:
    static QString repositoryRoot(const QString &path);
    static QString gitDirectory(const QString &repositoryRoot, bool common = false);
    static QVector<RepositoryWorkspace> load();
    static QVector<RepositoryWorkspace> workspacesForRepository(const QString &path);
    static void save(const QVector<RepositoryWorkspace> &workspaces);
    static RepositoryWorkspace workspaceForRepository(const QString &path, bool createIfMissing);
    static bool addRepository(RepositoryWorkspace &workspace, const QString &path, QString *error = nullptr);
    static bool removeRepository(RepositoryWorkspace &workspace, const QString &repositoryId);
    static bool updateRepositoryPath(RepositoryWorkspace &workspace, const QString &repositoryId, const QString &path, QString *error = nullptr);
    static void saveWorkspace(const RepositoryWorkspace &workspace);
};
}
