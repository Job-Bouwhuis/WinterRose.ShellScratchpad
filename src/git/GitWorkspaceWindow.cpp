#include "GitWorkspaceWindow.h"

#include "completion/CompletionController.h"
#include "editor/CodeEditor.h"
#include "output/TerminalOutput.h"
#include "syntax/SyntaxHighlighter.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QIcon>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPlainTextDocumentLayout>
#include <QProcess>
#include <QRegularExpression>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QSplitter>
#include <QSize>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QTextEdit>
#include <QStackedWidget>
#include <QScrollBar>
#include <memory>
#include <limits>

namespace ShellScratchpad::Git
{
namespace
{
enum class DiffLineKind { Unchanged, Added, Removed };
struct DiffLine
{
    QString left;
    QString right;
    DiffLineKind leftKind = DiffLineKind::Unchanged;
    DiffLineKind rightKind = DiffLineKind::Unchanged;
    int leftNumber = 0;
    int rightNumber = 0;
};

QStringList fileLines(const QString &text)
{
    if (text.isEmpty()) return {};
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (text.endsWith(QLatin1Char('\n'))) lines.removeLast();
    return lines;
}

QVector<DiffLine> makeSideBySideDiff(const QString &original, const QString &modified,
                                     int leftStartLine = 1, int rightStartLine = 1)
{
    const QStringList left = fileLines(original);
    const QStringList right = fileLines(modified);
    const qsizetype n = left.size();
    const qsizetype m = right.size();
    QVector<DiffLine> output;
    if (n * m > 4'000'000) {
        qsizetype i = 0;
        qsizetype j = 0;
        constexpr qsizetype LOOKAHEAD = 64;
        while (i < n || j < m) {
            if (i < n && j < m && left.at(i) == right.at(j)) {
                output.append({left.at(i), right.at(j), DiffLineKind::Unchanged, DiffLineKind::Unchanged,
                    leftStartLine + static_cast<int>(i), rightStartLine + static_cast<int>(j)});
                ++i;
                ++j;
                continue;
            }
            if (i == n) {
                output.append({QString(), right.at(j), DiffLineKind::Unchanged, DiffLineKind::Added,
                    0, rightStartLine + static_cast<int>(j++)});
                continue;
            }
            if (j == m) {
                output.append({left.at(i), QString(), DiffLineKind::Removed, DiffLineKind::Unchanged,
                    leftStartLine + static_cast<int>(i++), 0});
                continue;
            }
            qsizetype matchI = -1;
            qsizetype matchJ = -1;
            qsizetype bestDistance = std::numeric_limits<qsizetype>::max();
            const qsizetype iEnd = qMin(n, i + LOOKAHEAD);
            const qsizetype jEnd = qMin(m, j + LOOKAHEAD);
            for (qsizetype candidateI = i; candidateI < iEnd; ++candidateI) {
                for (qsizetype candidateJ = j; candidateJ < jEnd; ++candidateJ) {
                    if (left.at(candidateI) != right.at(candidateJ)) continue;
                    const qsizetype distance = (candidateI - i) + (candidateJ - j);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        matchI = candidateI;
                        matchJ = candidateJ;
                    }
                }
            }
            if (matchI < 0) {
                output.append({left.at(i), QString(), DiffLineKind::Removed, DiffLineKind::Unchanged,
                    leftStartLine + static_cast<int>(i++), 0});
                output.append({QString(), right.at(j), DiffLineKind::Unchanged, DiffLineKind::Added,
                    0, rightStartLine + static_cast<int>(j++)});
                continue;
            }
            while (i < matchI) {
                output.append({left.at(i), QString(), DiffLineKind::Removed, DiffLineKind::Unchanged,
                    leftStartLine + static_cast<int>(i++), 0});
            }
            while (j < matchJ) {
                output.append({QString(), right.at(j), DiffLineKind::Unchanged, DiffLineKind::Added,
                    0, rightStartLine + static_cast<int>(j++)});
            }
        }
        return output;
    }
    QVector<QVector<int>> lcs(static_cast<int>(n + 1), QVector<int>(static_cast<int>(m + 1), 0));
    for (qsizetype i = n - 1; i >= 0; --i)
        for (qsizetype j = m - 1; j >= 0; --j)
            lcs[static_cast<int>(i)][static_cast<int>(j)] = left.at(i) == right.at(j)
                ? 1 + lcs[static_cast<int>(i + 1)][static_cast<int>(j + 1)]
                : qMax(lcs[static_cast<int>(i + 1)][static_cast<int>(j)], lcs[static_cast<int>(i)][static_cast<int>(j + 1)]);

    struct Op { DiffLineKind kind; QString text; int leftNumber; int rightNumber; };
    QVector<Op> ops;
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && left.at(i) == right.at(j)) {
            ops.append({DiffLineKind::Unchanged, left.at(i), leftStartLine + static_cast<int>(i),
                        rightStartLine + static_cast<int>(j)});
            ++i;
            ++j;
        } else if (i < n && (j == m || lcs[static_cast<int>(i + 1)][static_cast<int>(j)] >= lcs[static_cast<int>(i)][static_cast<int>(j + 1)])) {
            ops.append({DiffLineKind::Removed, left.at(i), leftStartLine + static_cast<int>(i), 0});
            ++i;
        } else {
            ops.append({DiffLineKind::Added, right.at(j), 0, rightStartLine + static_cast<int>(j)});
            ++j;
        }
    }
    qsizetype op = 0;
    while (op < ops.size()) {
        if (ops.at(op).kind == DiffLineKind::Unchanged) {
            output.append({ops.at(op).text, ops.at(op).text, DiffLineKind::Unchanged, DiffLineKind::Unchanged,
                           ops.at(op).leftNumber, ops.at(op).rightNumber});
            ++op;
            continue;
        }
        QStringList removed;
        QStringList added;
        QVector<int> removedNumbers;
        QVector<int> addedNumbers;
        while (op < ops.size() && ops.at(op).kind != DiffLineKind::Unchanged) {
            if (ops.at(op).kind == DiffLineKind::Removed) {
                removed.append(ops.at(op).text);
                removedNumbers.append(ops.at(op).leftNumber);
            } else {
                added.append(ops.at(op).text);
                addedNumbers.append(ops.at(op).rightNumber);
            }
            ++op;
        }
        const qsizetype count = qMax(removed.size(), added.size());
        for (qsizetype row = 0; row < count; ++row) {
            const bool hasLeft = row < removed.size();
            const bool hasRight = row < added.size();
            output.append({hasLeft ? removed.at(row) : QString(), hasRight ? added.at(row) : QString(),
                hasLeft ? DiffLineKind::Removed : DiffLineKind::Unchanged,
                hasRight ? DiffLineKind::Added : DiffLineKind::Unchanged,
                hasLeft ? removedNumbers.at(row) : 0,
                hasRight ? addedNumbers.at(row) : 0});
        }
    }
    return output;
}

void renderDiffLines(QTextEdit *view, const QStringList &lines, const QVector<DiffLineKind> &kinds,
                     const QVector<int> &lineNumbers)
{
    QString html = QStringLiteral("<html><body style='margin:4px;background:#202124;color:#ffffff;font-family:monospace;font-size:10pt;'><pre style='margin:0;white-space:pre;'>");
    for (int i = 0; i < lines.size(); ++i) {
        const DiffLineKind kind = i < kinds.size() ? kinds.at(i) : DiffLineKind::Unchanged;
        QString style = QStringLiteral("color:#ffffff;background-color:#202124;");
        if (kind == DiffLineKind::Added) style = QStringLiteral("color:#c8f7d0;background-color:#17452a;");
        else if (kind == DiffLineKind::Removed) style = QStringLiteral("color:#ffd0d0;background-color:#5a252b;");
        const QString number = i < lineNumbers.size() && lineNumbers.at(i) > 0
            ? QString::number(lineNumbers.at(i)).rightJustified(5, QLatin1Char(' '))
            : QStringLiteral("     ");
        const QString content = (lines.at(i).isEmpty() ? QStringLiteral("&nbsp;") : lines.at(i).toHtmlEscaped());
        html += QStringLiteral("<span style='%1'>%2 │ %3</span>\n").arg(style, number.toHtmlEscaped(), content);
    }
    html += QStringLiteral("</pre></body></html>");
    view->setHtml(html);
}

void renderDiffSide(QTextEdit *view, const QVector<DiffLine> &rows, bool leftSide)
{
    QStringList lines;
    QVector<DiffLineKind> kinds;
    QVector<int> lineNumbers;
    lines.reserve(rows.size());
    kinds.reserve(rows.size());
    lineNumbers.reserve(rows.size());
    for (const DiffLine &row : rows) {
        lines.append(leftSide ? row.left : row.right);
        kinds.append(leftSide ? row.leftKind : row.rightKind);
        lineNumbers.append(leftSide ? row.leftNumber : row.rightNumber);
    }
    renderDiffLines(view, lines, kinds, lineNumbers);
}

void synchronizeScrollbars(QTextEdit *first, QTextEdit *second, Qt::Orientation orientation)
{
    QScrollBar *firstBar = orientation == Qt::Vertical ? first->verticalScrollBar() : first->horizontalScrollBar();
    QScrollBar *secondBar = orientation == Qt::Vertical ? second->verticalScrollBar() : second->horizontalScrollBar();
    auto syncing = std::make_shared<bool>(false);
    const auto synchronize = [syncing](QScrollBar *source, QScrollBar *target, int value) {
        if (*syncing) return;
        *syncing = true;
        const int sourceRange = source->maximum() - source->minimum();
        const int targetRange = target->maximum() - target->minimum();
        const qreal ratio = sourceRange > 0 ? qreal(value - source->minimum()) / sourceRange : 0.0;
        target->setValue(target->minimum() + qRound(ratio * targetRange));
        *syncing = false;
    };
    QObject::connect(firstBar, &QScrollBar::valueChanged, firstBar,
        [synchronize, firstBar, secondBar](int value) { synchronize(firstBar, secondBar, value); });
    QObject::connect(secondBar, &QScrollBar::valueChanged, secondBar,
        [synchronize, firstBar, secondBar](int value) { synchronize(secondBar, firstBar, value); });
    QObject::connect(firstBar, &QScrollBar::rangeChanged, firstBar,
        [synchronize, firstBar, secondBar](int, int) { synchronize(firstBar, secondBar, firstBar->value()); });
    QObject::connect(secondBar, &QScrollBar::rangeChanged, secondBar,
        [synchronize, firstBar, secondBar](int, int) { synchronize(secondBar, firstBar, secondBar->value()); });
}

