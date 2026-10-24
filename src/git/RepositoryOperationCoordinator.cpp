#include "RepositoryOperationCoordinator.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

namespace ShellScratchpad::Git
{
namespace
{
QMutex coordinatorMutex;
QSet<QString> busyRepositories;
}

QString RepositoryOperationCoordinator::keyForPath(const QString &repositoryPath)
{
    const QFileInfo info(repositoryPath);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool RepositoryOperationCoordinator::tryAcquire(const QString &repositoryPath)
{
    const QString key = keyForPath(repositoryPath);
    QMutexLocker locker(&coordinatorMutex);
    if (busyRepositories.contains(key)) return false;
    busyRepositories.insert(key);
    return true;
}

void RepositoryOperationCoordinator::release(const QString &repositoryPath)
{
    QMutexLocker locker(&coordinatorMutex);
    busyRepositories.remove(keyForPath(repositoryPath));
}

bool RepositoryOperationCoordinator::isBusy(const QString &repositoryPath)
{
    QMutexLocker locker(&coordinatorMutex);
    return busyRepositories.contains(keyForPath(repositoryPath));
}
}
