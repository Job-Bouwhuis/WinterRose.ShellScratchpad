#include "GitWorkspaceWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Winterrose"));
    QCoreApplication::setApplicationName(QStringLiteral("ShellScratchpad"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Git workspace"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("repository"), QStringLiteral("Repository folder to open"));
    parser.process(application);

    QString repositoryPath = parser.positionalArguments().isEmpty()
        ? QSettings().value(QStringLiteral("gitWorkspace/lastRepository")).toString().trimmed()
        : QFileInfo(parser.positionalArguments().first()).absoluteFilePath();
    if (repositoryPath.isEmpty()) repositoryPath = QDir::homePath();
    if (!ShellScratchpad::Git::GitWorkspaceWindow::isGitRepository(repositoryPath)) {
        if (!parser.positionalArguments().isEmpty()) {
            QMessageBox::critical(nullptr, QStringLiteral("Git repository required"),
                QStringLiteral("The selected folder is not inside a Git working tree."));
            return 2;
        }
        repositoryPath = QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Open Git Repository"),
            QDir(repositoryPath).exists() ? repositoryPath : QDir::homePath());
        if (repositoryPath.isEmpty()) return 0;
        if (!ShellScratchpad::Git::GitWorkspaceWindow::isGitRepository(repositoryPath)) {
            QMessageBox::critical(nullptr, QStringLiteral("Git repository required"),
                QStringLiteral("The selected folder is not inside a Git working tree."));
            return 2;
        }
    }
    auto *window = new ShellScratchpad::Git::GitWorkspaceWindow(repositoryPath);
    window->show();
    return application.exec();
}