void synchronizeDiffViews(QTextEdit *first, QTextEdit *second)
{
    synchronizeScrollbars(first, second, Qt::Vertical);
    synchronizeScrollbars(first, second, Qt::Horizontal);
}

class BranchMenu final : public QMenu
{
public:
    using ContextHandler = std::function<void(const QString &, bool, const QPoint &)>;
    BranchMenu(const QString &title, QWidget *parent, ContextHandler handler = {})
        : QMenu(title, parent), contextHandler(std::move(handler)) {}
protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QAction *action = actionAt(event->pos());
        if (action && action->data().isValid() && contextHandler) {
            contextHandler(action->data().toString(), action->property("branchRemote").toBool(), event->globalPos());
            event->accept();
            return;
        }
        QMenu::contextMenuEvent(event);
    }
private:
    ContextHandler contextHandler;
};

void addBranchPath(QMenu *root, const QString &path, const QString &ref, bool remote,
                   const BranchMenu::ContextHandler &contextHandler)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;
    QMenu *menu = root;
    QString prefix;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        if (!prefix.isEmpty()) prefix += QLatin1Char('/');
        prefix += parts.at(i);
        QMenu *next = nullptr;
        for (QAction *action : menu->actions()) {
            if (action->menu() && action->text() == parts.at(i)) {
                next = action->menu();
                break;
            }
        }
        if (!next) {
            auto *nested = new BranchMenu(parts.at(i), menu, contextHandler);
            menu->addMenu(nested);
            next = nested;
        }
        menu = next;
    }
    QAction *leaf = menu->addAction(parts.last());
    leaf->setData(ref);
    leaf->setProperty("branchRemote", remote);
}

class ConflictResolverDialog final : public QDialog
{
public:
    struct Hunk { qsizetype start = 0; qsizetype end = 0; int sourceLine = 1; QString incoming; QString ours; int choice = -1; };

    explicit ConflictResolverDialog(const QString &filePath, QWidget *parent)
        : QDialog(parent), path(filePath)
    {
        setWindowTitle(QStringLiteral("Resolve Merge Conflict — %1").arg(QFileInfo(path).fileName()));
        resize(1050, 650);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            error = file.errorString();
            return;
        }
        original = QString::fromUtf8(file.readAll());
        parseConflicts();

        auto *layout = new QVBoxLayout(this);
        auto *navigation = new QHBoxLayout;
        navigation->addWidget(new QLabel(QStringLiteral("Conflict:"), this));
        conflictSelector = new QComboBox(this);
        for (int i = 0; i < hunks.size(); ++i)
            conflictSelector->addItem(QStringLiteral("%1 of %2").arg(i + 1).arg(hunks.size()));
        navigation->addWidget(conflictSelector);
        navigation->addStretch(1);
        layout->addLayout(navigation);
        auto *sideLabels = new QHBoxLayout;
        sideLabels->addWidget(new QLabel(QStringLiteral("Incoming change"), this));
        sideLabels->addWidget(new QLabel(QStringLiteral("Your change"), this));
        layout->addLayout(sideLabels);
        auto *columns = new QSplitter(Qt::Horizontal, this);
        incomingView = makeTextView(columns);
        oursView = makeTextView(columns);
        columns->addWidget(incomingView);
        columns->addWidget(oursView);
        synchronizeDiffViews(incomingView, oursView);
        layout->addWidget(columns, 1);
        auto *choices = new QHBoxLayout;
        auto *incomingButton = new QPushButton(QStringLiteral("Use Incoming"), this);
        auto *oursButton = new QPushButton(QStringLiteral("Keep Mine"), this);
        auto *bothButton = new QPushButton(QStringLiteral("Keep Both (Incoming, then Mine)"), this);
        choices->addWidget(incomingButton);
        choices->addWidget(oursButton);
        choices->addWidget(bothButton);
        choices->addStretch(1);
        saveButton = new QPushButton(QStringLiteral("Save & Mark Resolved"), this);
        saveButton->setEnabled(false);
        choices->addWidget(saveButton);
        layout->addLayout(choices);
        connect(conflictSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) { showHunk(index); });
        connect(incomingButton, &QPushButton::clicked, this, [this] { choose(0); });
        connect(oursButton, &QPushButton::clicked, this, [this] { choose(1); });
        connect(bothButton, &QPushButton::clicked, this, [this] { choose(2); });
        connect(saveButton, &QPushButton::clicked, this, [this] {
            if (!saveResolution()) {
                QMessageBox::warning(this, QStringLiteral("Could not save resolution"), error);
                return;
            }
            accept();
        });
        if (hunks.isEmpty()) {
            error = QStringLiteral("No standard Git conflict markers were found in this file.");
            saveButton->setEnabled(false);
        } else {
            showHunk(0);
        }
    }

    QString errorMessage() const { return error; }
    bool saveResolution()
    {
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly | QIODevice::Text) || QString::fromUtf8(current.readAll()) != original) {
            error = QStringLiteral("The file changed while the resolver was open. Reopen it and apply your choices again.");
            return false;
        }
        QString result;
        qsizetype cursor = 0;
        for (const Hunk &hunk : hunks) {
            result += original.mid(cursor, hunk.start - cursor);
            if (hunk.choice == 0) result += hunk.incoming;
            else if (hunk.choice == 1) result += hunk.ours;
            else result += hunk.incoming + hunk.ours;
            cursor = hunk.end;
        }
        result += original.mid(cursor);
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
            error = output.errorString();
            return false;
        }
        if (output.write(result.toUtf8()) < 0 || !output.commit()) {
            error = output.errorString();
            return false;
        }
        return true;
    }

private:
    QString path;
    QString original;
    QString error;
    QVector<Hunk> hunks;
    QComboBox *conflictSelector = nullptr;
    QTextEdit *incomingView = nullptr;
    QTextEdit *oursView = nullptr;
    QPushButton *saveButton = nullptr;

    static QTextEdit *makeTextView(QWidget *parent)
    {
        auto *view = new QTextEdit(parent);
        view->setReadOnly(true);
        view->setLineWrapMode(QTextEdit::NoWrap);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        return view;
    }

    void parseConflicts()
    {
        const QRegularExpression expression(
            QStringLiteral("(?m)^<<<<<<<[^\\n]*\\n(.*?)(?:^\\|\\|\\|\\|\\|\\|\\|[^\\n]*\\n.*?^=======\\s*$|^=======\\s*$)(.*?)^>>>>>>>[^\\n]*(?:\\n|$)"),
            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
        auto match = expression.globalMatch(original);
        while (match.hasNext()) {
            const auto item = match.next();
            Hunk hunk;
            hunk.start = item.capturedStart(0);
            hunk.end = item.capturedEnd(0);
            hunk.sourceLine = original.left(hunk.start).count(QLatin1Char('\n')) + 2;
            hunk.ours = item.captured(1);
            hunk.incoming = item.captured(2);
            hunks.append(hunk);
        }
    }

    void showHunk(int index)
    {
        if (index < 0 || index >= hunks.size()) return;
        setWindowTitle(QStringLiteral("Resolve Merge Conflict — %1 (%2/%3)")
            .arg(QFileInfo(path).fileName()).arg(index + 1).arg(hunks.size()));
        const QVector<DiffLine> rows = makeSideBySideDiff(hunks.at(index).ours, hunks.at(index).incoming,
            hunks.at(index).sourceLine, hunks.at(index).sourceLine);
        renderDiffSide(incomingView, rows, false);
        renderDiffSide(oursView, rows, true);
    }

    void choose(int choice)
    {
        const int index = conflictSelector->currentIndex();
        if (index < 0 || index >= hunks.size()) return;
        hunks[index].choice = choice;
        int nextUnresolved = -1;
        for (int i = 0; i < hunks.size(); ++i) {
            if (hunks.at(i).choice < 0) { nextUnresolved = i; break; }
        }
        saveButton->setEnabled(nextUnresolved < 0);
        if (nextUnresolved >= 0) conflictSelector->setCurrentIndex(nextUnresolved);
    }
};

QString gitExecutable()
{
    return QStringLiteral("git");
}

QIcon gitIcon(const QString &name)
{
    return QIcon(QStringLiteral(":/git/icons/%1.svg").arg(name));
}

