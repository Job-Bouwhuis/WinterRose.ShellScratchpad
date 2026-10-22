#include "CompletionController.h"

#include "editor/CodeEditor.h"
#include "syntax/SyntaxTokenizer.h"
#include "util/FuzzyMatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QMap>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QHash>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolTip>
#include <QTimer>

#include <algorithm>

namespace ShellScratchpad::Completion
{
namespace
{
const QStringList KEYWORDS = {
    QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("elif"),
    QStringLiteral("fi"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("until"),
    QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
    QStringLiteral("function"), QStringLiteral("select"), QStringLiteral("time"), QStringLiteral("coproc")
};

const QStringList BUILTINS = {
    QStringLiteral("alias"), QStringLiteral("bg"), QStringLiteral("bind"), QStringLiteral("break"),
    QStringLiteral("builtin"), QStringLiteral("cd"), QStringLiteral("command"), QStringLiteral("continue"),
    QStringLiteral("declare"), QStringLiteral("echo"), QStringLiteral("eval"), QStringLiteral("exec"),
    QStringLiteral("exit"), QStringLiteral("export"), QStringLiteral("false"), QStringLiteral("fg"),
    QStringLiteral("getopts"), QStringLiteral("hash"), QStringLiteral("help"), QStringLiteral("history"),
    QStringLiteral("jobs"), QStringLiteral("kill"), QStringLiteral("local"), QStringLiteral("printf"),
    QStringLiteral("pwd"), QStringLiteral("read"), QStringLiteral("readonly"), QStringLiteral("return"),
    QStringLiteral("set"), QStringLiteral("shift"), QStringLiteral("source"), QStringLiteral("test"),
    QStringLiteral("times"), QStringLiteral("trap"), QStringLiteral("true"), QStringLiteral("type"),
    QStringLiteral("ulimit"), QStringLiteral("umask"), QStringLiteral("unalias"), QStringLiteral("unset")
};

QHash<QString, QStringList> argumentCatalog()
{
    return {
        {QStringLiteral("ls"), {QStringLiteral("-a"), QStringLiteral("-l"), QStringLiteral("-h"), QStringLiteral("-R"), QStringLiteral("--color=auto")}},
        {QStringLiteral("grep"), {QStringLiteral("-i"), QStringLiteral("-n"), QStringLiteral("-r"), QStringLiteral("-v"), QStringLiteral("-E"), QStringLiteral("-F"), QStringLiteral("--color=auto")}},
        {QStringLiteral("find"), {QStringLiteral("-name"), QStringLiteral("-iname"), QStringLiteral("-type"), QStringLiteral("-mtime"), QStringLiteral("-maxdepth"), QStringLiteral("-exec")}},
        {QStringLiteral("git"), {QStringLiteral("add"), QStringLiteral("am"), QStringLiteral("archive"),
            QStringLiteral("bisect"), QStringLiteral("blame"), QStringLiteral("branch"), QStringLiteral("bundle"),
            QStringLiteral("checkout"), QStringLiteral("cherry-pick"), QStringLiteral("clean"),
            QStringLiteral("clone"), QStringLiteral("commit"), QStringLiteral("config"), QStringLiteral("diff"),
            QStringLiteral("fetch"), QStringLiteral("format-patch"), QStringLiteral("grep"),
            QStringLiteral("init"), QStringLiteral("log"), QStringLiteral("merge"), QStringLiteral("mv"),
            QStringLiteral("pull"), QStringLiteral("push"), QStringLiteral("rebase"), QStringLiteral("reflog"),
            QStringLiteral("remote"), QStringLiteral("reset"), QStringLiteral("restore"),
            QStringLiteral("revert"), QStringLiteral("rm"), QStringLiteral("show"), QStringLiteral("sparse-checkout"),
            QStringLiteral("stash"), QStringLiteral("status"), QStringLiteral("submodule"),
            QStringLiteral("switch"), QStringLiteral("tag"), QStringLiteral("worktree"),
            QStringLiteral("-C"), QStringLiteral("--help"), QStringLiteral("--version")} },
        {QStringLiteral("cmake"), {QStringLiteral("-S"), QStringLiteral("-B"), QStringLiteral("--build"), QStringLiteral("--install"), QStringLiteral("-D")}},
        {QStringLiteral("systemctl"), {QStringLiteral("status"), QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("restart"), QStringLiteral("enable"), QStringLiteral("disable"), QStringLiteral("--user")}},
        {QStringLiteral("journalctl"), {QStringLiteral("-b"), QStringLiteral("-k"), QStringLiteral("-u"), QStringLiteral("-n"), QStringLiteral("--since"), QStringLiteral("--no-pager")}},
        {QStringLiteral("apt"), {QStringLiteral("update"), QStringLiteral("upgrade"), QStringLiteral("install"), QStringLiteral("remove"), QStringLiteral("purge"), QStringLiteral("--yes")}},
        {QStringLiteral("dnf"), {QStringLiteral("check-update"), QStringLiteral("upgrade"), QStringLiteral("install"), QStringLiteral("remove"), QStringLiteral("search"), QStringLiteral("--refresh")}},
        {QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("-E"), QStringLiteral("-u"), QStringLiteral("--preserve-env")}},
        {QStringLiteral("add"), {QStringLiteral("-A"), QStringLiteral("--all"), QStringLiteral("-p"), QStringLiteral("--patch"), QStringLiteral("-u"), QStringLiteral("--update"), QStringLiteral("-f"), QStringLiteral("--force"), QStringLiteral("-N"), QStringLiteral("--intent-to-add"), QStringLiteral("--renormalize"), QStringLiteral("--ignore-errors")}},
        {QStringLiteral("am"), {QStringLiteral("--continue"), QStringLiteral("--skip"), QStringLiteral("--abort"), QStringLiteral("--quit"), QStringLiteral("--3way"), QStringLiteral("--signoff"), QStringLiteral("--committer-date-is-author-date")}},
        {QStringLiteral("branch"), {QStringLiteral("-a"), QStringLiteral("--all"), QStringLiteral("-d"), QStringLiteral("--delete"), QStringLiteral("-D"), QStringLiteral("--force-delete"), QStringLiteral("-m"), QStringLiteral("--move"), QStringLiteral("-r"), QStringLiteral("--remotes"), QStringLiteral("--show-current"), QStringLiteral("--set-upstream-to")}},
        {QStringLiteral("checkout"), {QStringLiteral("-b"), QStringLiteral("-B"), QStringLiteral("--detach"), QStringLiteral("--ours"), QStringLiteral("--theirs"), QStringLiteral("--force"), QStringLiteral("--track"), QStringLiteral("--orphan")}},
        {QStringLiteral("cherry-pick"), {QStringLiteral("--continue"), QStringLiteral("--skip"), QStringLiteral("--abort"), QStringLiteral("--quit"), QStringLiteral("-n"), QStringLiteral("--no-commit"), QStringLiteral("-x"), QStringLiteral("--signoff"), QStringLiteral("-m")}},
        {QStringLiteral("clean"), {QStringLiteral("-n"), QStringLiteral("--dry-run"), QStringLiteral("-f"), QStringLiteral("--force"), QStringLiteral("-d"), QStringLiteral("-x"), QStringLiteral("-X"), QStringLiteral("-i"), QStringLiteral("--interactive")}},
        {QStringLiteral("clone"), {QStringLiteral("--depth"), QStringLiteral("--branch"), QStringLiteral("-b"), QStringLiteral("--single-branch"), QStringLiteral("--recurse-submodules"), QStringLiteral("--filter"), QStringLiteral("--bare"), QStringLiteral("--mirror"), QStringLiteral("--origin"), QStringLiteral("--sparse")}},
        {QStringLiteral("commit"), {QStringLiteral("-m"), QStringLiteral("--message"), QStringLiteral("-a"), QStringLiteral("--all"), QStringLiteral("--amend"), QStringLiteral("--no-edit"), QStringLiteral("--allow-empty"), QStringLiteral("--no-verify"), QStringLiteral("-S"), QStringLiteral("--signoff"), QStringLiteral("--fixup"), QStringLiteral("--squash")}},
        {QStringLiteral("config"), {QStringLiteral("--global"), QStringLiteral("--local"), QStringLiteral("--system"), QStringLiteral("--get"), QStringLiteral("--get-all"), QStringLiteral("--unset"), QStringLiteral("--unset-all"), QStringLiteral("--list"), QStringLiteral("--show-origin"), QStringLiteral("--edit"), QStringLiteral("--add"), QStringLiteral("--replace-all")}},
        {QStringLiteral("diff"), {QStringLiteral("--staged"), QStringLiteral("--cached"), QStringLiteral("--stat"), QStringLiteral("--name-only"), QStringLiteral("--name-status"), QStringLiteral("--word-diff"), QStringLiteral("--color-words"), QStringLiteral("--check"), QStringLiteral("--no-index"), QStringLiteral("--merge-base"), QStringLiteral("-U"), QStringLiteral("--unified")}},
        {QStringLiteral("fetch"), {QStringLiteral("--all"), QStringLiteral("--prune"), QStringLiteral("--tags"), QStringLiteral("--force"), QStringLiteral("--depth"), QStringLiteral("--unshallow"), QStringLiteral("--dry-run"), QStringLiteral("--recurse-submodules"), QStringLiteral("--set-upstream")}},
        {QStringLiteral("init"), {QStringLiteral("--bare"), QStringLiteral("--initial-branch"), QStringLiteral("-b"), QStringLiteral("--shared"), QStringLiteral("--separate-git-dir")}},
        {QStringLiteral("log"), {QStringLiteral("--oneline"), QStringLiteral("--graph"), QStringLiteral("--decorate"), QStringLiteral("--all"), QStringLiteral("--stat"), QStringLiteral("--name-only"), QStringLiteral("--name-status"), QStringLiteral("--author"), QStringLiteral("--since"), QStringLiteral("--until"), QStringLiteral("--grep"), QStringLiteral("-n"), QStringLiteral("--format"), QStringLiteral("--follow"), QStringLiteral("--first-parent"), QStringLiteral("--reflog")}},
        {QStringLiteral("merge"), {QStringLiteral("--continue"), QStringLiteral("--abort"), QStringLiteral("--quit"), QStringLiteral("--no-commit"), QStringLiteral("--no-ff"), QStringLiteral("--ff-only"), QStringLiteral("--squash"), QStringLiteral("--strategy"), QStringLiteral("--strategy-option"), QStringLiteral("--no-edit"), QStringLiteral("--autostash")}},
        {QStringLiteral("pull"), {QStringLiteral("--rebase"), QStringLiteral("--no-rebase"), QStringLiteral("--ff-only"), QStringLiteral("--no-ff"), QStringLiteral("--autostash"), QStringLiteral("--all"), QStringLiteral("--tags"), QStringLiteral("--prune"), QStringLiteral("--no-commit"), QStringLiteral("--strategy")}},
        {QStringLiteral("push"), {QStringLiteral("-u"), QStringLiteral("--set-upstream"), QStringLiteral("--all"), QStringLiteral("--tags"), QStringLiteral("--force-with-lease"), QStringLiteral("--force"), QStringLiteral("--delete"), QStringLiteral("--dry-run"), QStringLiteral("--atomic"), QStringLiteral("--follow-tags"), QStringLiteral("--prune"), QStringLiteral("--mirror")}},
        {QStringLiteral("rebase"), {QStringLiteral("--continue"), QStringLiteral("--skip"), QStringLiteral("--abort"), QStringLiteral("--quit"), QStringLiteral("--onto"), QStringLiteral("--interactive"), QStringLiteral("-i"), QStringLiteral("--autosquash"), QStringLiteral("--autostash"), QStringLiteral("--exec"), QStringLiteral("--rebase-merges"), QStringLiteral("--root")}},
        {QStringLiteral("remote"), {QStringLiteral("-v"), QStringLiteral("--verbose"), QStringLiteral("add"), QStringLiteral("remove"), QStringLiteral("rename"), QStringLiteral("set-url"), QStringLiteral("get-url"), QStringLiteral("show"), QStringLiteral("prune"), QStringLiteral("update"), QStringLiteral("set-head")}},
        {QStringLiteral("reset"), {QStringLiteral("--soft"), QStringLiteral("--mixed"), QStringLiteral("--hard"), QStringLiteral("--keep"), QStringLiteral("--merge"), QStringLiteral("-p"), QStringLiteral("--patch"), QStringLiteral("--recurse-submodules")}},
        {QStringLiteral("restore"), {QStringLiteral("--staged"), QStringLiteral("--worktree"), QStringLiteral("-s"), QStringLiteral("--source"), QStringLiteral("-p"), QStringLiteral("--patch"), QStringLiteral("--ours"), QStringLiteral("--theirs"), QStringLiteral("--merge"), QStringLiteral("--conflict")}},
        {QStringLiteral("revert"), {QStringLiteral("--continue"), QStringLiteral("--skip"), QStringLiteral("--abort"), QStringLiteral("--quit"), QStringLiteral("--no-commit"), QStringLiteral("--edit"), QStringLiteral("--no-edit"), QStringLiteral("-m"), QStringLiteral("--mainline")}},
        {QStringLiteral("show"), {QStringLiteral("--stat"), QStringLiteral("--summary"), QStringLiteral("--name-only"), QStringLiteral("--name-status"), QStringLiteral("--format"), QStringLiteral("--pretty"), QStringLiteral("--no-patch"), QStringLiteral("--first-parent")}},
        {QStringLiteral("stash"), {QStringLiteral("push"), QStringLiteral("pop"), QStringLiteral("apply"), QStringLiteral("list"), QStringLiteral("show"), QStringLiteral("drop"), QStringLiteral("clear"), QStringLiteral("branch"), QStringLiteral("-m"), QStringLiteral("--message"), QStringLiteral("-u"), QStringLiteral("--include-untracked"), QStringLiteral("--all"), QStringLiteral("--keep-index")}},
        {QStringLiteral("status"), {QStringLiteral("--short"), QStringLiteral("--branch"), QStringLiteral("--porcelain"), QStringLiteral("--ignored"), QStringLiteral("--untracked-files"), QStringLiteral("--show-stash"), QStringLiteral("--ahead-behind"), QStringLiteral("--renames")}},
        {QStringLiteral("tag"), {QStringLiteral("-a"), QStringLiteral("--annotate"), QStringLiteral("-d"), QStringLiteral("--delete"), QStringLiteral("-l"), QStringLiteral("--list"), QStringLiteral("-v"), QStringLiteral("--verify"), QStringLiteral("-s"), QStringLiteral("--sign"), QStringLiteral("-m"), QStringLiteral("--message"), QStringLiteral("--contains"), QStringLiteral("--sort")}}
    };
}

QString optionDescription(const QString &command, const QString &option)
{
    static const QHash<QString, QString> gitCommandDescriptions = {
        {QStringLiteral("add"), QStringLiteral("Stage file contents for the next commit.")},
        {QStringLiteral("bisect"), QStringLiteral("Find the commit that introduced a bug by binary search.")},
        {QStringLiteral("blame"), QStringLiteral("Show which commit and author last changed each line.")},
        {QStringLiteral("branch"), QStringLiteral("List, create, rename, or delete branches.")},
        {QStringLiteral("checkout"), QStringLiteral("Switch branches or restore paths; prefer switch/restore for clarity.")},
        {QStringLiteral("cherry-pick"), QStringLiteral("Apply changes from existing commits to the current branch.")},
        {QStringLiteral("clean"), QStringLiteral("Remove untracked files; use -n first to preview deletions.")},
        {QStringLiteral("clone"), QStringLiteral("Copy a remote repository into a new local directory.")},
        {QStringLiteral("commit"), QStringLiteral("Record the staged snapshot as a new commit.")},
        {QStringLiteral("config"), QStringLiteral("Read or change Git configuration at system, global, or repository scope.")},
        {QStringLiteral("diff"), QStringLiteral("Compare working tree, index, commits, or paths.")},
        {QStringLiteral("fetch"), QStringLiteral("Download remote refs and objects without integrating them.")},
        {QStringLiteral("init"), QStringLiteral("Create an empty Git repository or reinitialize one.")},
        {QStringLiteral("log"), QStringLiteral("Show commit history with filters and formatting options.")},
        {QStringLiteral("merge"), QStringLiteral("Join another branch's history into the current branch.")},
        {QStringLiteral("pull"), QStringLiteral("Fetch and integrate changes from a remote branch.")},
        {QStringLiteral("push"), QStringLiteral("Update refs on a remote; --force can overwrite remote history.")},
        {QStringLiteral("rebase"), QStringLiteral("Replay commits on a new base; interactive mode can rewrite local history.")},
        {QStringLiteral("remote"), QStringLiteral("Manage named remote repositories and their URLs.")},
        {QStringLiteral("reset"), QStringLiteral("Move HEAD and optionally reset the index or working tree; --hard discards changes.")},
        {QStringLiteral("restore"), QStringLiteral("Restore working tree or staged paths from a source.")},
        {QStringLiteral("revert"), QStringLiteral("Create a new commit that reverses an earlier commit.")},
        {QStringLiteral("show"), QStringLiteral("Inspect commits, tags, trees, and blobs.")},
        {QStringLiteral("stash"), QStringLiteral("Temporarily save and later reapply working tree changes.")},
        {QStringLiteral("status"), QStringLiteral("Show staged, unstaged, and untracked changes.")},
        {QStringLiteral("tag"), QStringLiteral("Create, list, verify, or delete tags.")},
        {QStringLiteral("worktree"), QStringLiteral("Manage multiple working trees attached to one repository.")}
    };
    if (command == QLatin1String("git") && gitCommandDescriptions.contains(option))
        return gitCommandDescriptions.value(option);
    static const QHash<QString, QString> gitOptionDescriptions = {
        {QStringLiteral("add:-p"), QStringLiteral("Interactively choose individual hunks to stage.")},
        {QStringLiteral("add:-A"), QStringLiteral("Stage all additions, modifications, and deletions.")},
        {QStringLiteral("commit:--amend"), QStringLiteral("Replace the latest commit; avoid amending commits already shared.")},
        {QStringLiteral("commit:--no-verify"), QStringLiteral("Skip pre-commit and commit-msg hooks.")},
        {QStringLiteral("diff:--cached"), QStringLiteral("Compare the staged index with HEAD.")},
        {QStringLiteral("push:--force-with-lease"), QStringLiteral("Force update only if the remote ref still has the expected value.")},
        {QStringLiteral("push:--force"), QStringLiteral("Overwrite the remote ref without checking; can discard others' commits.")},
        {QStringLiteral("pull:--rebase"), QStringLiteral("Replay local commits on top of the fetched branch instead of creating a merge commit.")},
        {QStringLiteral("rebase:--continue"), QStringLiteral("Continue after resolving conflicts in the current rebase.")},
        {QStringLiteral("rebase:--abort"), QStringLiteral("Cancel the current rebase and restore the original branch state.")},
        {QStringLiteral("restore:--staged"), QStringLiteral("Unstage paths while leaving working tree contents unchanged.")},
        {QStringLiteral("restore:--source"), QStringLiteral("Choose a tree-ish or commit as the restore source.")},
        {QStringLiteral("reset:--hard"), QStringLiteral("Reset HEAD, index, and working tree; uncommitted changes can be lost.")},
        {QStringLiteral("clean:-n"), QStringLiteral("Dry run: list untracked files that would be removed.")},
        {QStringLiteral("config:--global"), QStringLiteral("Read or write the current user's ~/.gitconfig.")},
        {QStringLiteral("config:--local"), QStringLiteral("Read or write this repository's .git/config.")},
        {QStringLiteral("clone:--depth"), QStringLiteral("Create a shallow clone limited to the requested history depth.")},
        {QStringLiteral("fetch:--prune"), QStringLiteral("Remove remote-tracking refs that no longer exist on the remote.")},
        {QStringLiteral("status:--porcelain"), QStringLiteral("Print stable, script-friendly status output.")}
    };
    const QString gitOptionKey = command + QLatin1Char(':') + option;
    if (gitOptionDescriptions.contains(gitOptionKey))
        return gitOptionDescriptions.value(gitOptionKey);
    static const QHash<QString, QString> descriptions = {
        {QStringLiteral("-a"), QStringLiteral("Include hidden entries.")},
        {QStringLiteral("-l"), QStringLiteral("Use the long listing format.")},
        {QStringLiteral("-h"), QStringLiteral("Print human-readable sizes or help, depending on the command.")},
        {QStringLiteral("-i"), QStringLiteral("Ignore case distinctions.")},
        {QStringLiteral("-n"), QStringLiteral("Show line numbers or limit output, depending on the command.")},
        {QStringLiteral("-r"), QStringLiteral("Process directories recursively.")},
        {QStringLiteral("-v"), QStringLiteral("Invert the match or increase verbosity, depending on the command.")},
        {QStringLiteral("-E"), QStringLiteral("Interpret the pattern as an extended regular expression.")},
        {QStringLiteral("-F"), QStringLiteral("Interpret the pattern as a fixed string.")},
        {QStringLiteral("--color=auto"), QStringLiteral("Use color when output is connected to a terminal.")},
        {QStringLiteral("-S"), QStringLiteral("Set the CMake source directory.")},
        {QStringLiteral("-B"), QStringLiteral("Set the CMake build directory.")},
        {QStringLiteral("--build"), QStringLiteral("Build a configured CMake build directory.")},
        {QStringLiteral("-D"), QStringLiteral("Set a CMake cache variable.")},
        {QStringLiteral("--no-pager"), QStringLiteral("Do not pipe output through a pager.")},
        {QStringLiteral("--since"), QStringLiteral("Show journal entries newer than a time or date.")},
        {QStringLiteral("--user"), QStringLiteral("Operate on the calling user's service manager.")},
        {QStringLiteral("--yes"), QStringLiteral("Answer yes to prompts automatically.")},
        {QStringLiteral("-u"), QStringLiteral("Select a user or a service unit, depending on the command.")},
        {QStringLiteral("-C"), QStringLiteral("Run Git as if started in the specified directory.")}
    };
    const QString DESCRIPTION = descriptions.value(option);
    return DESCRIPTION.isEmpty()
        ? QStringLiteral("Suggested for %1. Confirm the option with the command's manual or --help.").arg(command)
        : DESCRIPTION;
}

const QHash<QString, QString> &curatedCommandDescriptions()
{
    static const QHash<QString, QString> DESCRIPTIONS = {
        {QStringLiteral("bash"), QStringLiteral("Run commands with the Bash shell.")},
        {QStringLiteral("cat"), QStringLiteral("Print or concatenate files.")},
        {QStringLiteral("cd"), QStringLiteral("Change the current shell working directory.")},
        {QStringLiteral("cmake"), QStringLiteral("Configure or build a project using CMake.")},
        {QStringLiteral("cp"), QStringLiteral("Copy files and directories.")},
        {QStringLiteral("curl"), QStringLiteral("Transfer data to or from a URL.")},
        {QStringLiteral("dnf"), QStringLiteral("Install, update, remove, or inspect Fedora packages.")},
        {QStringLiteral("echo"), QStringLiteral("Print text to standard output.")},
        {QStringLiteral("find"), QStringLiteral("Search for files in a directory hierarchy.")},
        {QStringLiteral("git"), QStringLiteral("Track changes and manage source code repositories.")},
        {QStringLiteral("grep"), QStringLiteral("Search input for lines matching a pattern.")},
        {QStringLiteral("journalctl"), QStringLiteral("Query and display systemd journal logs.")},
        {QStringLiteral("ls"), QStringLiteral("List directory contents.")},
        {QStringLiteral("mkdir"), QStringLiteral("Create directories.")},
        {QStringLiteral("mv"), QStringLiteral("Move or rename files and directories.")},
        {QStringLiteral("pacman"), QStringLiteral("Install, update, remove, or inspect Arch Linux packages.")},
        {QStringLiteral("printf"), QStringLiteral("Format and print text.")},
        {QStringLiteral("pwd"), QStringLiteral("Print the current working directory.")},
        {QStringLiteral("rm"), QStringLiteral("Remove files or directories.")},
        {QStringLiteral("sed"), QStringLiteral("Filter and transform text streams.")},
        {QStringLiteral("sudo"), QStringLiteral("Run a command with another user's privileges.")},
        {QStringLiteral("systemctl"), QStringLiteral("Inspect and control systemd services and units.")},
        {QStringLiteral("tar"), QStringLiteral("Create or extract archive files.")},
        {QStringLiteral("touch"), QStringLiteral("Create files or update file timestamps.")},
        {QStringLiteral("ubuntu-drivers"), QStringLiteral("Manage additional drivers on Ubuntu systems.")},
        {QStringLiteral("which"), QStringLiteral("Locate an executable in the current PATH.")}
    };
    return DESCRIPTIONS;
}

QString builtinDescription(const QString &name)
{
    static const QHash<QString, QString> DESCRIPTIONS = {
        {QStringLiteral("alias"), QStringLiteral("Define or display shell aliases.")},
        {QStringLiteral("bg"), QStringLiteral("Resume a suspended job in the background.")},
        {QStringLiteral("bind"), QStringLiteral("Read or change Readline key bindings and settings.")},
        {QStringLiteral("break"), QStringLiteral("Exit a loop or a specified number of enclosing loops.")},
        {QStringLiteral("builtin"), QStringLiteral("Run a shell builtin, bypassing a same-named function.")},
        {QStringLiteral("cd"), QStringLiteral("Change the current shell working directory.")},
        {QStringLiteral("command"), QStringLiteral("Run a command while bypassing a same-named shell function.")},
        {QStringLiteral("continue"), QStringLiteral("Resume the next iteration of a loop.")},
        {QStringLiteral("declare"), QStringLiteral("Declare variables and set their attributes or scope.")},
        {QStringLiteral("echo"), QStringLiteral("Print text to standard output.")},
        {QStringLiteral("eval"), QStringLiteral("Parse and execute text as shell commands.")},
        {QStringLiteral("exec"), QStringLiteral("Replace the shell with a command, or change shell redirections.")},
        {QStringLiteral("exit"), QStringLiteral("Exit the shell with an optional status code.")},
        {QStringLiteral("export"), QStringLiteral("Mark variables to be passed to child processes.")},
        {QStringLiteral("false"), QStringLiteral("Return an unsuccessful exit status.")},
        {QStringLiteral("fg"), QStringLiteral("Resume a job in the foreground.")},
        {QStringLiteral("getopts"), QStringLiteral("Parse positional parameters as command-line options.")},
        {QStringLiteral("hash"), QStringLiteral("Remember or display command locations found through PATH.")},
        {QStringLiteral("help"), QStringLiteral("Display help for Bash builtins.")},
        {QStringLiteral("history"), QStringLiteral("Display or edit the interactive shell command history.")},
        {QStringLiteral("jobs"), QStringLiteral("List active jobs started by this shell.")},
        {QStringLiteral("kill"), QStringLiteral("Send a signal to a process or job.")},
        {QStringLiteral("local"), QStringLiteral("Declare variables local to a shell function.")},
        {QStringLiteral("printf"), QStringLiteral("Format and print text.")},
        {QStringLiteral("pwd"), QStringLiteral("Print the current working directory.")},
        {QStringLiteral("read"), QStringLiteral("Read a line from standard input into shell variables.")},
        {QStringLiteral("readonly"), QStringLiteral("Mark variables or functions as read-only.")},
        {QStringLiteral("return"), QStringLiteral("Return from a shell function or sourced script.")},
        {QStringLiteral("set"), QStringLiteral("Change shell options or positional parameters.")},
        {QStringLiteral("shift"), QStringLiteral("Remove positional parameters from the start of the list.")},
        {QStringLiteral("source"), QStringLiteral("Read and execute a script in the current shell.")},
        {QStringLiteral("test"), QStringLiteral("Evaluate a conditional expression and return its result.")},
        {QStringLiteral("times"), QStringLiteral("Display accumulated user and system CPU times for the shell and its children.")},
        {QStringLiteral("trap"), QStringLiteral("Run commands when the shell receives signals or other conditions.")},
        {QStringLiteral("true"), QStringLiteral("Return a successful exit status.")},
        {QStringLiteral("type"), QStringLiteral("Describe how Bash interprets a command name.")},
        {QStringLiteral("ulimit"), QStringLiteral("Display or change resource limits for the shell and its children.")},
        {QStringLiteral("umask"), QStringLiteral("Display or set the default permission mask for new files.")},
        {QStringLiteral("unalias"), QStringLiteral("Remove one or more shell aliases.")},
        {QStringLiteral("unset"), QStringLiteral("Remove variables or functions from the shell environment.")}
    };
    return DESCRIPTIONS.value(name, QStringLiteral("Bash builtin command."));
}

bool insideSingleQuotedString(const QString &source, qsizetype end)
{
    bool singleQuoted = false;
    bool doubleQuoted = false;
    bool comment = false;
    bool escaped = false;
    for (qsizetype index = 0; index < end; ++index) {
        const QChar CHARACTER = source.at(index);
        if (comment) {
            if (CHARACTER == QLatin1Char('\n'))
                comment = false;
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (CHARACTER == QLatin1Char('\\') && !singleQuoted) {
            escaped = true;
            continue;
        }
        if (!singleQuoted && !doubleQuoted && CHARACTER == QLatin1Char('#') &&
            (index == 0 || source.at(index - 1).isSpace() || QStringLiteral(";|&(").contains(source.at(index - 1)))) {
            comment = true;
            continue;
        }
        if (CHARACTER == QLatin1Char('\'') && !doubleQuoted)
            singleQuoted = !singleQuoted;
        else if (CHARACTER == QLatin1Char('"') && !singleQuoted)
            doubleQuoted = !doubleQuoted;
    }
    return singleQuoted;
}
}

CompletionController::CompletionController(QPlainTextEdit *targetEditor, QObject *parent)
    : QObject(parent), editor(targetEditor)
{
    workingDirectory = QDir::currentPath();
    popup = new QListWidget(editor);
    popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setFocusPolicy(Qt::NoFocus);
    popup->setMouseTracking(true);
    popup->setMaximumHeight(240);
    popup->setMinimumWidth(340);
    popup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    popup->hide();
    popup->setStyleSheet(QStringLiteral("QListWidget { background: #25282d; color: #e5e7eb; border: 1px solid #697586; } QListWidget::item:selected { background: #46566b; }"));
    popup->setStyleSheet(QStringLiteral("QListWidget { background: #25282d; color: #e5e7eb; border: 1px solid #697586; } QListWidget::item:selected { background: #46566b; } QToolTip { background: #25282d; color: #e5e7eb; border: 1px solid #697586; padding: 6px; }"));

    descriptionLookupProcess = new QProcess(this);
    connect(descriptionLookupProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        if (activeDescriptionLookupGeneration == descriptionLookupGeneration)
            consumeDescriptionLookupOutput();
        else
            descriptionLookupProcess->readAllStandardOutput();
    });
    connect(descriptionLookupProcess,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        if (activeDescriptionLookupGeneration == descriptionLookupGeneration)
            consumeDescriptionLookupOutput();
        startNextDescriptionLookupBatch();
        updateSuggestions();
    });
    connect(descriptionLookupProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart &&
            activeDescriptionLookupGeneration == descriptionLookupGeneration) {
            descriptionLookupOffset = descriptionLookupNames.size();
        }
    });

    connect(editor, &QPlainTextEdit::textChanged, this, [this]() { updateSuggestions(); });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this]() { updateSuggestions(); });
    selectedDescriptionTimer.setSingleShot(true);
    selectedDescriptionTimer.setInterval(40);
    connect(&selectedDescriptionTimer, &QTimer::timeout, this, [this]() { showSelectedDescription(); });
    connect(popup, &QListWidget::currentRowChanged, this, [this](int) {
        QToolTip::hideText();
        if (popup->isVisible())
            selectedDescriptionTimer.start();
    });
    connect(popup, &QListWidget::itemClicked, this, [this](QListWidgetItem *) { acceptCurrentSuggestion(); });
    editor->installEventFilter(this);
    editor->setMouseTracking(true);
    editor->viewport()->setMouseTracking(true);
    editor->viewport()->installEventFilter(this);
    hoverDescriptionTimer.setSingleShot(true);
    hoverDescriptionTimer.setInterval(500);
    connect(&hoverDescriptionTimer, &QTimer::timeout, this, [this]() { showHoverDescription(); });
    refreshCommandCache();
    refreshSnippets();
}

