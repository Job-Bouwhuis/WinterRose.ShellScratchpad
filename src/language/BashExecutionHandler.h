#pragma once

#include <QByteArray>
#include <QProcess>
#include <QSet>

namespace ShellScratchpad::Language
{
struct BashExecutionRequest
{
    QByteArray source;
    bool guiSudoPrompts = false;
    bool reuseSudoPassword = false;
    bool captureHistory = false;
    bool debuggingEnabled = false;
    QSet<int> breakpoints;
    int sourceLineOffset = 0;
    bool startPaused = false;
};

// Bash-specific preparation for launching a script and its runtime instrumentation.
// The window owns the process lifecycle and consumes the resulting output/debug events.
class BashExecutionHandler final
{
public:
    static QByteArray buildRuntimeScript(const BashExecutionRequest &request,
                                         int *userScriptStartLine = nullptr);
    static void configureProcess(QProcess &process, const QString &runtimeScriptPath,
                                 int scriptDescriptor,
                                 int historyReadDescriptor, int historyWriteDescriptor,
                                 int debuggerChildSocketDescriptor, int debuggerSocketDescriptor);
};
}