class GitCloneDialog final : public QDialog
{
public:
    explicit GitCloneDialog(const QString &directory, QWidget *parent)
        : QDialog(parent), parentDirectory(directory)
    {
        setWindowTitle(QStringLiteral("Clone Git Repository"));
        setMinimumWidth(520);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        repositoryUrl = new QLineEdit(this);
        repositoryUrl->setPlaceholderText(QStringLiteral("https://host/user/repository.git or git@host:user/repository.git"));
        destinationName = new QLineEdit(this);
        form->addRow(QStringLiteral("Repository URL"), repositoryUrl);
        form->addRow(QStringLiteral("New folder name"), destinationName);
        auto *folderLabel = new QLabel(parentDirectory, this);
        folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(QStringLiteral("Clone into"), folderLabel);
        layout->addLayout(form);
        output = new QPlainTextEdit(this);
        output->setReadOnly(true);
        output->setMaximumBlockCount(1000);
        output->setPlaceholderText(QStringLiteral("Clone progress and errors will appear here."));
        layout->addWidget(output, 1);
        auto *buttons = new QDialogButtonBox(this);
        cloneButton = buttons->addButton(QStringLiteral("Clone Repository"), QDialogButtonBox::AcceptRole);
        auto *cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
        layout->addWidget(buttons);

        process = new QProcess(this);
        process->setWorkingDirectory(parentDirectory);
        connect(repositoryUrl, &QLineEdit::textChanged, this, [this](const QString &url) {
            if (nameEdited) return;
            QString name = url.trimmed();
            while (name.endsWith(QLatin1Char('/'))) name.chop(1);
            name = name.section(QLatin1Char('/'), -1);
            name = name.section(QLatin1Char(':'), -1);
            if (name.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive)) name.chop(4);
            destinationName->setText(name);
        });
        connect(destinationName, &QLineEdit::textEdited, this, [this] { nameEdited = true; });
        connect(cloneButton, &QPushButton::clicked, this, [this] { startClone(); });
        connect(cancelButton, &QPushButton::clicked, this, [this] {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
            reject();
        });
        connect(process, &QProcess::readyReadStandardOutput, this, [this] {
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString::fromLocal8Bit(process->readAllStandardOutput()));
            output->ensureCursorVisible();
        });
        connect(process, &QProcess::readyReadStandardError, this, [this] {
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(QString::fromLocal8Bit(process->readAllStandardError()));
            output->ensureCursorVisible();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
            if (fetchingRefs) {
                fetchingRefs = false;
                if (exitStatus != QProcess::NormalExit || exitCode != 0)
                    output->appendPlainText(QStringLiteral("\nClone succeeded, but refreshing remote branches failed (exit code %1). You can fetch later from the Git workspace.").arg(exitCode));
                else
                    output->appendPlainText(QStringLiteral("\nRemote branches refreshed."));
                succeeded = true;
                cloneButton->setEnabled(true);
                accept();
                return;
            }
            cloneButton->setEnabled(true);
            repositoryUrl->setEnabled(true);
            destinationName->setEnabled(true);
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                clonedPath = QDir(parentDirectory).filePath(destinationName->text().trimmed());
                output->appendPlainText(QStringLiteral("\nRefreshing remote branches…"));
                fetchingRefs = true;
                process->setWorkingDirectory(clonedPath);
                process->setProgram(QStringLiteral("git"));
                process->setArguments({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
                process->start();
            } else {
                output->appendPlainText(QStringLiteral("\nClone failed with exit code %1.").arg(exitCode));
            }
        });
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            if (fetchingRefs) {
                fetchingRefs = false;
                output->appendPlainText(QStringLiteral("Remote branch refresh could not start: %1").arg(process->errorString()));
                succeeded = true;
                accept();
                return;
            }
            cloneButton->setEnabled(true);
            repositoryUrl->setEnabled(true);
            destinationName->setEnabled(true);
            output->appendPlainText(QStringLiteral("Could not start Git: %1").arg(process->errorString()));
        });
    }

    QString clonedRepositoryPath() const { return clonedPath; }
    bool cloneSucceeded() const { return succeeded; }

private:
    QString parentDirectory;
    QString clonedPath;
    QLineEdit *repositoryUrl = nullptr;
    QLineEdit *destinationName = nullptr;
    QPlainTextEdit *output = nullptr;
    QPushButton *cloneButton = nullptr;
    QProcess *process = nullptr;
    bool nameEdited = false;
    bool succeeded = false;
    bool fetchingRefs = false;

    void startClone()
    {
        const QString url = repositoryUrl->text().trimmed();
        const QString name = destinationName->text().trimmed();
        if (url.isEmpty() || name.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Repository details required"),
                                 QStringLiteral("Enter a repository URL and destination folder name."));
            return;
        }
        if (name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/')) ||
            name.contains(QLatin1Char('\\')) || name.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")))) {
            QMessageBox::warning(this, QStringLiteral("Invalid folder name"),
                                 QStringLiteral("Use a folder name without path separators."));
            return;
        }
        const QString destination = QDir(parentDirectory).filePath(name);
        if (QFileInfo::exists(destination)) {
            QMessageBox::warning(this, QStringLiteral("Destination already exists"),
                                 QStringLiteral("Choose a new folder name. The destination already exists."));
            return;
        }
        cloneButton->setEnabled(false);
        repositoryUrl->setEnabled(false);
        destinationName->setEnabled(false);
        output->appendPlainText(QStringLiteral("Cloning into %1…\n").arg(destination));
        process->setProgram(QStringLiteral("git"));
        process->setArguments({QStringLiteral("clone"), url, destination});
        process->start();
    }
};

}

bool GitWorkspaceWindow::isGitRepository(const QString &path)
{
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("-C"), QDir(path).absolutePath(), QStringLiteral("rev-parse"),
                          QStringLiteral("--is-inside-work-tree")});
    process.start();
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    return process.exitCode() == 0 && process.readAllStandardOutput().trimmed() == QByteArray("true");
}

bool GitWorkspaceWindow::showCloneDialog(const QString &directory, QWidget *parent)
{
    const QString parentDirectory = QDir(directory).absolutePath();
    if (!QDir(parentDirectory).exists()) {
        QMessageBox::warning(parent, QStringLiteral("Folder unavailable"),
                             QStringLiteral("The selected destination folder does not exist."));
        return false;
    }
    GitCloneDialog dialog(parentDirectory, parent);
    if (dialog.exec() == QDialog::Accepted && dialog.cloneSucceeded()) {
        auto *window = new GitWorkspaceWindow(dialog.clonedRepositoryPath());
        window->show();
        window->raise();
        window->activateWindow();
        return true;
    }
    return false;
}