void CompletionController::setWorkingDirectory(const QString &path)
{
    workingDirectory = path.isEmpty() ? QDir::currentPath() : QDir::cleanPath(path);
    updateSuggestions();
}

bool CompletionController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == editor && event->type() == QEvent::FocusOut) {
        selectedDescriptionTimer.stop();
        popup->hide();
        hoverDescriptionTimer.stop();
        hoverTokenStart = -1;
        hoverDescription.clear();
        QToolTip::hideText();
    }
    if (watched == editor->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            updateHoverTarget(mouseEvent->pos());
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
            hoverDescriptionTimer.stop();
            hoverTokenStart = -1;
            hoverDescription.clear();
            QToolTip::hideText();
        }
    }
    if (watched == editor && event->type() == QEvent::KeyPress) {
        // Keep hover help away while the user is typing, then let it return
        // after the same quiet interval used for the initial hover.
        QToolTip::hideText();
        if (hoverTokenStart >= 0)
            hoverDescriptionTimer.start();
    }
    if (watched == editor && event->type() == QEvent::KeyPress && popup->isVisible()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int DIRECTION = key->key() == Qt::Key_Down ? 1 : -1;
            const int COUNT = popup->count();
            if (COUNT > 0) {
                int row = popup->currentRow();
                row = (row + DIRECTION + COUNT) % COUNT;
                popup->setCurrentRow(row);
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            acceptCurrentSuggestion();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            popup->hide();
            QToolTip::hideText();
            return true;
        }
        if (key->key() == Qt::Key_Tab && popup->count() > 0) {
            acceptCurrentSuggestion();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

QString CompletionController::descriptionForTokenAt(const QPoint &position, qsizetype *tokenStart) const
{
    if (tokenStart)
        *tokenStart = -1;
    if (!editor)
        return {};
    const QString SOURCE = editor->toPlainText();
    const QTextCursor CURSOR = editor->cursorForPosition(position);
    qsizetype offset = CURSOR.position();
    if (offset == SOURCE.size() && offset > 0)
        --offset;
    const QVector<Syntax::SyntaxToken> TOKENS = Syntax::SyntaxTokenizer::tokenize(SOURCE);
    const Syntax::SyntaxToken *hovered = nullptr;
    for (const auto &token : TOKENS) {
        if (offset >= token.startOffset && offset < token.startOffset + token.length) {
            hovered = &token;
            break;
        }
    }
    if (!hovered)
        return {};
    if (tokenStart)
        *tokenStart = hovered->startOffset;

    QString command;
    for (const auto &token : TOKENS) {
        if (token.startOffset > hovered->startOffset)
            break;
        if (token.kind == Syntax::TokenKind::Operator) {
            const QString OPERATOR = SOURCE.mid(token.startOffset, token.length);
            if (OPERATOR == QStringLiteral(";") || OPERATOR == QStringLiteral("&&") ||
                OPERATOR == QStringLiteral("||") || OPERATOR == QStringLiteral("|") ||
                OPERATOR == QStringLiteral("|&"))
                command.clear();
        } else if (token.kind == Syntax::TokenKind::Command || token.kind == Syntax::TokenKind::Builtin) {
            command = SOURCE.mid(token.startOffset, token.length);
        }
    }

    const QString TEXT = SOURCE.mid(hovered->startOffset, hovered->length);
    switch (hovered->kind) {
    case Syntax::TokenKind::Variable: {
        QString name = TEXT;
        if (name.startsWith(QStringLiteral("${")) && name.endsWith(QLatin1Char('}')))
            name = name.mid(2, name.size() - 3);
        else if (name.startsWith(QLatin1Char('$')))
            name.remove(0, 1);
        const QString declaration = runtimeVariables.value(name);
        return declaration.isEmpty() ? QString() : QStringLiteral("Runtime value: %1").arg(declaration);
    }
    case Syntax::TokenKind::Command:
        return commandDescription(TEXT);
    case Syntax::TokenKind::Builtin:
        return builtinDescription(TEXT);
    case Syntax::TokenKind::Keyword: {
        static const QHash<QString, QString> DESCRIPTIONS = {
            {QStringLiteral("if"), QStringLiteral("Run a conditional branch when its test succeeds.")},
            {QStringLiteral("then"), QStringLiteral("Begin the body of a conditional branch.")},
            {QStringLiteral("elif"), QStringLiteral("Test another conditional branch.")},
            {QStringLiteral("else"), QStringLiteral("Begin the fallback branch of a conditional.")},
            {QStringLiteral("fi"), QStringLiteral("End an if statement.")},
            {QStringLiteral("for"), QStringLiteral("Repeat a command block for each value in a list.")},
            {QStringLiteral("while"), QStringLiteral("Repeat a command block while its test succeeds.")},
            {QStringLiteral("until"), QStringLiteral("Repeat a command block until its test succeeds.")},
            {QStringLiteral("do"), QStringLiteral("Begin the body of a loop.")},
            {QStringLiteral("done"), QStringLiteral("End a loop.")},
            {QStringLiteral("case"), QStringLiteral("Match a value against a list of patterns.")},
            {QStringLiteral("esac"), QStringLiteral("End a case statement.")},
            {QStringLiteral("in"), QStringLiteral("Introduce a list of values or case patterns.")},
            {QStringLiteral("function"), QStringLiteral("Declare a shell function.")},
            {QStringLiteral("select"), QStringLiteral("Create a menu loop over a list of values.")},
            {QStringLiteral("time"), QStringLiteral("Measure the time taken by a command.")},
            {QStringLiteral("coproc"), QStringLiteral("Run a command asynchronously with a coprocess.")}
        };
        return DESCRIPTIONS.value(TEXT);
    }
    case Syntax::TokenKind::Option:
        return optionDescription(command.section(QLatin1Char('/'), -1).toLower(), TEXT);
    case Syntax::TokenKind::SpecialParameter: {
        static const QHash<QString, QString> DESCRIPTIONS = {
            {QStringLiteral("$?"), QStringLiteral("Exit status of the most recently completed command.")},
            {QStringLiteral("$#"), QStringLiteral("Number of positional parameters passed to the script or function.")},
            {QStringLiteral("$@"), QStringLiteral("All positional parameters, preserved as separate words when quoted.")},
            {QStringLiteral("$*"), QStringLiteral("All positional parameters, joined into a single word when quoted.")},
            {QStringLiteral("$$"), QStringLiteral("Process ID of the current shell.")},
            {QStringLiteral("$!"), QStringLiteral("Process ID of the most recently backgrounded job.")}
        };
        return DESCRIPTIONS.value(TEXT);
    }
    case Syntax::TokenKind::Redirection:
        if (TEXT.startsWith(QStringLiteral("<<")))
            return QStringLiteral("Read a here-document until its delimiter.");
        if (TEXT == QStringLiteral(">>") || TEXT == QStringLiteral("&>>"))
            return QStringLiteral("Append command output to a file.");
        if (TEXT.contains(QLatin1Char('>')))
            return QStringLiteral("Redirect command output.");
        return QStringLiteral("Redirect command input.");
    case Syntax::TokenKind::Operator:
        if (TEXT == QStringLiteral("&&")) return QStringLiteral("Run the next command only if this command succeeds.");
        if (TEXT == QStringLiteral("||")) return QStringLiteral("Run the next command only if this command fails.");
        if (TEXT == QStringLiteral("|")) return QStringLiteral("Send this command's output to the next command's input.");
        if (TEXT == QStringLiteral(";")) return QStringLiteral("Separate commands in the same statement.");
        return {};
    default:
        return {};
    }
}

void CompletionController::updateHoverTarget(const QPoint &position)
{
    hoverPosition = position;
    qsizetype tokenStart = -1;
    const QString DESCRIPTION = descriptionForTokenAt(position, &tokenStart);
    if (tokenStart != hoverTokenStart) {
        hoverTokenStart = tokenStart;
        hoverDescription = DESCRIPTION;
        hoverDescriptionTimer.stop();
        QToolTip::hideText();
        if (tokenStart >= 0 && !DESCRIPTION.isEmpty())
            hoverDescriptionTimer.start();
    } else {
        hoverDescription = DESCRIPTION;
        if (QToolTip::isVisible() && !DESCRIPTION.isEmpty())
            QToolTip::showText(editor->viewport()->mapToGlobal(position), DESCRIPTION, editor->viewport());
        else if (DESCRIPTION.isEmpty()) {
            hoverDescriptionTimer.stop();
            QToolTip::hideText();
        } else if (!hoverDescriptionTimer.isActive())
            hoverDescriptionTimer.start();
    }
}

void CompletionController::showHoverDescription()
{
    if (hoverTokenStart < 0 || hoverDescription.isEmpty() || popup->isVisible())
        return;
    qsizetype currentTokenStart = -1;
    const QString DESCRIPTION = descriptionForTokenAt(hoverPosition, &currentTokenStart);
    if (currentTokenStart != hoverTokenStart || DESCRIPTION.isEmpty())
        return;
    hoverDescription = DESCRIPTION;
    QToolTip::showText(editor->viewport()->mapToGlobal(hoverPosition), DESCRIPTION, editor->viewport());
}

void CompletionController::refreshCommandCache()
{
    ++descriptionLookupGeneration;
    if (descriptionLookupProcess->state() != QProcess::NotRunning)
        descriptionLookupProcess->kill();
    systemCommands.clear();
    commandDescriptions.clear();
    descriptionLookupNames.clear();
    descriptionLookupOffset = 0;
    QSet<QString> seen;
    const QString PATH = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PATH"));
    for (const QString &directory : PATH.split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        const QDir dir(directory);
        const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Executable | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString NAME = entry.fileName();
            if (!seen.contains(NAME)) {
                seen.insert(NAME);
                systemCommands.append(NAME);
            }
        }
    }
    systemCommands.sort(Qt::CaseInsensitive);
    descriptionLookupNames = systemCommands;
    descriptionLookupProgram = QStandardPaths::findExecutable(QStringLiteral("whatis"));
    descriptionLookupArguments.clear();
    if (descriptionLookupProgram.isEmpty()) {
        descriptionLookupProgram = QStandardPaths::findExecutable(QStringLiteral("man"));
        descriptionLookupArguments = {QStringLiteral("-f")};
    }
    if (!descriptionLookupProgram.isEmpty())
        startNextDescriptionLookupBatch();
    updateSuggestions();
}

void CompletionController::refreshSnippets()
{
    snippets = Snippets::SnippetStore::loadAll();
    snippets += Snippets::SnippetStore::builtInSnippets();
    updateSuggestions();
}

void CompletionController::setProfileEnvironmentVariables(const QStringList &names)
{
    profileEnvironmentVariables = names;
    updateSuggestions();
}

void CompletionController::setRuntimeVariables(const QHash<QString, QString> &variables)
{
    runtimeVariables = variables;
    if (editor && editor->hasFocus())
        updateSuggestions();
    if (hoverTokenStart >= 0)
        updateHoverTarget(hoverPosition);
}

void CompletionController::setSourcedScriptSymbols(const QVector<ScriptSymbol> &symbols)
{
    sourcedScriptSymbols = symbols;
    if (editor)
        refreshScriptSymbols(editor->toPlainText());
}

void CompletionController::setAdditionalScriptSymbols(const QVector<ScriptSymbol> &symbols)
{
    additionalScriptSymbols = symbols;
    if (editor) {
        refreshScriptSymbols(editor->toPlainText());
        updateSuggestions();
    }
}

bool CompletionController::hasVisibleSuggestions() const
{
    return popup && popup->isVisible();
}

void CompletionController::startNextDescriptionLookupBatch()
{
    if (!descriptionLookupProcess || descriptionLookupProcess->state() != QProcess::NotRunning ||
        descriptionLookupProgram.isEmpty() || descriptionLookupOffset >= descriptionLookupNames.size())
        return;
    const qsizetype BATCH_SIZE = 96;
    const qsizetype END = qMin(descriptionLookupNames.size(), descriptionLookupOffset + BATCH_SIZE);
    QStringList arguments = descriptionLookupArguments;
    for (qsizetype index = descriptionLookupOffset; index < END; ++index)
        arguments.append(descriptionLookupNames.at(index));
    descriptionLookupOffset = END;
    descriptionLookupBuffer.clear();
    activeDescriptionLookupGeneration = descriptionLookupGeneration;
    descriptionLookupProcess->setProgram(descriptionLookupProgram);
    descriptionLookupProcess->setArguments(arguments);
    descriptionLookupProcess->start();
}

void CompletionController::consumeDescriptionLookupOutput()
{
    descriptionLookupBuffer += QString::fromLocal8Bit(descriptionLookupProcess->readAllStandardOutput());
    qsizetype NEWLINE = -1;
    while ((NEWLINE = descriptionLookupBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString LINE = descriptionLookupBuffer.left(NEWLINE).trimmed();
        descriptionLookupBuffer.remove(0, NEWLINE + 1);
        parseDescriptionLookupLine(LINE);
    }
    if (descriptionLookupProcess->state() == QProcess::NotRunning && !descriptionLookupBuffer.isEmpty()) {
        parseDescriptionLookupLine(descriptionLookupBuffer.trimmed());
        descriptionLookupBuffer.clear();
    }
}

void CompletionController::parseDescriptionLookupLine(const QString &line)
{
    static const QRegularExpression WHATIS_LINE(
        QStringLiteral(R"(^([^\s,]+)\s+\([^)]*\)\s+-\s+(.+)$)"));
    const auto MATCH = WHATIS_LINE.match(line);
    if (MATCH.hasMatch())
        commandDescriptions.insert(MATCH.captured(1).toLower(), MATCH.captured(2).trimmed());
}

QString CompletionController::commandDescription(const QString &command) const
{
    const QString NAME = command.section(QLatin1Char('/'), -1).toLower();
    const QString MANUAL_DESCRIPTION = commandDescriptions.value(NAME);
    if (!MANUAL_DESCRIPTION.isEmpty())
        return MANUAL_DESCRIPTION;
    const QString CURATED_DESCRIPTION = curatedCommandDescriptions().value(NAME);
    if (!CURATED_DESCRIPTION.isEmpty())
        return CURATED_DESCRIPTION;
    return QStringLiteral("Executable found in the current PATH. No local manual-page summary was found.");
}

void CompletionController::refreshScriptSymbols(const QString &source)
{
    knownScriptSymbols.clear();
    const QVector<Syntax::SyntaxToken> TOKENS = Syntax::SyntaxTokenizer::tokenize(source);
    auto isIdentifier = [](const QString &name) {
        static const QRegularExpression IDENTIFIER(QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_]*$)"));
        return IDENTIFIER.match(name).hasMatch();
    };
    auto isInNonCodeText = [&TOKENS](qsizetype offset) {
        for (const Syntax::SyntaxToken &token : TOKENS) {
            if (offset >= token.startOffset && offset < token.startOffset + token.length)
                return token.kind == Syntax::TokenKind::Comment ||
                       token.kind == Syntax::TokenKind::StringContent ||
                       token.kind == Syntax::TokenKind::StringDelimiter ||
                       token.kind == Syntax::TokenKind::HereDocContent;
        }
        return false;
    };
    QSet<qsizetype> functionOffsets;
    auto addSymbol = [this, &source](const QString &name, ScriptSymbol::Kind kind, qsizetype offset,
                                     bool readOnly = false) {
        if (name.isEmpty() || offset < 0 || offset >= source.size())
            return;
        QString value;
        if (kind == ScriptSymbol::Kind::Variable &&
            source.mid(offset + name.size(), 1) == QLatin1String("=")) {
            qsizetype valueStart = offset + name.size() + 1;
            qsizetype valueEnd = valueStart;
            QChar quote;
            while (valueEnd < source.size()) {
                const QChar character = source.at(valueEnd);
                if (character == QLatin1Char('\\') && valueEnd + 1 < source.size()) {
                    valueEnd += 2;
                    continue;
                }
                if (!quote.isNull()) {
                    if (character == quote)
                        quote = QChar();
                } else if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
                    quote = character;
                } else if (character.isSpace() || character == QLatin1Char(';') ||
                           character == QLatin1Char('&') || character == QLatin1Char('|')) {
                    break;
                }
                ++valueEnd;
            }
            value = source.mid(valueStart, valueEnd - valueStart);
        }
        for (ScriptSymbol &existing : knownScriptSymbols) {
            if (existing.kind == kind && existing.declarationOffset == offset && existing.name == name) {
                existing.readOnly = existing.readOnly || readOnly;
                if (existing.value.isEmpty())
                    existing.value = value;
                return;
            }
        }
        int line = 1;
        int column = 1;
        for (qsizetype index = 0; index < offset; ++index) {
            if (source.at(index) == QLatin1Char('\n')) {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        knownScriptSymbols.append(ScriptSymbol{name, kind, offset, line, column, readOnly, value});
    };

    for (const Syntax::SyntaxToken &token : TOKENS) {
        if (token.kind == Syntax::TokenKind::AssignmentName && !isInNonCodeText(token.startOffset)) {
            const QString NAME = source.mid(token.startOffset, token.length);
            if (isIdentifier(NAME)) {
                addSymbol(NAME, ScriptSymbol::Kind::Variable, token.startOffset);
            }
        }
    }

    static const QSet<QString> DECLARATIONS = {
        QStringLiteral("declare"), QStringLiteral("typeset"), QStringLiteral("export"),
        QStringLiteral("local"), QStringLiteral("readonly"), QStringLiteral("read")
    };
    for (qsizetype index = 0; index < TOKENS.size(); ++index) {
        const Syntax::SyntaxToken &declaration = TOKENS.at(index);
        const QString DECLARATION = source.mid(declaration.startOffset, declaration.length);
        if (declaration.kind != Syntax::TokenKind::Builtin || !DECLARATIONS.contains(DECLARATION))
            continue;
        bool readOnlyDeclaration = DECLARATION == QLatin1String("readonly");
        bool previousWasEquals = false;
        for (qsizetype next = index + 1; next < TOKENS.size(); ++next) {
            const Syntax::SyntaxToken &candidate = TOKENS.at(next);
            if (candidate.line != declaration.line || candidate.kind == Syntax::TokenKind::Comment)
                break;
            const QString TEXT = source.mid(candidate.startOffset, candidate.length);
            if (candidate.kind == Syntax::TokenKind::Operator) {
                if (TEXT == QStringLiteral(";") || TEXT == QStringLiteral("&&") ||
                    TEXT == QStringLiteral("||") || TEXT == QStringLiteral("|"))
                    break;
                previousWasEquals = TEXT == QStringLiteral("=");
                continue;
            }
            if ((DECLARATION == QLatin1String("declare") || DECLARATION == QLatin1String("typeset")) &&
                candidate.kind == Syntax::TokenKind::Option && TEXT.size() > 1 && TEXT.mid(1).contains(QLatin1Char('r')))
                readOnlyDeclaration = true;
            if (!previousWasEquals && (candidate.kind == Syntax::TokenKind::AssignmentName ||
                                       candidate.kind == Syntax::TokenKind::Argument ||
                                       candidate.kind == Syntax::TokenKind::Word) &&
                isIdentifier(TEXT) && !isInNonCodeText(candidate.startOffset)) {
                addSymbol(TEXT, ScriptSymbol::Kind::Variable, candidate.startOffset, readOnlyDeclaration);
            }
            previousWasEquals = false;
        }
    }

    // Bash supports both `function name { ... }` and `name() { ... }` forms.
    static const QRegularExpression FUNCTION_DECLARATION(
        QStringLiteral(R"((?:^|[;\n]\s*)(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(\s*\))?\s*\{)"));
    auto functionMatches = FUNCTION_DECLARATION.globalMatch(source);
    while (functionMatches.hasNext()) {
        const auto MATCH = functionMatches.next();
        const qsizetype OFFSET = MATCH.capturedStart(1);
        if (isInNonCodeText(OFFSET) || functionOffsets.contains(OFFSET))
            continue;
        addSymbol(MATCH.captured(1), ScriptSymbol::Kind::Function, OFFSET);
        functionOffsets.insert(OFFSET);
    }
    for (ScriptSymbol &symbol : knownScriptSymbols) {
        if (symbol.kind != ScriptSymbol::Kind::Variable || !symbol.readOnly || !symbol.value.isEmpty())
            continue;
        const ScriptSymbol *latestValue = nullptr;
        for (const ScriptSymbol &candidate : knownScriptSymbols) {
            if (candidate.kind == ScriptSymbol::Kind::Variable && candidate.name == symbol.name &&
                candidate.declarationOffset < symbol.declarationOffset && !candidate.value.isEmpty() &&
                (!latestValue || candidate.declarationOffset > latestValue->declarationOffset))
                latestValue = &candidate;
        }
        if (latestValue)
            symbol.value = latestValue->value;
    }

    for (const ScriptSymbol &externalSymbol : sourcedScriptSymbols) {
        const bool alreadyKnown = std::any_of(knownScriptSymbols.cbegin(), knownScriptSymbols.cend(),
            [&externalSymbol](const ScriptSymbol &symbol) { return symbol.name == externalSymbol.name; });
        if (!alreadyKnown)
            knownScriptSymbols.append(externalSymbol);
    }
    for (const ScriptSymbol &additionalSymbol : additionalScriptSymbols) {
        const bool alreadyKnown = std::any_of(knownScriptSymbols.cbegin(), knownScriptSymbols.cend(),
            [&additionalSymbol](const ScriptSymbol &symbol) { return symbol.name == additionalSymbol.name; });
        if (!alreadyKnown)
            knownScriptSymbols.append(additionalSymbol);
    }
}

void CompletionController::updateSuggestions()
{
    if (!editor)
        return;
    if (!editor->hasFocus()) {
        popup->hide();
        return;
    }
    const QString SOURCE = editor->toPlainText();
    refreshScriptSymbols(SOURCE);
    const QTextCursor CURSOR = editor->textCursor();
    const qsizetype POSITION = CURSOR.position();
    const QVector<Syntax::SyntaxToken> DOCUMENT_TOKENS = Syntax::SyntaxTokenizer::tokenize(SOURCE);
    const bool IN_HEREDOC = std::any_of(DOCUMENT_TOKENS.cbegin(), DOCUMENT_TOKENS.cend(),
        [POSITION](const Syntax::SyntaxToken &token) {
            return token.kind == Syntax::TokenKind::HereDocContent &&
                   POSITION >= token.startOffset && POSITION <= token.startOffset + token.length;
        });
    if (IN_HEREDOC) {
        popup->hide();
        QToolTip::hideText();
        return;
    }
    if (CURSOR.hasSelection()) {
        popup->hide();
        QToolTip::hideText();
        return;
    }

    const QVector<Syntax::SyntaxToken> TOKENS = Syntax::SyntaxTokenizer::tokenize(SOURCE.left(POSITION));
    const Syntax::SyntaxToken *CONTEXT_TOKEN = nullptr;
    for (const Syntax::SyntaxToken &token : TOKENS) {
        if (POSITION > token.startOffset && POSITION <= token.startOffset + token.length) {
            CONTEXT_TOKEN = &token;
            break;
        }
    }
    if (CONTEXT_TOKEN && (CONTEXT_TOKEN->kind == Syntax::TokenKind::Comment ||
                          CONTEXT_TOKEN->kind == Syntax::TokenKind::HereDocContent)) {
        popup->hide();
        QToolTip::hideText();
        return;
    }

    qsizetype wordStart = POSITION;
    while (wordStart > 0) {
        const QChar CHAR = SOURCE.at(wordStart - 1);
        if (!(CHAR.isLetterOrNumber() || CHAR == QLatin1Char('_') || CHAR == QLatin1Char('-') ||
              CHAR == QLatin1Char('.') || CHAR == QLatin1Char('/') || CHAR == QLatin1Char(':')))
            break;
        --wordStart;
    }
    qsizetype wordEnd = POSITION;
    while (wordEnd < SOURCE.size()) {
        const QChar CHAR = SOURCE.at(wordEnd);
        if (!(CHAR.isLetterOrNumber() || CHAR == QLatin1Char('_') || CHAR == QLatin1Char('-') ||
              CHAR == QLatin1Char('.') || CHAR == QLatin1Char('/') || CHAR == QLatin1Char(':')))
            break;
        ++wordEnd;
    }
    const bool BRACED_VARIABLE = wordStart >= 2 && SOURCE.at(wordStart - 1) == QLatin1Char('{') &&
                                 SOURCE.at(wordStart - 2) == QLatin1Char('$');
    const bool SIMPLE_VARIABLE = wordStart >= 1 && SOURCE.at(wordStart - 1) == QLatin1Char('$');
    const bool SPECIAL_VARIABLE = POSITION >= 2 && SOURCE.at(POSITION - 2) == QLatin1Char('$') &&
        QStringLiteral("?#@*!$0123456789").contains(SOURCE.at(POSITION - 1));
    const bool VARIABLE_POSITION = BRACED_VARIABLE || SIMPLE_VARIABLE || SPECIAL_VARIABLE;
    QString PREFIX = SOURCE.mid(wordStart, POSITION - wordStart);
    if (SPECIAL_VARIABLE)
        PREFIX = SOURCE.mid(POSITION - 1, 1);
    replacementStart = BRACED_VARIABLE ? wordStart - 2
        : (SPECIAL_VARIABLE ? POSITION - 2 : (SIMPLE_VARIABLE ? wordStart - 1 : wordStart));
    replacementEnd = SPECIAL_VARIABLE ? POSITION : wordEnd;
    if (BRACED_VARIABLE && replacementEnd < SOURCE.size() && SOURCE.at(replacementEnd) == QLatin1Char('}'))
        ++replacementEnd;
    const QTextBlock BLOCK = editor->document()->findBlock(POSITION);
    const QString BEFORE_WORD = BLOCK.text().left(wordStart - BLOCK.position()).trimmed();
    const bool ENDS_WITH_BOUNDARY = BEFORE_WORD.isEmpty() ||
        QRegularExpression(QStringLiteral(R"((?:^|&&|\|\||[;|])\s*$)" )).match(BEFORE_WORD).hasMatch();
    const bool AFTER_CONTROL = QRegularExpression(QStringLiteral(R"((?:^|\s)(?:if|then|else|elif|while|until|do|!|time)\s*$)"))
                                   .match(BEFORE_WORD).hasMatch();
    static const QRegularExpression SUDO_WITHOUT_COMMAND(
        QStringLiteral(R"(^\s*(?:(?:[A-Za-z_][A-Za-z0-9_]*=\S*)\s+)*sudo(?:\s+(?:(?:-u|-g|-h|-p|-C)\s+\S+|--?[A-Za-z][A-Za-z0-9-]*))*\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const bool AFTER_SUDO_PREFIX = SUDO_WITHOUT_COMMAND.match(BEFORE_WORD).hasMatch();
    commandPosition = ENDS_WITH_BOUNDARY || AFTER_CONTROL || AFTER_SUDO_PREFIX;
    currentCommand.clear();
    if (!commandPosition) {
        static const QRegularExpression COMMAND_MATCH(
            QStringLiteral(R"(^\s*(?:(?:[A-Za-z_][A-Za-z0-9_]*=\S*)\s+)*(?:command\s+|sudo\s+(?:(?:-u|-g|-h|-p|-C)\s+\S+\s+|--?[A-Za-z][A-Za-z0-9-]*\s+)*)?([^\s;&|]+))"),
            QRegularExpression::CaseInsensitiveOption);
        const auto MATCH = COMMAND_MATCH.match(BEFORE_WORD);
        if (MATCH.hasMatch())
            currentCommand = MATCH.captured(1);
    }
    const QString NORMALIZED_COMMAND = currentCommand.section(QLatin1Char('/'), -1).toLower();
    const bool SOURCE_PATH_POSITION = !commandPosition &&
        (NORMALIZED_COMMAND == QLatin1String("source") || NORMALIZED_COMMAND == QLatin1String("."));
    const bool IN_STRING = CONTEXT_TOKEN &&
        (CONTEXT_TOKEN->kind == Syntax::TokenKind::StringContent ||
         CONTEXT_TOKEN->kind == Syntax::TokenKind::StringDelimiter);
    if (!VARIABLE_POSITION && !SOURCE_PATH_POSITION &&
        (IN_STRING || insideSingleQuotedString(SOURCE, POSITION))) {
        popup->hide();
        QToolTip::hideText();
        return;
    }

    currentSuggestions.clear();
    auto append = [this](const QString &text, const QString &description, const QString &category) {
        currentSuggestions.append(Suggestion{text, description, category, QString(), {}, {}});
    };
    if (VARIABLE_POSITION) {
        QMap<QString, QString> variables;
        auto addVariable = [&variables](const QString &name, const QString &source) {
            QString description = variables.value(name);
            if (!description.isEmpty())
                description += QLatin1Char('\n');
            if (!description.contains(source))
                description += source;
            variables.insert(name, description);
        };
        const QProcessEnvironment ENVIRONMENT = QProcessEnvironment::systemEnvironment();
        for (const QString &name : ENVIRONMENT.keys())
            addVariable(name, QStringLiteral("Inherited environment variable."));
        for (auto it = runtimeVariables.cbegin(); it != runtimeVariables.cend(); ++it)
            addVariable(it.key(), QStringLiteral("Variable captured from the paused Bash process.\n%1").arg(it.value()));
        for (const QString &name : profileEnvironmentVariables)
            addVariable(name, QStringLiteral("Active execution profile override."));
        for (const ScriptSymbol &symbol : knownScriptSymbols) {
            if (symbol.kind != ScriptSymbol::Kind::Variable)
                continue;
            addVariable(symbol.name, QStringLiteral("Declared or assigned in this script at line %1.")
                .arg(symbol.declarationLine));
            if (symbol.readOnly) {
                QString &description = variables[symbol.name];
                if (!description.contains(QStringLiteral("Read-only variable."))) {
                    const QString VALUE = symbol.value.isEmpty() ? QStringLiteral("(empty)") : symbol.value;
                    description += QStringLiteral("\nRead-only variable. Value: %1").arg(VALUE);
                }
            }
        }
        for (auto it = variables.cbegin(); it != variables.cend(); ++it) {
            const QString COMPLETION = BRACED_VARIABLE
                ? QStringLiteral("${%1}").arg(it.key()) : QStringLiteral("$%1").arg(it.key());
            const bool IS_ENVIRONMENT = it.value().contains(QStringLiteral("Inherited environment"));
            const bool IS_PROFILE = it.value().contains(QStringLiteral("execution profile"), Qt::CaseInsensitive);
            const bool IS_SCRIPT = it.value().contains(QStringLiteral("script."), Qt::CaseInsensitive);
            QStringList SOURCES;
            if (IS_ENVIRONMENT) SOURCES.append(QStringLiteral("Environment"));
            if (IS_PROFILE) SOURCES.append(QStringLiteral("Profile"));
            if (IS_SCRIPT) SOURCES.append(QStringLiteral("Script"));
            const QString CATEGORY = SOURCES.isEmpty()
                ? QStringLiteral("Script variable") : SOURCES.join(QStringLiteral(" + "));
            currentSuggestions.append(Suggestion{COMPLETION, it.value(), CATEGORY, QString(), {}, {}});
        }
        for (const QString &special : {QStringLiteral("$?"), QStringLiteral("$#"), QStringLiteral("$@"),
                                       QStringLiteral("$*"), QStringLiteral("$!"), QStringLiteral("$$")}) {
            currentSuggestions.append(Suggestion{special, QStringLiteral("Special Bash parameter."),
                                                 QStringLiteral("Special parameter"), QString(), {}, {}});
        }
    } else if (commandPosition) {
        for (const Snippets::Snippet &snippet : snippets) {
            currentSuggestions.append(Suggestion{snippet.name, snippet.description,
                                                 QStringLiteral("Snippet"), snippet.code, snippet.highlightPoints,
                                                 snippet.autoChangeSections});
        }
        if (!AFTER_SUDO_PREFIX) {
            for (const QString &word : KEYWORDS)
                append(word, QStringLiteral("Bash reserved word."), QStringLiteral("Keyword"));
        }
        for (const QString &word : BUILTINS)
            append(word, builtinDescription(word), QStringLiteral("Builtin"));
        for (const ScriptSymbol &symbol : knownScriptSymbols) {
            if (symbol.kind != ScriptSymbol::Kind::Function)
                continue;
            const bool ALREADY_SUGGESTED = std::any_of(currentSuggestions.cbegin(), currentSuggestions.cend(),
                [&symbol](const Suggestion &suggestion) { return suggestion.text == symbol.name; });
            if (!ALREADY_SUGGESTED)
                currentSuggestions.append(Suggestion{symbol.name,
                    QStringLiteral("Function defined in this script at line %1.").arg(symbol.declarationLine),
                    QStringLiteral("Script function"), QString(), {}, {}});
        }
        for (const QString &word : systemCommands)
            if (!BUILTINS.contains(word))
                append(word, commandDescription(word), QStringLiteral("Command"));
    } else {
        const QHash<QString, QStringList> ARGUMENTS = argumentCatalog();
        QStringList OPTIONS = ARGUMENTS.value(NORMALIZED_COMMAND);
        QString argumentCommand = NORMALIZED_COMMAND;
        if (NORMALIZED_COMMAND == QLatin1String("git")) {
            const QStringList priorWords = BEFORE_WORD.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            int gitIndex = -1;
            for (int index = 0; index < priorWords.size(); ++index) {
                if (priorWords.at(index) == QLatin1String("git")) {
                    gitIndex = index;
                    break;
                }
            }
            int subcommandIndex = gitIndex + 1;
            const QSet<QString> GIT_GLOBAL_OPTIONS_WITH_VALUE = {
                QStringLiteral("-C"), QStringLiteral("-c"), QStringLiteral("--git-dir"),
                QStringLiteral("--work-tree"), QStringLiteral("--namespace"),
                QStringLiteral("--config-env"), QStringLiteral("--exec-path"),
                QStringLiteral("--super-prefix"), QStringLiteral("--attr-source")
            };
            while (gitIndex >= 0 && subcommandIndex < priorWords.size()) {
                const QString word = priorWords.at(subcommandIndex);
                if (GIT_GLOBAL_OPTIONS_WITH_VALUE.contains(word)) {
                    subcommandIndex += 2;
                } else if (word.startsWith(QLatin1Char('-')) && word.contains(QLatin1Char('='))) {
                    ++subcommandIndex;
                } else if (word.startsWith(QLatin1Char('-'))) {
                    ++subcommandIndex;
                } else {
                    break;
                }
            }
            if (gitIndex >= 0 && subcommandIndex < priorWords.size()) {
                argumentCommand = priorWords.at(subcommandIndex);
                OPTIONS = ARGUMENTS.value(argumentCommand);
            }
        }
        for (const QString &option : OPTIONS)
            append(option, optionDescription(argumentCommand, option),
                   NORMALIZED_COMMAND == QLatin1String("git") && argumentCommand == QLatin1String("git")
                       ? QStringLiteral("Git subcommand") : QStringLiteral("Argument"));
        for (const Snippets::Snippet &snippet : snippets)
            currentSuggestions.append(Suggestion{snippet.name, snippet.description,
                                                 QStringLiteral("Snippet"), snippet.code, snippet.highlightPoints,
                                                 snippet.autoChangeSections});
        if (SOURCE_PATH_POSITION && !VARIABLE_POSITION) {
            QString pathPrefix = PREFIX;
            QString relativeDirectory;
            const qsizetype slash = pathPrefix.lastIndexOf(QLatin1Char('/'));
            if (slash >= 0) {
                relativeDirectory = pathPrefix.left(slash + 1);
                pathPrefix = pathPrefix.mid(slash + 1);
            }
            QString lookupDirectory;
            if (relativeDirectory.startsWith(QLatin1Char('/'))) {
                lookupDirectory = relativeDirectory;
            } else if (relativeDirectory.startsWith(QStringLiteral("~/"))) {
                lookupDirectory = QDir::home().filePath(relativeDirectory.mid(2));
            } else if (relativeDirectory == QStringLiteral("~")) {
                lookupDirectory = QDir::homePath();
            } else {
                lookupDirectory = QDir(workingDirectory).filePath(relativeDirectory);
            }
            const QFileInfo directoryInfo(lookupDirectory);
            if (directoryInfo.exists() && directoryInfo.isDir()) {
                const QFileInfoList entries = QDir(lookupDirectory).entryInfoList(
                    QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);
                for (const QFileInfo &entry : entries) {
                    const bool isDirectory = entry.isDir();
                    if (!isDirectory && !entry.fileName().endsWith(QStringLiteral(".sh"), Qt::CaseInsensitive))
                        continue;
                    if (!entry.fileName().startsWith(pathPrefix, Qt::CaseInsensitive))
                        continue;
                    const QString suggestion = relativeDirectory + entry.fileName() +
                        (isDirectory ? QStringLiteral("/") : QString());
                    append(suggestion, isDirectory ? QStringLiteral("Directory; continue the path.")
                                                   : QStringLiteral("Shell script file."),
                           isDirectory ? QStringLiteral("Directory") : QStringLiteral("Shell script"));
                }
            }
        }
    }

    struct RankedSuggestion { Suggestion suggestion; int tier; int score; };
    QVector<RankedSuggestion> rankedSuggestions;
    rankedSuggestions.reserve(currentSuggestions.size());
    for (const Suggestion &suggestion : currentSuggestions) {
        QString candidate = suggestion.text;
        if (suggestion.category == QLatin1String("Special parameter")) {
            candidate.remove(0, 1);
        } else if (VARIABLE_POSITION) {
            if (candidate.startsWith(QStringLiteral("${")) && candidate.endsWith(QLatin1Char('}')))
                candidate = candidate.mid(2, candidate.size() - 3);
            else if (candidate.startsWith(QLatin1Char('$')))
                candidate.remove(0, 1);
        }
        if (candidate.compare(PREFIX, Qt::CaseInsensitive) == 0) {
            rankedSuggestions.append({suggestion, 0, 0});
        } else if (candidate.startsWith(PREFIX, Qt::CaseInsensitive)) {
            // Keep all prefix matches ahead of fuzzy results, while putting
            // the closest completion first. This makes a short declared name
            // such as $myvar easy to reach even when many environment names
            // share the same prefix.
            rankedSuggestions.append({suggestion, 1,
                static_cast<int>(candidate.size() - PREFIX.size())});
        } else {
            const int SCORE = ShellScratchpad::Util::fuzzyMatchScore(PREFIX, candidate);
            if (SCORE >= 0)
                rankedSuggestions.append({suggestion, 2, SCORE});
        }
    }
    std::stable_sort(rankedSuggestions.begin(), rankedSuggestions.end(),
        [](const RankedSuggestion &left, const RankedSuggestion &right) {
            if (left.tier != right.tier)
                return left.tier < right.tier;
            return left.score < right.score;
        });
    currentSuggestions.clear();
    currentSuggestions.reserve(rankedSuggestions.size());
    for (const RankedSuggestion &ranked : rankedSuggestions)
        currentSuggestions.append(ranked.suggestion);

    popup->clear();
    const int DISPLAY_LIMIT = 80;
    for (int index = 0; index < qMin(DISPLAY_LIMIT, currentSuggestions.size()); ++index) {
        const Suggestion &suggestion = currentSuggestions.at(index);
        auto *item = new QListWidgetItem(QStringLiteral("%1    %2").arg(suggestion.text, suggestion.category), popup);
        item->setData(Qt::UserRole, index);
    }
    if ((PREFIX.isEmpty() && !VARIABLE_POSITION && !SOURCE_PATH_POSITION) || popup->count() == 0) {
        selectedDescriptionTimer.stop();
        popup->hide();
        QToolTip::hideText();
        return;
    }
    popup->setCurrentRow(0);
    popup->setFixedHeight(qMin(240, popup->sizeHintForRow(0) * qMin(popup->count(), 10) + 6));
    positionPopup();
    popup->show();
    popup->raise();
    selectedDescriptionTimer.start();
}

void CompletionController::positionPopup()
{
    const QRect CURSOR_RECT = editor->cursorRect();
    QPoint below = editor->viewport()->mapToGlobal(CURSOR_RECT.bottomLeft() + QPoint(0, 3));
    QScreen *screen = QGuiApplication::screenAt(below);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    constexpr int minimumWidth = 340;
    if (screen) {
        const QRect screenGeometry = screen->geometry();
        int availableWidth = qMax(1, screenGeometry.right() - below.x() + 1);
        if (availableWidth < minimumWidth && screenGeometry.width() >= minimumWidth) {
            below.setX(screenGeometry.right() - minimumWidth + 1);
            availableWidth = qMax(1, screenGeometry.right() - below.x() + 1);
        }
        const int desiredWidth = qMax(minimumWidth, popup->sizeHintForColumn(0) + 2 * popup->frameWidth() + 12);
        const int width = qMax(1, qMin(desiredWidth, availableWidth));
        popup->setMinimumWidth(qMin(minimumWidth, availableWidth));
        popup->setMaximumWidth(availableWidth);
        popup->resize(width, popup->height());
    } else {
        popup->setMinimumWidth(minimumWidth);
        popup->setMaximumWidth(QWIDGETSIZE_MAX);
        popup->resize(qMax(minimumWidth, popup->sizeHintForColumn(0) + 2 * popup->frameWidth() + 12),
                      popup->height());
    }
    popup->move(below);
}

void CompletionController::acceptCurrentSuggestion()
{
    if (popup->currentRow() < 0 || popup->currentRow() >= popup->count())
        return;
    const int SUGGESTION_INDEX = popup->currentItem()->data(Qt::UserRole).toInt();
    if (SUGGESTION_INDEX < 0 || SUGGESTION_INDEX >= currentSuggestions.size())
        return;
    const Suggestion SUGGESTION = currentSuggestions.at(SUGGESTION_INDEX);
    QTextCursor cursor(editor->document());
    cursor.setPosition(qMin<qsizetype>(replacementStart, editor->document()->characterCount() - 1));
    cursor.setPosition(qMin<qsizetype>(replacementEnd, editor->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    if (SUGGESTION.category == QLatin1String("Snippet")) {
        QVector<QPair<int, int>> points;
        points.reserve(SUGGESTION.highlightPoints.size());
        for (const Snippets::HighlightPoint &point : SUGGESTION.highlightPoints)
            points.append(qMakePair(static_cast<int>(point.offset), static_cast<int>(point.length)));
        if (auto *codeEditor = qobject_cast<ShellScratchpad::CodeEditor *>(editor)) {
            editor->setTextCursor(cursor);
            codeEditor->insertSnippet(SUGGESTION.insertText, points, SUGGESTION.autoChangeSections);
            popup->hide();
            QToolTip::hideText();
            editor->setFocus();
            return;
        }
    }
    cursor.insertText(SUGGESTION.insertText.isEmpty() ? SUGGESTION.text : SUGGESTION.insertText);
    editor->setTextCursor(cursor);
    popup->hide();
    QToolTip::hideText();
    editor->setFocus();
}

void CompletionController::showSelectedDescription()
{
    // setCurrentRow() fires before updateSuggestions() shows the popup. Do not
    // anchor a tooltip to that not-yet-visible list; the queued call after show()
    // will run once Qt has laid out and exposed the suggestion rows.
    if (!popup->isVisible()) {
        QToolTip::hideText();
        return;
    }
    if (popup->currentRow() < 0 || popup->currentRow() >= popup->count()) {
        QToolTip::hideText();
        return;
    }
    const int INDEX = popup->currentItem()->data(Qt::UserRole).toInt();
    if (INDEX < 0 || INDEX >= currentSuggestions.size()) {
        QToolTip::hideText();
        return;
    }
    const QString DESCRIPTION = currentSuggestions.at(INDEX).description;
    if (DESCRIPTION.isEmpty()) {
        QToolTip::hideText();
        return;
    }
    const QRect ROW_RECT = popup->visualItemRect(popup->currentItem());
    if (ROW_RECT.isEmpty())
        return;
    const QPoint GLOBAL_POSITION = popup->mapToGlobal(QPoint(popup->width() + 8, ROW_RECT.top()));
    QToolTip::showText(GLOBAL_POSITION, DESCRIPTION, popup, ROW_RECT);
}
}
