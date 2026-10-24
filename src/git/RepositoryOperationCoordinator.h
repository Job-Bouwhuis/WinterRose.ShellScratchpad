#pragma once

#include <QString>

namespace ShellScratchpad::Git
{
class RepositoryOperationCoordinator final
{
public:
    static bool tryAcquire(const QString &repositoryPath);
    static void release(const QString &repositoryPath);
    static bool isBusy(const QString &repositoryPath);
    static QString keyForPath(const QString &repositoryPath);
};
}