GitWorkspaceWindow::GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent)
    : QMainWindow(parent), repoPath(QDir(repositoryPath).absolutePath())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Git Workspace — %1").arg(QDir(repoPath).dirName()));
    resize(1100, 760);

    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(4, 2, 4, 2);
    toolbar->setSpacing(4);
    branchButton = new QToolButton(root);
    branchButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    branchButton->setIcon(gitIcon(QStringLiteral("branch")));
    branchButton->setText(QStringLiteral("Branch"));
    branchButton->setPopupMode(QToolButton::InstantPopup);
    branchButton->setToolTip(QStringLiteral("Switch branches or create a branch"));
    const BranchMenu::ContextHandler branchContext = [this](const QString &branch, bool remote, const QPoint &position) {
        showBranchContextMenu(branch, remote, position);
    };
    auto *branchMenu = new BranchMenu(QString(), branchButton, branchContext);
    branchButton->setMenu(branchMenu);
    connect(branchMenu, &QMenu::aboutToShow, this, [this, branchMenu] {
        branchMenu->clear();
        auto *localMenu = new BranchMenu(QStringLiteral("Local"), branchMenu,
            [this](const QString &branch, bool remote, const QPoint &position) { showBranchContextMenu(branch, remote, position); });
        auto *remoteMenu = new BranchMenu(QStringLiteral("Remote"), branchMenu,
            [this](const QString &branch, bool remote, const QPoint &position) { showBranchContextMenu(branch, remote, position); });
        bool haveLocal = false;
        bool haveRemote = false;
        for (int index = 0; index < branchesList->count(); ++index) {
            QListWidgetItem *item = branchesList->item(index);
            const QString name = item->data(Qt::UserRole).toString();
            const bool remote = item->data(Qt::UserRole + 1).toBool();
            const bool current = item->data(Qt::UserRole + 2).toBool();
            if (name.isEmpty()) continue;
            (remote ? haveRemote : haveLocal) = true;
            addBranchPath(remote ? static_cast<QMenu *>(remoteMenu) : static_cast<QMenu *>(localMenu),
                          name, name, remote,
                          [this](const QString &branch, bool isRemote, const QPoint &position) {
                              showBranchContextMenu(branch, isRemote, position);
                          });
            // Branch actions are attached to leaves by addBranchPath; the menu trigger is handled recursively below.
            const auto configureLeaves = [this, remote, name, current](QMenu *menu, auto &&self) -> void {
                for (QAction *action : menu->actions()) {
                    if (action->menu()) { self(action->menu(), self); continue; }
                    if (action->data().toString() != name) continue;
                    action->setCheckable(!remote);
                    action->setChecked(current);
                    connect(action, &QAction::triggered, this, [this, name, remote] { switchToBranch(name, remote); });
                }
            };
            configureLeaves(remote ? static_cast<QMenu *>(remoteMenu) : static_cast<QMenu *>(localMenu), configureLeaves);
        }
        if (haveLocal) branchMenu->addMenu(localMenu); else delete localMenu;
        if (haveRemote) branchMenu->addMenu(remoteMenu); else delete remoteMenu;
        if (!branchMenu->actions().isEmpty()) branchMenu->addSeparator();
        QAction *createAction = branchMenu->addAction(QStringLiteral("Create New Branch…"));
        connect(createAction, &QAction::triggered, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("New branch"),
                QStringLiteral("Branch name:"), QLineEdit::Normal, QString(), &ok).trimmed();
            if (ok && !name.isEmpty()) runGit({QStringLiteral("switch"), QStringLiteral("-c"), name});
        });
    });
    commitButton = new QToolButton(root);
    commitButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    commitButton->setIcon(gitIcon(QStringLiteral("commit")));
    commitButton->setText(QStringLiteral("Commit…"));
    commitButton->setToolTip(QStringLiteral("Commit staged changes"));
    connect(commitButton, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString message = QInputDialog::getText(this, QStringLiteral("Commit"),
            QStringLiteral("Commit message:"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !message.isEmpty()) runGit({QStringLiteral("commit"), QStringLiteral("-m"), message});
    });
    stageAllButton = new QToolButton(root);
    stageAllButton->setText(QStringLiteral("Stage All"));
    stageAllButton->setToolTip(QStringLiteral("Stage all modified, deleted, and untracked files"));
    stageAllButton->setVisible(false);
    connect(stageAllButton, &QToolButton::clicked, this, [this] {
        runGit({QStringLiteral("add"), QStringLiteral("-A")});
    });
    syncButton = new QToolButton(root);
    syncButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    syncButton->setIcon(gitIcon(QStringLiteral("sync")));
    syncButton->setText(QStringLiteral("Sync"));
    syncButton->setPopupMode(QToolButton::InstantPopup);
    syncButton->setToolTip(QStringLiteral("Fetch, pull, or push repository changes"));
    auto *syncMenu = new QMenu(syncButton);
    syncButton->setMenu(syncMenu);
    connect(syncMenu, &QMenu::aboutToShow, this, [this, syncMenu] {
        syncMenu->clear();
        if (hasRemote) {
            QAction *fetchAction = syncMenu->addAction(QStringLiteral("Fetch all remotes"));
            connect(fetchAction, &QAction::triggered, this, [this] {
                runGit({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
            });
        } else {
            QAction *noRemote = syncMenu->addAction(QStringLiteral("No remote configured"));
            noRemote->setEnabled(false);
        }
        if (hasRemote) {
            syncMenu->addSeparator();
            const QString upstream = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("@{upstream}")});
            QAction *pullAction = syncMenu->addAction(QStringLiteral("Pull with Rebase"));
            pullAction->setEnabled(!upstream.isEmpty() && !upstream.startsWith(QStringLiteral("fatal:")));
            connect(pullAction, &QAction::triggered, this, [this] {
                runGit({QStringLiteral("pull"), QStringLiteral("--rebase")});
            });
            QAction *pushAction = syncMenu->addAction(QStringLiteral("Push"));
            pushAction->setEnabled(!upstream.isEmpty() && !upstream.startsWith(QStringLiteral("fatal:")));
            connect(pushAction, &QAction::triggered, this, [this] { runGit({QStringLiteral("push")}); });
        }
    });
    pullButton = new QToolButton(root);
    pullButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pullButton->setIcon(gitIcon(QStringLiteral("pull")));
    pullButton->setText(QStringLiteral("Pull"));
    pullButton->setToolTip(QStringLiteral("Pull changes from a remote branch"));
    connect(pullButton, &QToolButton::clicked, this, [this] { pullCurrentBranch(); });
    pushButton = new QToolButton(root);
    pushButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pushButton->setIcon(gitIcon(QStringLiteral("push")));
    pushButton->setText(QStringLiteral("Push"));
    pushButton->setToolTip(QStringLiteral("Push local commits to a remote branch"));
    connect(pushButton, &QToolButton::clicked, this, [this] { pushCurrentBranch(); });
    moreButton = new QToolButton(root);
    moreButton->setIcon(gitIcon(QStringLiteral("more")));
    moreButton->setToolTip(QStringLiteral("More repository actions"));
    moreButton->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(moreButton);
    moreButton->setMenu(moreMenu);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        if (hasRemote && !hasUpstream) {
            QAction *fetch = moreMenu->addAction(QStringLiteral("Fetch All Remotes"));
            connect(fetch, &QAction::triggered, this, [this] {
                runGit({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
            });
            moreMenu->addSeparator();
        }
        if (!hasStagedChanges) {
            QAction *commitUnavailable = moreMenu->addAction(QStringLiteral("Commit Staged Changes…"));
            commitUnavailable->setEnabled(false);
            commitUnavailable->setToolTip(QStringLiteral("Stage at least one file before committing."));
        }
        QAction *unstageAll = moreMenu->addAction(QStringLiteral("Unstage All Changes"));
        unstageAll->setEnabled(hasStagedChanges);
        connect(unstageAll, &QAction::triggered, this, [this] {
            runGit({QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral(":/")});
        });
        moreMenu->addSeparator();
        QAction *stash = moreMenu->addAction(QStringLiteral("Stash Changes"));
        stash->setEnabled(hasStagedChanges || hasUnstagedChanges);
        connect(stash, &QAction::triggered, this, [this] {
            runGit({QStringLiteral("stash"), QStringLiteral("push")});
        });
        QAction *popStash = moreMenu->addAction(QStringLiteral("Apply Latest Stash"));
        popStash->setEnabled(hasStashes);
        connect(popStash, &QAction::triggered, this, [this] { runGit({QStringLiteral("stash"), QStringLiteral("pop")}); });
    });
    conflictsButton = new QToolButton(root);
    conflictsButton->setText(QStringLiteral("Resolve Conflicts"));
    conflictsButton->setToolTip(QStringLiteral("Open the side-by-side resolver for an unmerged file"));
    conflictsButton->setEnabled(false);
    connect(conflictsButton, &QToolButton::clicked, this, [this] {
        if (conflictedFiles.size() == 1) {
            resolveConflictFile(conflictedFiles.first());
            return;
        }
        QMenu menu(conflictsButton);
        for (const QString &path : conflictedFiles) {
            QAction *action = menu.addAction(path);
            connect(action, &QAction::triggered, this, [this, path] { resolveConflictFile(path); });
        }
        menu.exec(conflictsButton->mapToGlobal(QPoint(0, conflictsButton->height())));
    });
    auto *refreshButton = new QToolButton(root);
    refreshButton->setIcon(gitIcon(QStringLiteral("refresh")));
    refreshButton->setToolTip(QStringLiteral("Refresh repository status"));
    connect(refreshButton, &QToolButton::clicked, this, [this] {
        if (hasRemote) runGit({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
        else refreshAll();
    });
    toolbar->addWidget(branchButton);
    toolbar->addWidget(commitButton);
    toolbar->addWidget(stageAllButton);
    toolbar->addWidget(syncButton);
    toolbar->addWidget(pullButton);
    toolbar->addWidget(pushButton);
    toolbar->addWidget(moreButton);
    toolbar->addWidget(conflictsButton);
    toolbar->addWidget(refreshButton);
    toolbar->addStretch(1);
    toolbar->addWidget(new QLabel(QDir(repoPath).dirName(), root));
    layout->addLayout(toolbar);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, root);
    auto *leftTabs = new QTabWidget(mainSplitter);
    changesTree = new QTreeWidget(leftTabs);
    changesTree->setHeaderLabels({QStringLiteral("Working tree changes"), QStringLiteral("Status")});
    changesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    changesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    leftTabs->addTab(changesTree, QStringLiteral("Changes"));
    branchesList = new QListWidget(leftTabs);
    leftTabs->addTab(branchesList, QStringLiteral("Branches"));
    historyList = new QListWidget(leftTabs);
    leftTabs->addTab(historyList, QStringLiteral("History"));

    rightTabs = new QTabWidget(mainSplitter);
    detailsStack = new QStackedWidget(rightTabs);
    detailsView = new QPlainTextEdit(detailsStack);
    detailsView->setReadOnly(true);
    detailsView->setPlaceholderText(QStringLiteral("Select a changed file, branch, or commit to inspect it."));
    detailsStack->addWidget(detailsView);
    auto *diffPanel = new QWidget(detailsStack);
    auto *diffLayout = new QVBoxLayout(diffPanel);
    diffLayout->setContentsMargins(4, 4, 4, 4);
    diffFilesList = new QListWidget(diffPanel);
    diffFilesList->setMinimumWidth(130);
    auto *diffContent = new QWidget(diffPanel);
    auto *diffContentLayout = new QVBoxLayout(diffContent);
    diffContentLayout->setContentsMargins(0, 0, 0, 0);
    diffSummaryLabel = new QLabel(diffContent);
    diffContentLayout->addWidget(diffSummaryLabel);
    auto *diffLabels = new QHBoxLayout;
    originalDiffLabel = new QLabel(QStringLiteral("Original"), diffContent);
    changedDiffLabel = new QLabel(QStringLiteral("Changed"), diffContent);
    diffLabels->addWidget(originalDiffLabel);
    diffLabels->addWidget(changedDiffLabel);
    diffContentLayout->addLayout(diffLabels);
    auto *diffColumns = new QSplitter(Qt::Horizontal, diffContent);
    originalFileView = new QTextEdit(diffColumns);
    workingFileView = new QTextEdit(diffColumns);
    for (QTextEdit *view : {originalFileView, workingFileView}) {
        view->setReadOnly(true);
        view->setLineWrapMode(QTextEdit::NoWrap);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    diffColumns->addWidget(originalFileView);
    diffColumns->addWidget(workingFileView);
    synchronizeDiffViews(originalFileView, workingFileView);
    diffContentLayout->addWidget(diffColumns, 1);
    auto *diffOuterSplitter = new QSplitter(Qt::Horizontal, diffPanel);
    diffOuterSplitter->addWidget(diffFilesList);
    diffOuterSplitter->addWidget(diffContent);
    diffOuterSplitter->setStretchFactor(0, 0);
    diffOuterSplitter->setStretchFactor(1, 1);
    diffLayout->addWidget(diffOuterSplitter, 1);
    connect(diffFilesList, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem *item) {
            if (!item || currentDiffCommitHash.isEmpty()) return;
            showCommitFile(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
        });
    detailsStack->addWidget(diffPanel);
    rightTabs->addTab(detailsStack, QStringLiteral("Details / Diff"));

    auto *commandPanel = new QWidget(rightTabs);
    auto *commandLayout = new QVBoxLayout(commandPanel);
    commandLayout->setContentsMargins(4, 4, 4, 4);
    commandLayout->addWidget(new QLabel(QStringLiteral("Run a Git command in this repository. Suggestions include subcommands and common options."), commandPanel));
    commandEditor = new CodeEditor(commandPanel);
    commandEditor->setMaximumBlockCount(2);
    commandEditor->setFixedHeight(72);
    commandEditor->setPlaceholderText(QStringLiteral("git status --short --branch"));
    commandEditor->setPlainText(QStringLiteral("git status --short --branch"));
    new ShellScratchpad::Syntax::SyntaxHighlighter(commandEditor);
    completionController = new ShellScratchpad::Completion::CompletionController(commandEditor, commandEditor);
    completionController->setWorkingDirectory(repoPath);
    completionController->setGitCommandMode(true);
    commandLayout->addWidget(commandEditor);
    auto *commandRow = new QHBoxLayout;
    auto *runButton = new QPushButton(QStringLiteral("Run Command"), commandPanel);
    runButton->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    auto *rawHelp = new QLabel(QStringLiteral("Ctrl+Enter runs raw Git arguments; no shell expansion."), commandPanel);
    commandRow->addWidget(runButton);
    commandRow->addWidget(rawHelp, 1);
    commandLayout->addLayout(commandRow);
    auto *outputHeader = new QHBoxLayout;
    outputHeader->addWidget(new QLabel(QStringLiteral("Output"), commandPanel));
    outputHeader->addStretch(1);
    auto *copyOutputButton = new QPushButton(QStringLiteral("Copy"), commandPanel);
    auto *clearOutputButton = new QPushButton(QStringLiteral("Clear"), commandPanel);
    outputHeader->addWidget(copyOutputButton);
    outputHeader->addWidget(clearOutputButton);
    commandLayout->addLayout(outputHeader);
    commandOutput = new QPlainTextEdit(commandPanel);
    commandOutput->setReadOnly(true);
    commandOutput->setObjectName(QStringLiteral("outputEdit"));
    commandOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    commandOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    commandOutput->setPlaceholderText(QStringLiteral("Git command output and errors will appear here."));
    commandOutputDocument = new QTextDocument(commandOutput);
    commandOutputDocument->setDocumentLayout(new QPlainTextDocumentLayout(commandOutputDocument));
    commandOutput->setDocument(commandOutputDocument);
    terminalOutputRenderer = new ShellScratchpad::Output::TerminalOutput(commandOutputDocument);
    commandLayout->addWidget(commandOutput, 1);
    auto *commandInputRow = new QHBoxLayout;
    commandInput = new QLineEdit(commandPanel);
    commandInput->setPlaceholderText(QStringLiteral("Send input to a running Git command…"));
    commandInput->setEnabled(false);
    auto *sendInputButton = new QPushButton(QStringLiteral("Send input"), commandPanel);
    sendInputButton->setEnabled(false);
    commandInputRow->addWidget(commandInput, 1);
    commandInputRow->addWidget(sendInputButton);
    commandLayout->addLayout(commandInputRow);
    rightTabs->addTab(commandPanel, QStringLiteral("Git Command"));

    auto *remotesPanel = new QWidget(rightTabs);
    auto *remotesLayout = new QVBoxLayout(remotesPanel);
    remotesLayout->addWidget(new QLabel(QStringLiteral("Manage any number of Git remotes for this repository."), remotesPanel));
    remotesList = new QListWidget(remotesPanel);
    remotesList->setContextMenuPolicy(Qt::CustomContextMenu);
    remotesLayout->addWidget(remotesList, 1);
    auto *remoteActions = new QHBoxLayout;
    auto *addRemoteButton = new QPushButton(QStringLiteral("Add Remote…"), remotesPanel);
    auto *editRemoteButton = new QPushButton(QStringLiteral("Change URL…"), remotesPanel);
    auto *removeRemoteButton = new QPushButton(QStringLiteral("Remove Remote…"), remotesPanel);
    remoteActions->addWidget(addRemoteButton);
    remoteActions->addWidget(editRemoteButton);
    remoteActions->addWidget(removeRemoteButton);
    remoteActions->addStretch(1);
    remotesLayout->addLayout(remoteActions);
    rightTabs->addTab(remotesPanel, QStringLiteral("Remotes"));
    connect(addRemoteButton, &QPushButton::clicked, this, [this] { addRemote(); });
    connect(editRemoteButton, &QPushButton::clicked, this, [this] {
        if (remotesList->currentItem()) changeRemoteUrl(remotesList->currentItem()->data(Qt::UserRole).toString());
    });
    connect(removeRemoteButton, &QPushButton::clicked, this, [this] {
        if (remotesList->currentItem()) removeRemote(remotesList->currentItem()->data(Qt::UserRole).toString());
    });
    connect(remotesList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item) changeRemoteUrl(item->data(Qt::UserRole).toString());
    });
    connect(remotesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = remotesList->itemAt(position);
        if (!item) return;
        remotesList->setCurrentItem(item);
        const QString name = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        QAction *fetch = menu.addAction(QStringLiteral("Fetch %1").arg(name));
        connect(fetch, &QAction::triggered, this, [this, name] { runGit({QStringLiteral("fetch"), name, QStringLiteral("--prune")}); });
        QAction *edit = menu.addAction(QStringLiteral("Change URL…"));
        connect(edit, &QAction::triggered, this, [this, name] { changeRemoteUrl(name); });
        QAction *remove = menu.addAction(QStringLiteral("Remove Remote…"));
        connect(remove, &QAction::triggered, this, [this, name] { removeRemote(name); });
        menu.exec(remotesList->viewport()->mapToGlobal(position));
    });

    auto *credentialsPanel = new QWidget(rightTabs);
    auto *credentialsForm = new QFormLayout(credentialsPanel);
    userNameEdit = new QLineEdit(credentialsPanel);
    emailEdit = new QLineEdit(credentialsPanel);
    credentialHelperCombo = new QComboBox(credentialsPanel);
    credentialHelperCombo->addItem(QStringLiteral("Keep current helper"), QString());
    credentialHelperCombo->addItem(QStringLiteral("Git Credential Manager"), QStringLiteral("manager"));
    credentialHelperCombo->addItem(QStringLiteral("GNOME Keyring / libsecret"), QStringLiteral("/usr/lib/git-core/git-credential-libsecret"));
    credentialHelperCombo->addItem(QStringLiteral("Temporary in-memory cache"), QStringLiteral("cache --timeout=3600"));
    credentialHelperCombo->addItem(QStringLiteral("Plain-text store (not recommended)"), QStringLiteral("store"));
    credentialsForm->addRow(QStringLiteral("Commit name (global)"), userNameEdit);
    credentialsForm->addRow(QStringLiteral("Commit email (global)"), emailEdit);
    credentialsForm->addRow(QStringLiteral("Credential storage (global)"), credentialHelperCombo);
    auto *saveIdentityButton = new QPushButton(QStringLiteral("Save Git Identity"), credentialsPanel);
    auto *configureHelperButton = new QPushButton(QStringLiteral("Configure Helper"), credentialsPanel);
    auto *addCredentialButton = new QPushButton(QStringLiteral("Add / Update Credential…"), credentialsPanel);
    credentialsForm->addRow(saveIdentityButton, configureHelperButton);
    credentialsForm->addRow(addCredentialButton);
    credentialsForm->addRow(new QLabel(QStringLiteral("Credentials are passed to Git’s configured credential helper. The app does not save passwords in its own settings."), credentialsPanel));
    rightTabs->addTab(credentialsPanel, QStringLiteral("Identity & Credentials"));

    mainSplitter->addWidget(leftTabs);
    mainSplitter->addWidget(rightTabs);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 2);
    layout->addWidget(mainSplitter, 1);
    setCentralWidget(root);
    setStyleSheet(QStringLiteral(
        "QPlainTextEdit#outputEdit { border: 1px solid palette(mid); border-radius: 6px; "
        "padding: 6px; background: palette(base); }"));
    branchStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(branchStatusLabel);
    updateStatusBar();

    commandProcess = new QProcess(this);
    commandProcess->setWorkingDirectory(repoPath);
    connect(commandProcess, &QProcess::readyReadStandardOutput, this, [this] {
        const QString output = QString::fromLocal8Bit(commandProcess->readAllStandardOutput());
        if (actionCommandActive) actionOutput += output;
        else appendOutput(output);
    });
    connect(commandProcess, &QProcess::readyReadStandardError, this, [this] {
        const QString output = QString::fromLocal8Bit(commandProcess->readAllStandardError());
        if (actionCommandActive) actionOutput += output;
        else appendOutput(output);
    });
    connect(commandProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (actionCommandActive) {
            QString summary = actionOutput;
            static const QRegularExpression ANSI_SEQUENCE(QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]"));
            summary.remove(ANSI_SEQUENCE);
            summary.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            summary = summary.simplified();
            if (summary.isEmpty()) summary = code == 0 ? QStringLiteral("Completed") : QStringLiteral("Failed");
            if (summary.size() > 180) summary = summary.left(177) + QStringLiteral("…");
            statusBar()->showMessage(QStringLiteral("Git %1: %2").arg(code == 0 ? QStringLiteral("completed")
                                                                                : QStringLiteral("failed"), summary), 7000);
            actionCommandActive = false;
            actionOutput.clear();
        } else {
            appendOutput(QStringLiteral("\n[git exited with code %1]\n").arg(code));
            statusBar()->showMessage(QStringLiteral("Git command exited with code %1").arg(code), 4000);
        }
        commandInput->setEnabled(false);
        refreshAll();
    });
    connect(commandProcess, &QProcess::started, this, [this] {
        if (!actionCommandActive) commandInput->setEnabled(true);
    });
    connect(commandProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        const QString message = commandProcess->errorString();
        if (actionCommandActive) {
            actionCommandActive = false;
            actionOutput.clear();
            statusBar()->showMessage(QStringLiteral("Could not start Git: %1").arg(message), 7000);
        } else {
            appendOutput(QStringLiteral("\nCould not start Git: %1\n").arg(message));
        }
        commandInput->setEnabled(false);
    });
    connect(runButton, &QPushButton::clicked, this, [this] { runRawCommand(); });
    connect(sendInputButton, &QPushButton::clicked, this, [this] { sendCommandInput(); });
    connect(commandInput, &QLineEdit::returnPressed, this, [this] { sendCommandInput(); });
    connect(copyOutputButton, &QPushButton::clicked, this, [this] {
        const QTextCursor cursor = commandOutput->textCursor();
        QApplication::clipboard()->setText(cursor.hasSelection() ? cursor.selectedText() : terminalOutputRenderer->plainText());
    });
    connect(clearOutputButton, &QPushButton::clicked, this, [this] { terminalOutputRenderer->clear(); });
    connect(commandEditor, &QPlainTextEdit::textChanged, this, [this] {
        if (commandEditor->toPlainText().contains(QLatin1Char('\n'))) {
            QTextCursor cursor = commandEditor->textCursor();
            cursor.movePosition(QTextCursor::End);
            commandEditor->setTextCursor(cursor);
            commandEditor->textCursor().deletePreviousChar();
        }
    });
    connect(changesTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!selectedPath().isEmpty()) {
            showDiff(selectedPath());
            rightTabs->setCurrentIndex(0);
        }
    });
    connect(changesTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item && (item->data(0, Qt::UserRole + 1).toBool()))
            resolveConflictFile(item->data(0, Qt::UserRole).toString());
    });
    connect(branchesList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item) return;
        switchToBranch(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toBool());
    });
    connect(historyList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item) showCommit(item->data(Qt::UserRole).toString());
    });
    branchesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(branchesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = branchesList->itemAt(position);
        if (!item) return;
        branchesList->setCurrentItem(item);
        showBranchContextMenu(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toBool(), branchesList->viewport()->mapToGlobal(position));
    });
    historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(historyList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = historyList->itemAt(position);
        if (item) showHistoryContextMenu(item, historyList->viewport()->mapToGlobal(position));
    });
    connect(saveIdentityButton, &QPushButton::clicked, this, [this] { saveIdentity(); });
    connect(configureHelperButton, &QPushButton::clicked, this, [this] { configureCredentialHelper(); });
    connect(addCredentialButton, &QPushButton::clicked, this, [this] { addCredential(); });
    changesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(changesTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = changesTree->itemAt(position);
        if (!item) return;
        changesTree->setCurrentItem(item);
        const QString path = item->data(0, Qt::UserRole).toString();
        const QString status = item->text(1);
        QMenu menu(this);
        if (item->data(0, Qt::UserRole + 1).toBool()) {
            QAction *resolve = menu.addAction(QStringLiteral("Resolve Merge Conflict…"));
            connect(resolve, &QAction::triggered, this, [this, path] { resolveConflictFile(path); });
            menu.addSeparator();
        }
        if (status.contains(QLatin1Char('?')) || status.contains(QLatin1Char('A')) ||
            status.contains(QLatin1Char('M')) || status.contains(QLatin1Char('D')) || status.contains(QLatin1Char('R'))) {
            QAction *stage = menu.addAction(QStringLiteral("Stage"));
            connect(stage, &QAction::triggered, this, [this, path] { runGit({QStringLiteral("add"), QStringLiteral("--"), path}); });
        }
        const bool hasStagedChanges = status.size() >= 2 && status.at(0) != QLatin1Char(' ') && status.at(0) != QLatin1Char('?');
        if (hasStagedChanges) {
            QAction *unstage = menu.addAction(QStringLiteral("Unstage"));
            connect(unstage, &QAction::triggered, this, [this, path] {
                runGit({QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--"), path});
            });
        }
        menu.addSeparator();
        QAction *discard = menu.addAction(QStringLiteral("Discard working tree changes…"));
        const bool hasUnstagedChanges = status.size() >= 2 && status.at(1) != QLatin1Char(' ') && status != QStringLiteral("??");
        discard->setEnabled(hasUnstagedChanges);
        connect(discard, &QAction::triggered, this, [this, path] {
            if (QMessageBox::question(this, QStringLiteral("Discard changes"),
                    QStringLiteral("Discard unstaged changes to %1?").arg(path),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                runGit({QStringLiteral("restore"), QStringLiteral("--worktree"), QStringLiteral("--"), path});
        });
        menu.exec(changesTree->viewport()->mapToGlobal(position));
    });
    refreshAll();
}

QString GitWorkspaceWindow::gitOutput(const QStringList &arguments, int *exitCode) const
{
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments(arguments);
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        if (exitCode) *exitCode = -1;
        return QStringLiteral("Git command timed out.");
    }
    if (exitCode) *exitCode = process.exitCode();
    const QByteArray output = process.readAllStandardOutput() + process.readAllStandardError();
    return QString::fromLocal8Bit(output).trimmed();
}

void GitWorkspaceWindow::refreshAll()
{
    refreshStatus();
    refreshBranches();
    refreshRemotes();
    refreshHistory();
    refreshIdentity();
    hasStashes = !gitOutput({QStringLiteral("stash"), QStringLiteral("list")}).isEmpty();
    updateToolbarState();
    updateStatusBar();
}

void GitWorkspaceWindow::refreshStatus()
{
    changesTree->clear();
    conflictedFiles.clear();
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z")});
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForFinished(5000) || process.exitCode() != 0) {
        changesTree->addTopLevelItem(new QTreeWidgetItem({gitOutput({QStringLiteral("status")}), QString()}));
        return;
    }
    const QList<QByteArray> records = process.readAllStandardOutput().split('\0');
    hasStagedChanges = false;
    hasUnstagedChanges = false;
    stagedPathCount = 0;
    unstagedPathCount = 0;
    for (qsizetype recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
        const QByteArray &record = records.at(recordIndex);
        if (record.size() < 4) continue;
        const QString xy = QString::fromLocal8Bit(record.left(2));
        const bool conflicted = xy.contains(QLatin1Char('U')) || xy == QStringLiteral("AA") || xy == QStringLiteral("DD");
        if (xy.size() >= 2) {
            if (xy.at(0) != QLatin1Char(' ') && xy.at(0) != QLatin1Char('?')) {
                hasStagedChanges = true;
                ++stagedPathCount;
            }
            if (xy.at(1) != QLatin1Char(' ') || xy == QStringLiteral("??")) {
                hasUnstagedChanges = true;
                ++unstagedPathCount;
            }
        }
        const QString path = QString::fromLocal8Bit(record.mid(3));
        const bool hasSecondaryRenamePath = xy.contains(QLatin1Char('R')) || xy.contains(QLatin1Char('C'));
        const QString label = hasSecondaryRenamePath ? QStringLiteral("%1 (from %2)").arg(
            path, recordIndex + 1 < records.size() ? QString::fromLocal8Bit(records.at(recordIndex + 1)) : QString()) : path;
        auto *item = new QTreeWidgetItem({label, xy});
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, conflicted);
        if (conflicted) conflictedFiles.append(path);
        changesTree->addTopLevelItem(item);
        if (hasSecondaryRenamePath)
            ++recordIndex;
    }
    if (changesTree->topLevelItemCount() == 0)
        changesTree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("Working tree clean"), QString()}));
    hasConflicts = !conflictedFiles.isEmpty();
    conflictsButton->setEnabled(hasConflicts);
    conflictsButton->setToolTip(hasConflicts
        ? QStringLiteral("Resolve %1 conflicted file(s)").arg(conflictedFiles.size())
        : QStringLiteral("No merge conflicts to resolve"));
}

void GitWorkspaceWindow::refreshBranches()
{
    branchesList->clear();
    const QString text = gitOutput({QStringLiteral("branch"), QStringLiteral("--all"), QStringLiteral("--no-color")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        QString name = line.trimmed();
        const bool current = name.startsWith(QStringLiteral("* "));
        if (name.startsWith(QStringLiteral("* ")) || name.startsWith(QStringLiteral("  "))) name.remove(0, 2);
        const bool remote = name.startsWith(QStringLiteral("remotes/"));
        if (remote) name.remove(0, QStringLiteral("remotes/").size());
        if (name.contains(QStringLiteral(" -> "))) continue;
        auto *item = new QListWidgetItem((remote ? QStringLiteral("remotes/") : QString()) + name, branchesList);
        item->setData(Qt::UserRole, name);
        item->setData(Qt::UserRole + 1, remote);
        item->setData(Qt::UserRole + 2, current);
    }
    hasRemote = !gitOutput({QStringLiteral("remote")}).isEmpty();
    const QString branch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    const QString upstream = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("@{upstream}")});
    hasUpstream = !upstream.isEmpty() && !upstream.startsWith(QStringLiteral("fatal:"));
    bool countOk = false;
    const int unpushedCount = branch.isEmpty() || !hasRemote ? 0
        : gitOutput({QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD"), QStringLiteral("--not"), QStringLiteral("--remotes")}).toInt(&countOk);
    hasUnpushedCommits = countOk && unpushedCount > 0;
    branchButton->setText(branch.isEmpty() ? QStringLiteral("Detached HEAD") : branch);
}

void GitWorkspaceWindow::pullCurrentBranch()
{
    if (!hasRemote) return;
    if (hasUpstream) {
        runGit({QStringLiteral("pull"), QStringLiteral("--rebase")});
        return;
    }

    QStringList branches = gitOutput({QStringLiteral("for-each-ref"), QStringLiteral("--format=%(refname:short)"), QStringLiteral("refs/remotes")})
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    branches.removeIf([](const QString &ref) { return ref.endsWith(QStringLiteral("/HEAD")); });
    if (branches.isEmpty()) {
        appendOutput(QStringLiteral("No remote branches are available. Fetch remotes, then try Pull again."));
        return;
    }
    bool accepted = false;
    const QString selected = QInputDialog::getItem(this, QStringLiteral("Pull from remote branch"),
        QStringLiteral("Remote branch:"), branches, 0, false, &accepted);
    if (!accepted || selected.isEmpty()) return;
    const int slash = selected.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash == selected.size() - 1) return;
    runGit({QStringLiteral("pull"), QStringLiteral("--rebase"), selected.left(slash), selected.mid(slash + 1)});
}

void GitWorkspaceWindow::pushCurrentBranch()
{
    if (!hasRemote || !hasUnpushedCommits) return;
    if (hasUpstream) {
        runGit({QStringLiteral("push")});
        return;
    }
    const QString branch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    if (branch.isEmpty()) {
        appendOutput(QStringLiteral("Cannot push while HEAD is detached."));
        return;
    }
    const QStringList remotes = gitOutput({QStringLiteral("remote")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (remotes.isEmpty()) return;
    QString remote = remotes.first();
    if (remotes.size() > 1) {
        bool accepted = false;
        remote = QInputDialog::getItem(this, QStringLiteral("Push branch"), QStringLiteral("Remote:"), remotes, 0, false, &accepted);
        if (!accepted || remote.isEmpty()) return;
    }
    runGit({QStringLiteral("push"), QStringLiteral("--set-upstream"), remote, branch});
}

void GitWorkspaceWindow::refreshHistory()
{
    historyList->clear();
    const QString text = gitOutput({QStringLiteral("log"), QStringLiteral("-30"), QStringLiteral("--format=%H%x09%h%x09%s")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList columns = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        if (columns.size() < 3) continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(columns.at(1), columns.mid(2).join(QLatin1Char('\t'))));
        item->setData(Qt::UserRole, columns.at(0));
        historyList->addItem(item);
    }
}

void GitWorkspaceWindow::refreshRemotes()
{
    if (!remotesList) return;
    remotesList->clear();
    const QStringList names = gitOutput({QStringLiteral("remote")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &name : names) {
        const QString urls = gitOutput({QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("--all"), name});
        auto *item = new QListWidgetItem(name, remotesList);
        item->setData(Qt::UserRole, name);
        item->setToolTip(urls.isEmpty() ? name : QStringLiteral("%1\n%2").arg(name, urls));
    }
}

void GitWorkspaceWindow::addRemote()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Add Git remote"), QStringLiteral("Remote name:"),
        QLineEdit::Normal, QStringLiteral("origin"), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    const QString url = QInputDialog::getText(this, QStringLiteral("Add Git remote"), QStringLiteral("Fetch URL:"),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (accepted && !url.isEmpty()) runGit({QStringLiteral("remote"), QStringLiteral("add"), name, url});
}

void GitWorkspaceWindow::changeRemoteUrl(const QString &name)
{
    if (name.isEmpty()) return;
    const QString currentUrl = gitOutput({QStringLiteral("remote"), QStringLiteral("get-url"), name});
    bool accepted = false;
    const QString url = QInputDialog::getText(this, QStringLiteral("Change remote URL"),
        QStringLiteral("Fetch URL for %1:").arg(name), QLineEdit::Normal, currentUrl, &accepted).trimmed();
    if (accepted && !url.isEmpty() && url != currentUrl)
        runGit({QStringLiteral("remote"), QStringLiteral("set-url"), name, url});
}

void GitWorkspaceWindow::removeRemote(const QString &name)
{
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("Remove Git remote"),
        QStringLiteral("Remove remote %1 and its repository configuration?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        runGit({QStringLiteral("remote"), QStringLiteral("remove"), name});
}

void GitWorkspaceWindow::switchToBranch(const QString &branch, bool remote)
{
    if (branch.isEmpty()) return;
    if (!remote) {
        runGit({QStringLiteral("switch"), branch});
        return;
    }
    const QString localName = branch.section(QLatin1Char('/'), 1);
    const QStringList localBranches = gitOutput({QStringLiteral("branch"), QStringLiteral("--format=%(refname:short)")})
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (localBranches.contains(localName)) {
        runGit({QStringLiteral("switch"), localName});
        return;
    }
    const auto answer = QMessageBox::question(this, QStringLiteral("Create local branch?"),
        QStringLiteral("%1 exists only on the remote. Create a local tracking branch and switch to it?").arg(branch),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes)
        runGit({QStringLiteral("switch"), QStringLiteral("--track"), QStringLiteral("-c"), localName, branch});
}

void GitWorkspaceWindow::showBranchContextMenu(const QString &branch, bool remote, const QPoint &globalPosition)
{
    if (branch.isEmpty()) return;
    QMenu menu(this);
    const QString currentBranch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    const bool canMerge = branch != currentBranch;
    QMenu *mergeMenu = menu.addMenu(QStringLiteral("Merge into Current Branch"));
    QAction *merge = mergeMenu->addAction(QStringLiteral("Merge"));
    QAction *noFastForward = mergeMenu->addAction(QStringLiteral("Merge without Fast-forward"));
    QAction *squash = mergeMenu->addAction(QStringLiteral("Squash Merge"));
    merge->setEnabled(canMerge);
    noFastForward->setEnabled(canMerge);
    squash->setEnabled(canMerge);
    const QString mergeTarget = remote ? QStringLiteral("refs/remotes/%1").arg(branch) : branch;
    connect(merge, &QAction::triggered, this, [this, mergeTarget] {
        runGit({QStringLiteral("merge"), QStringLiteral("--no-edit"), mergeTarget});
    });
    connect(noFastForward, &QAction::triggered, this, [this, mergeTarget] {
        runGit({QStringLiteral("merge"), QStringLiteral("--no-edit"), QStringLiteral("--no-ff"), mergeTarget});
    });
    connect(squash, &QAction::triggered, this, [this, mergeTarget] {
        runGit({QStringLiteral("merge"), QStringLiteral("--squash"), mergeTarget});
    });
    menu.addSeparator();
    QAction *switchAction = menu.addAction(remote ? QStringLiteral("Create or switch to local branch…") : QStringLiteral("Switch to Branch"));
    connect(switchAction, &QAction::triggered, this, [this, branch, remote] { switchToBranch(branch, remote); });
    menu.addSeparator();
    if (remote) {
        const QString remoteName = branch.section(QLatin1Char('/'), 0, 0);
        const QString remoteBranch = branch.mid(remoteName.size() + 1);
        QAction *fetch = menu.addAction(QStringLiteral("Fetch %1").arg(remoteName));
        connect(fetch, &QAction::triggered, this, [this, remoteName] { runGit({QStringLiteral("fetch"), remoteName, QStringLiteral("--prune")}); });
        QAction *remove = menu.addAction(QStringLiteral("Delete from Remote…"));
        connect(remove, &QAction::triggered, this, [this, branch, remoteName, remoteBranch] {
            if (QMessageBox::question(this, QStringLiteral("Delete remote branch"),
                QStringLiteral("Delete %1 from remote %2? This affects everyone using that remote.").arg(remoteBranch, remoteName),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                runGit({QStringLiteral("push"), remoteName, QStringLiteral("--delete"), remoteBranch});
        });
    } else {
        QAction *rename = menu.addAction(QStringLiteral("Rename…"));
        connect(rename, &QAction::triggered, this, [this, branch] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Rename branch"), QStringLiteral("New branch name:"),
                QLineEdit::Normal, branch, &ok).trimmed();
            if (ok && !name.isEmpty() && name != branch) runGit({QStringLiteral("branch"), QStringLiteral("-m"), branch, name});
        });
        const QString current = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
        QAction *remove = menu.addAction(QStringLiteral("Delete Local Branch…"));
        remove->setEnabled(branch != current);
        connect(remove, &QAction::triggered, this, [this, branch] {
            if (QMessageBox::question(this, QStringLiteral("Delete local branch"),
                QStringLiteral("Delete local branch %1? Git will refuse if it has unmerged work.").arg(branch),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                runGit({QStringLiteral("branch"), QStringLiteral("-d"), branch});
        });
        QAction *push = menu.addAction(QStringLiteral("Push Branch to Remote…"));
        push->setEnabled(hasRemote);
        connect(push, &QAction::triggered, this, [this, branch] {
            const QString remoteName = gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("branch.%1.remote").arg(branch)});
            runGit(remoteName.isEmpty()
                ? QStringList{QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), branch}
                : QStringList{QStringLiteral("push"), QStringLiteral("-u"), remoteName, branch});
        });
    }
    menu.addSeparator();
    QAction *copy = menu.addAction(QStringLiteral("Copy Branch Name"));
    connect(copy, &QAction::triggered, this, [branch] { QApplication::clipboard()->setText(branch); });
    menu.exec(globalPosition);
}

void GitWorkspaceWindow::showHistoryContextMenu(QListWidgetItem *item, const QPoint &globalPosition)
{
    if (!item) return;
    const QString hash = item->data(Qt::UserRole).toString();
    if (hash.isEmpty()) return;
    historyList->setCurrentItem(item);
    QMenu menu(this);
    QAction *show = menu.addAction(QStringLiteral("Show Commit Details"));
    connect(show, &QAction::triggered, this, [this, hash] {
        showCommit(hash);
    });
    QAction *copy = menu.addAction(QStringLiteral("Copy Commit Hash"));
    connect(copy, &QAction::triggered, this, [hash] { QApplication::clipboard()->setText(hash); });
    menu.addSeparator();
    QAction *branch = menu.addAction(QStringLiteral("Create Branch at Commit…"));
    connect(branch, &QAction::triggered, this, [this, hash] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Branch at commit"), QStringLiteral("New branch name:"),
            QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty()) runGit({QStringLiteral("switch"), QStringLiteral("-c"), name, hash});
    });
    QAction *cherryPick = menu.addAction(QStringLiteral("Cherry-pick Commit"));
    connect(cherryPick, &QAction::triggered, this, [this, hash] { runGit({QStringLiteral("cherry-pick"), hash}); });
    QAction *revert = menu.addAction(QStringLiteral("Revert Commit…"));
    connect(revert, &QAction::triggered, this, [this, hash] {
        if (QMessageBox::question(this, QStringLiteral("Revert commit"), QStringLiteral("Create a new commit that reverses %1?").arg(hash.left(10)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            runGit({QStringLiteral("revert"), hash});
    });
    QAction *checkout = menu.addAction(QStringLiteral("Checkout Commit Detached"));
    connect(checkout, &QAction::triggered, this, [this, hash] { runGit({QStringLiteral("switch"), QStringLiteral("--detach"), hash}); });
    menu.exec(globalPosition);
}

void GitWorkspaceWindow::resolveConflictFile(const QString &path)
{
    const QString absolutePath = QDir(repoPath).filePath(path);
    ConflictResolverDialog dialog(absolutePath, this);
    if (!dialog.errorMessage().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Cannot open conflict resolver"), dialog.errorMessage());
        return;
    }
    if (dialog.exec() != QDialog::Accepted) return;
    runGit({QStringLiteral("add"), QStringLiteral("--"), path});
}

void GitWorkspaceWindow::refreshIdentity()
{
    userNameEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.name")}));
    emailEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.email")}));
}

void GitWorkspaceWindow::runGit(const QStringList &arguments)
{
    if (commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git is busy"), QStringLiteral("Wait for the current Git command to finish."));
        return;
    }
    QStringList fullArguments{QStringLiteral("-C"), repoPath};
    fullArguments.append(arguments);
    commandProcess->setArguments(fullArguments);
    commandProcess->setProgram(gitExecutable());
    commandProcess->setWorkingDirectory(repoPath);
    actionCommandActive = true;
    actionOutput.clear();
    statusBar()->showMessage(QStringLiteral("Running git %1…").arg(arguments.join(QLatin1Char(' '))));
    commandProcess->start();
}

void GitWorkspaceWindow::runRawCommand()
{
    if (commandProcess->state() != QProcess::NotRunning) {
        statusBar()->showMessage(QStringLiteral("Wait for the current Git command to finish."), 3000);
        return;
    }
    QStringList args = QProcess::splitCommand(commandEditor->toPlainText().trimmed());
    if (!args.isEmpty() && args.first() == QLatin1String("git")) args.removeFirst();
    if (args.isEmpty()) return;
    appendOutput(QStringLiteral("$ git %1\n").arg(args.join(QLatin1Char(' '))));
    actionCommandActive = false;
    commandProcess->setProgram(gitExecutable());
    commandProcess->setArguments(args);
    commandProcess->setWorkingDirectory(repoPath);
    rightTabs->setCurrentIndex(1);
    commandProcess->start();
}

QString GitWorkspaceWindow::selectedPath() const
{
    const auto items = changesTree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, Qt::UserRole).toString();
}

void GitWorkspaceWindow::showDiff(const QString &path)
{
    if (path.isEmpty()) return;
    currentDiffCommitHash.clear();
    diffFilesList->clear();
    auto *fileItem = new QListWidgetItem(path, diffFilesList);
    fileItem->setData(Qt::UserRole, path);
    diffSummaryLabel->setText(QStringLiteral("Working tree changes compared with HEAD"));
    originalDiffLabel->setText(QStringLiteral("Latest in repository (HEAD)"));
    changedDiffLabel->setText(QStringLiteral("Working file"));
    QByteArray originalBytes;
    QProcess originalProcess;
    originalProcess.setProgram(gitExecutable());
    originalProcess.setArguments({QStringLiteral("show"), QStringLiteral("HEAD:") + path});
    originalProcess.setWorkingDirectory(repoPath);
    originalProcess.start();
    if (originalProcess.waitForFinished(10000) && originalProcess.exitCode() == 0)
        originalBytes = originalProcess.readAllStandardOutput();

    QByteArray workingBytes;
    QFile workingFile(QDir(repoPath).filePath(path));
    if (workingFile.open(QIODevice::ReadOnly))
        workingBytes = workingFile.readAll();
    if (originalBytes.contains('\0') || workingBytes.contains('\0')) {
        detailsView->setPlainText(QStringLiteral("This file contains binary data and cannot be shown as a text diff."));
        detailsStack->setCurrentWidget(detailsView);
        return;
    }
    const QVector<DiffLine> rows = makeSideBySideDiff(QString::fromUtf8(originalBytes), QString::fromUtf8(workingBytes));
    if (rows.isEmpty()) {
        detailsView->setPlainText(QStringLiteral("No textual diff is available for this path."));
        detailsStack->setCurrentWidget(detailsView);
        return;
    }
    renderDiffSide(originalFileView, rows, true);
    renderDiffSide(workingFileView, rows, false);
    detailsStack->setCurrentWidget(detailsStack->widget(1));
}

void GitWorkspaceWindow::showCommit(const QString &hash)
{
    if (hash.isEmpty()) return;
    currentDiffCommitHash = hash;
    diffFilesList->clear();
    int parentCode = -1;
    const QString parent = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--verify"), hash + QLatin1Char('^')}, &parentCode);
    QStringList arguments{QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"),
        QStringLiteral("--name-status"), QStringLiteral("-r"), QStringLiteral("-M"), QStringLiteral("-z")};
    if (parentCode != 0) arguments.append(QStringLiteral("--root"));
    if (parentCode == 0) arguments << parent << hash;
    else arguments << hash;
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments(arguments);
    process.setWorkingDirectory(repoPath);
    process.start();
    QByteArray records;
    if (process.waitForFinished(10000) && process.exitCode() == 0)
        records = process.readAllStandardOutput();

    const QList<QByteArray> fields = records.split('\0');
    for (qsizetype index = 0; index < fields.size();) {
        const QString status = QString::fromLocal8Bit(fields.at(index++));
        if (status.isEmpty() || index >= fields.size()) continue;
        QString oldPath;
        QString path;
        if (status.startsWith(QLatin1Char('R')) || status.startsWith(QLatin1Char('C'))) {
            oldPath = QString::fromLocal8Bit(fields.at(index++));
            if (index >= fields.size()) break;
            path = QString::fromLocal8Bit(fields.at(index++));
        } else {
            path = QString::fromLocal8Bit(fields.at(index++));
            oldPath = path;
        }
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(status, path), diffFilesList);
        item->setData(Qt::UserRole, path);
        item->setData(Qt::UserRole + 1, oldPath);
        item->setToolTip(path == oldPath ? path : QStringLiteral("%1 → %2").arg(oldPath, path));
    }
    if (diffFilesList->count() == 0) {
        currentDiffCommitHash.clear();
        detailsView->setPlainText(gitOutput({QStringLiteral("show"), QStringLiteral("--no-patch"),
            QStringLiteral("--format=fuller"), hash}));
        detailsStack->setCurrentWidget(detailsView);
    } else {
        diffSummaryLabel->setText(QStringLiteral("Commit %1 — select a changed file").arg(hash.left(10)));
        diffFilesList->setCurrentRow(0);
        detailsStack->setCurrentWidget(detailsStack->widget(1));
    }
    rightTabs->setCurrentWidget(detailsStack);
}

void GitWorkspaceWindow::showCommitFile(const QString &path, const QString &oldPath)
{
    if (currentDiffCommitHash.isEmpty() || path.isEmpty()) return;
    int parentCode = -1;
    const QString parent = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--verify"),
        currentDiffCommitHash + QLatin1Char('^')}, &parentCode);
    const QString previousPath = oldPath.isEmpty() ? path : oldPath;
    auto readGitFile = [this](const QString &spec) {
        QProcess process;
        process.setProgram(gitExecutable());
        process.setArguments({QStringLiteral("show"), spec});
        process.setWorkingDirectory(repoPath);
        process.start();
        if (!process.waitForFinished(10000) || process.exitCode() != 0) return QByteArray();
        return process.readAllStandardOutput();
    };
    const QByteArray originalBytes = parentCode == 0 ? readGitFile(parent + QLatin1Char(':') + previousPath) : QByteArray();
    const QByteArray modifiedBytes = readGitFile(currentDiffCommitHash + QLatin1Char(':') + path);
    diffSummaryLabel->setText(QStringLiteral("Commit %1 — %2").arg(currentDiffCommitHash.left(10), path));
    originalDiffLabel->setText(QStringLiteral("Parent commit"));
    changedDiffLabel->setText(QStringLiteral("Selected commit"));
    if (originalBytes.contains('\0') || modifiedBytes.contains('\0')) {
        detailsView->setPlainText(QStringLiteral("This file contains binary data and cannot be shown as a text diff."));
        detailsStack->setCurrentWidget(detailsView);
        return;
    }
    const QVector<DiffLine> rows = makeSideBySideDiff(QString::fromUtf8(originalBytes), QString::fromUtf8(modifiedBytes));
    renderDiffSide(originalFileView, rows, true);
    renderDiffSide(workingFileView, rows, false);
    detailsStack->setCurrentWidget(detailsStack->widget(1));
}

void GitWorkspaceWindow::saveIdentity()
{
    const QString name = userNameEdit->text().trimmed();
    const QString email = emailEdit->text().trimmed();
    if (name.isEmpty() || email.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Git identity required"), QStringLiteral("Enter both a commit name and email."));
        return;
    }
    int nameCode = -1;
    int emailCode = -1;
    const QString nameOutput = gitOutput({QStringLiteral("config"), QStringLiteral("--global"),
        QStringLiteral("user.name"), name}, &nameCode);
    const QString emailOutput = gitOutput({QStringLiteral("config"), QStringLiteral("--global"),
        QStringLiteral("user.email"), email}, &emailCode);
    if (nameCode != 0 || emailCode != 0)
        QMessageBox::warning(this, QStringLiteral("Could not save Git identity"), nameOutput + QLatin1Char('\n') + emailOutput);
    else
        statusBar()->showMessage(QStringLiteral("Git identity saved globally"), 3000);
}

void GitWorkspaceWindow::configureCredentialHelper()
{
    const QString helper = credentialHelperCombo->currentData().toString();
    if (helper.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Credential helper"),
            gitOutput({QStringLiteral("config"), QStringLiteral("--show-origin"), QStringLiteral("--get-all"), QStringLiteral("credential.helper")}));
        return;
    }
    int code = -1;
    const QString output = gitOutput({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("credential.helper"), helper}, &code);
    statusBar()->showMessage(code == 0 ? QStringLiteral("Credential helper configured") : output, 5000);
}

void GitWorkspaceWindow::addCredential()
{
    const QString helpers = gitOutput({QStringLiteral("config"), QStringLiteral("--get-all"),
                                       QStringLiteral("credential.helper")});
    if (helpers.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Credential helper not configured"),
            QStringLiteral("Choose and configure a credential helper first. Git will only store this credential through that helper."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Git credential"));
    dialog.setMinimumWidth(640);
    dialog.resize(680, dialog.sizeHint().height());
    auto *form = new QFormLayout(&dialog);
    auto *host = new QLineEdit(&dialog);
    auto *username = new QLineEdit(&dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Host (e.g. github.com)"), host);
    form->addRow(QStringLiteral("Username"), username);
    form->addRow(QStringLiteral("Token / password"), password);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (host->text().trimmed().isEmpty() || username->text().isEmpty() || password->text().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Credential incomplete"), QStringLiteral("Enter host, username, and token."));
        return;
    }
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("credential"), QStringLiteral("approve")});
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForStarted()) {
        QMessageBox::warning(this, QStringLiteral("Git unavailable"), process.errorString());
        return;
    }
    const QByteArray input = QByteArray("protocol=https\nhost=") + host->text().trimmed().toUtf8()
        + QByteArray("\nusername=") + username->text().toUtf8()
        + QByteArray("\npassword=") + password->text().toUtf8() + QByteArray("\n\n");
    process.write(input);
    process.closeWriteChannel();
    password->clear();
    if (!process.waitForFinished(10000) || process.exitCode() != 0)
        QMessageBox::warning(this, QStringLiteral("Credential was not stored"), QString::fromLocal8Bit(process.readAllStandardError()));
    else
        statusBar()->showMessage(QStringLiteral("Credential passed to Git’s configured helper"), 5000);
}

void GitWorkspaceWindow::appendOutput(const QString &text)
{
    if (text.isEmpty()) return;
    if (commandOutput->viewport()->height() > 0) {
        const int lineHeight = qMax(1, commandOutput->fontMetrics().lineSpacing());
        terminalOutputRenderer->setScreenRows(commandOutput->viewport()->height() / lineHeight);
    }
    terminalOutputRenderer->append(text);
    commandOutput->setTextCursor(terminalOutputRenderer->cursor());
    commandOutput->ensureCursorVisible();
}

void GitWorkspaceWindow::sendCommandInput()
{
    if (!commandInput->isEnabled() || commandProcess->state() == QProcess::NotRunning || actionCommandActive)
        return;
    commandProcess->write((commandInput->text() + QLatin1Char('\n')).toLocal8Bit());
    commandInput->clear();
}

void GitWorkspaceWindow::updateToolbarState()
{
    if (commitButton) {
        commitButton->setVisible(hasStagedChanges);
        commitButton->setEnabled(hasStagedChanges);
    }
    if (stageAllButton) {
        stageAllButton->setVisible(hasUnstagedChanges);
        stageAllButton->setEnabled(hasUnstagedChanges);
    }
    if (syncButton) syncButton->setVisible(hasRemote && (hasUpstream || hasUnpushedCommits));
    if (pullButton) pullButton->setVisible(hasRemote);
    if (pushButton) {
        pushButton->setVisible(hasRemote && hasUnpushedCommits);
        pushButton->setEnabled(hasRemote && hasUnpushedCommits);
    }
    if (branchButton) branchButton->setEnabled(true);
}

void GitWorkspaceWindow::updateStatusBar()
{
    if (!branchStatusLabel) return;
    const QString branch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    const QString branchText = branch.isEmpty() ? QStringLiteral("Detached HEAD") : branch;
    const QString changes = hasStagedChanges || hasUnstagedChanges
        ? QStringLiteral("%1 staged • %2 unstaged").arg(stagedPathCount).arg(unstagedPathCount)
        : QStringLiteral("Working tree clean");
    branchStatusLabel->setText(QStringLiteral("%1  •  %2").arg(branchText, changes));
}
}
