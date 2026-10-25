#include "GitWorkspaceWindow.h"
#include "BatchOperationsDialog.h"
#include "RepositoryOperationCoordinator.h"

#include "completion/CompletionController.h"
#include "editor/CodeEditor.h"
#include "output/TerminalOutput.h"
#include "syntax/SyntaxHighlighter.h"

#include <QComboBox>
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QBuffer>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QIcon>
#include <QKeySequence>
#include <QMenuBar>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPlainTextDocumentLayout>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QPushButton>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QStatusBar>
#include <QProgressBar>
#include <QProgressDialog>
#include <QSplitter>
#include <QStandardPaths>
#include <QSize>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QGridLayout>
#include <QVBoxLayout>
#include <functional>
#include <utility>
#include <QFileInfo>
#include <QHash>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QTextEdit>
#include <QStackedWidget>
#include <QResizeEvent>
#include <QPainter>
#include <QLineF>
#include <QMouseEvent>
#include <QToolTip>
#include <QTemporaryFile>
#include <QScrollBar>
#include <QScrollArea>
#include <algorithm>

#include <QTimer>
#include <QUrl>
#include <QCryptographicHash>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCloseEvent>
#include <QEvent>
#include <QTimer>
#include <memory>
#include <limits>

namespace ShellScratchpad::Git
{
class HistoryGraphWidget final : public QWidget
{
public:
    struct Node { QString hash; QStringList parents; QString subject; QStringList refs; int lane = 0; bool merge = false; };
    struct Edge { int from; int to; QString branch; QColor color; };
    QVector<Node> nodes;
    QVector<Edge> edges;
    QHash<int,QString> laneNames;
    QHash<int,QColor> laneColors;
    std::function<void(const QString &)> commitClicked;
    explicit HistoryGraphWidget(QWidget *parent = nullptr) : QWidget(parent) { setMouseTracking(true); }
    QSize sizeHint() const override { return {640, qMax(360, static_cast<int>(nodes.size()) * 34 + 24)}; }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        QHash<QString,int> row; for (int i=0;i<nodes.size();++i) row.insert(nodes[i].hash,i);
        const int top=20, step=34;
        for (const auto &edge: edges) {
            const int y1=top+edge.from*step, y2=top+edge.to*step;
            p.setPen(QPen(edge.color, nodes[edge.from].lane==0 ? 4 : 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(36+nodes[edge.from].lane*22,y1,36+nodes[edge.to].lane*22,y2);
        }
        for (int i=0;i<nodes.size();++i) {
            const int y=top+i*step; const auto &n=nodes[i];
            const int x=36+n.lane*22;
            p.setPen(QPen(QColor("#34495e"),2)); p.setBrush(n.merge ? QColor("#f1b84b") : palette().base());
            if (n.merge) { QPolygon diamond; diamond << QPoint(x,y-8) << QPoint(x+8,y) << QPoint(x,y+8) << QPoint(x-8,y); p.drawPolygon(diamond); }
            else p.drawEllipse(QPoint(x,y),6,6);
            p.setPen(palette().text().color());
            p.drawText(qMax(56,x+20),y+5,QStringLiteral("%1  %2").arg(n.hash.left(7),n.subject));
            int refX=430;
            for (const QString &ref:n.refs) { p.setPen(Qt::NoPen); p.setBrush(QColor("#dcebdc"));
                const QRect pill(refX,y-10,qMin(200, ref.size()*7+14),20); p.drawRoundedRect(pill,8,8);
                p.setPen(QColor("#285b35")); p.drawText(pill,Qt::AlignCenter,ref); refX+=pill.width()+5; }
        }
        setMinimumHeight(sizeHint().height());
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        QStringList names;
        for(const auto &edge:edges) {
            const QPointF a(36+nodes[edge.from].lane*22,20+edge.from*34);
            const QPointF b(36+nodes[edge.to].lane*22,20+edge.to*34);
            const QLineF ab(a,b); const double length2=ab.length()*ab.length();
            const double t=length2>0 ? qBound(0.0,QPointF::dotProduct(e->pos()-a,b-a)/length2,1.0) : 0.0;
            if(QLineF(e->pos(),a+t*(b-a)).length()<7.0 && !edge.branch.isEmpty()) names<<edge.branch;
        }
        names.removeDuplicates();
        if(!names.isEmpty()) QToolTip::showText(e->globalPosition().toPoint(),names.join(QStringLiteral("\n")),this);
        else QToolTip::hideText();
    }
    void mousePressEvent(QMouseEvent *e) override {
        const int row=(e->pos().y()-20+17)/34;
        if(e->button()==Qt::LeftButton && row>=0 && row<nodes.size() && qAbs(e->pos().x()-(36+nodes[row].lane*22))<12 && commitClicked) commitClicked(nodes[row].hash);
    }
};
namespace
{
class CommitMessageShortcutFilter final : public QObject
{
public:
    CommitMessageShortcutFilter(QObject *parent, std::function<void()> commit,
                                std::function<void()> commitAndPush)
        : QObject(parent), commit(std::move(commit)), commitAndPush(std::move(commitAndPush))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched);
        if (event->type() != QEvent::KeyPress) return false;
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() != Qt::Key_Return && keyEvent->key() != Qt::Key_Enter) return false;

        const Qt::KeyboardModifiers shortcutModifiers = keyEvent->modifiers()
            & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
        if (shortcutModifiers == Qt::ControlModifier) {
            commit();
            keyEvent->accept();
            return true;
        }
        if (shortcutModifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
            commitAndPush();
            keyEvent->accept();
            return true;
        }
        return false;
    }

private:
    std::function<void()> commit;
    std::function<void()> commitAndPush;
};
}

class ImagePreviewLabel final : public QLabel
{
public:
    explicit ImagePreviewLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setMinimumSize(100, 100);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        setWordWrap(true);
        renderTimer.setSingleShot(true);
        renderTimer.setInterval(100);
        QObject::connect(&renderTimer, &QTimer::timeout, this, [this] { updatePreview(); });
        spinnerTimer.setInterval(180);
        QObject::connect(&spinnerTimer, &QTimer::timeout, this, [this] {
            spinnerFrame = (spinnerFrame + 1) % 4;
            setText(QStringLiteral("Rendering SVG%1").arg(QString(spinnerFrame, QLatin1Char('.'))));
        });
    }

    void setImage(const QImage &image, const QString &fallbackText)
    {
        sourceImage = image;
        svgBytes.clear();
        svgPath.clear();
        emptyText = fallbackText;
        cancelSvgRender();
        updatePreview();
    }

    void setSvgData(const QByteArray &bytes, const QString &path, const QString &fallbackText)
    {
        sourceImage = {};
        svgBytes = bytes;
        svgPath = path;
        emptyText = fallbackText;
        updatePreview();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        if (!svgBytes.isEmpty()) renderTimer.start();
        else updatePreview();
    }

private:
    QImage sourceImage;
    QString emptyText;
    QByteArray svgBytes;
    QString svgPath;
    QTimer renderTimer;
    QTimer spinnerTimer;
    int spinnerFrame = 0;
    QProcess *activeProcess = nullptr;
    QTemporaryFile *activeSvgFile = nullptr;

    void cancelSvgRender()
    {
        renderTimer.stop();
        spinnerTimer.stop();
        if (activeProcess) {
            disconnect(activeProcess, nullptr, this, nullptr);
            activeProcess->kill();
            activeProcess->deleteLater();
            activeProcess = nullptr;
        }
        if (activeSvgFile) {
            activeSvgFile->deleteLater();
            activeSvgFile = nullptr;
        }
    }

    void startSvgRender(const QSize &size)
    {
        cancelSvgRender();
        if (size.isEmpty() || svgBytes.isEmpty()) return;
        auto *svgFile = new QTemporaryFile(this);
        if (!svgFile->open() || svgFile->write(svgBytes) != svgBytes.size() || !svgFile->flush()) {
            delete svgFile;
            setText(QStringLiteral("The SVG data could not be prepared for rendering."));
            return;
        }
        const QString magick = QStandardPaths::findExecutable(QStringLiteral("magick"));
        const QString convert = magick.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("convert")) : magick;
        if (convert.isEmpty()) {
            delete svgFile;
            setText(QStringLiteral("SVG rendering is unavailable because no SVG image reader is installed."));
            return;
        }
        // ImageMagick's SVG delegate rasterizes at density 96 as only a quarter
        // of the requested pixel dimensions. Supersampling keeps enlargement crisp.
        const int density = std::clamp(std::max(size.width(), size.height()) * 16, 96, 32768);
        auto *process = new QProcess(this);
        process->setProgram(convert);
        process->setArguments({QStringLiteral("-background"), QStringLiteral("none"),
            QStringLiteral("-density"), QString::number(density),
            QStringLiteral("svg:") + svgFile->fileName(), QStringLiteral("png:-")});
        activeProcess = process;
        activeSvgFile = svgFile;
        spinnerFrame = 0;
        setPixmap(QPixmap());
        setText(QStringLiteral("Rendering SVG"));
        spinnerTimer.start();
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, svgFile](int exitCode, QProcess::ExitStatus) {
                if (activeProcess != process) return;
                activeProcess = nullptr;
                activeSvgFile = nullptr;
                spinnerTimer.stop();
                if (exitCode == 0) {
                    const QImage image = QImage::fromData(process->readAllStandardOutput(), "PNG");
                    if (!image.isNull()) {
                        setText(QString());
                        setPixmap(QPixmap::fromImage(image).scaled(contentsRect().size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    } else setText(QStringLiteral("The SVG renderer returned no image."));
                } else {
                    setText(QStringLiteral("SVG rendering failed: %1").arg(QString::fromLocal8Bit(process->readAllStandardError()).trimmed()));
                }
                process->deleteLater();
                svgFile->deleteLater();
            });
        connect(process, &QProcess::errorOccurred, this, [this, process, svgFile](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || activeProcess != process) return;
            activeProcess = nullptr;
            activeSvgFile = nullptr;
            spinnerTimer.stop();
            setText(QStringLiteral("Could not start the SVG renderer."));
            process->deleteLater();
            svgFile->deleteLater();
        });
        process->start();
    }

    void updatePreview()
    {
        if (!svgBytes.isEmpty()) {
            startSvgRender(contentsRect().size());
            return;
        }
        const QImage image = sourceImage;
        if (image.isNull()) {
            clear();
            setText(emptyText);
            return;
        }
        setText(QString());
        setPixmap(QPixmap::fromImage(image).scaled(contentsRect().size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
};

namespace
{
struct RepositorySettingDefinition
{
    QString key;
    QString label;
    QStringList labels;
    QStringList values;
};

const QVector<RepositorySettingDefinition> &repositorySettingDefinitions()
{
    static const QVector<RepositorySettingDefinition> definitions{
        {QStringLiteral("core.autocrlf"), QStringLiteral("Line ending conversion"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("Off"), QStringLiteral("Convert on commit"), QStringLiteral("Convert on checkout and commit")},
            {QStringLiteral("__inherit__"), QStringLiteral("false"), QStringLiteral("input"), QStringLiteral("true")}},
        {QStringLiteral("core.filemode"), QStringLiteral("Track executable bit changes"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("pull.rebase"), QStringLiteral("Pull strategy"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("Merge commits"), QStringLiteral("Rebase local commits"), QStringLiteral("Rebase and preserve merges"), QStringLiteral("Interactive rebase")},
            {QStringLiteral("__inherit__"), QStringLiteral("false"), QStringLiteral("true"), QStringLiteral("merges"), QStringLiteral("interactive")}},
        {QStringLiteral("pull.ff"), QStringLiteral("Allow pull fast-forward"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("When possible"), QStringLiteral("Never"), QStringLiteral("Only fast-forward")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("only")}},
        {QStringLiteral("fetch.prune"), QStringLiteral("Prune deleted remote branches on fetch"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("push.autoSetupRemote"), QStringLiteral("Set upstream on first push"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("push.default"), QStringLiteral("Default push target"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("Current branch"), QStringLiteral("Upstream branch"), QStringLiteral("Same named branches"), QStringLiteral("Matching branches"), QStringLiteral("Nothing")},
            {QStringLiteral("__inherit__"), QStringLiteral("current"), QStringLiteral("upstream"), QStringLiteral("simple"), QStringLiteral("matching"), QStringLiteral("nothing")}},
        {QStringLiteral("merge.ff"), QStringLiteral("Merge fast-forward behavior"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("When possible"), QStringLiteral("Never"), QStringLiteral("Only fast-forward")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("only")}},
        {QStringLiteral("commit.gpgsign"), QStringLiteral("Sign commits"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("rebase.autoStash"), QStringLiteral("Automatically stash before rebase"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("rebase.autosquash"), QStringLiteral("Autosquash fixup commits"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}},
        {QStringLiteral("rerere.enabled"), QStringLiteral("Reuse recorded conflict resolutions"),
            {QStringLiteral("Use inherited setting"), QStringLiteral("On"), QStringLiteral("Off")},
            {QStringLiteral("__inherit__"), QStringLiteral("true"), QStringLiteral("false")}}
    };
    return definitions;
}

bool isManagedRepositorySetting(const QString &key)
{
    return key.startsWith(QStringLiteral("remote."), Qt::CaseInsensitive)
        || key.startsWith(QStringLiteral("branch."), Qt::CaseInsensitive)
        || key.compare(QStringLiteral("user.name"), Qt::CaseInsensitive) == 0
        || key.compare(QStringLiteral("user.email"), Qt::CaseInsensitive) == 0
        || key.compare(QStringLiteral("credential.helper"), Qt::CaseInsensitive) == 0;
}

bool isCuratedRepositorySetting(const QString &key)
{
    return std::any_of(repositorySettingDefinitions().cbegin(), repositorySettingDefinitions().cend(),
        [&key](const RepositorySettingDefinition &setting) { return setting.key.compare(key, Qt::CaseInsensitive) == 0; });
}

QString kwalletCredentialHelperPath()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("git-credential-kwallet"));
    if (path.isEmpty()) {
        const QString adjacent = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("git-credential-kwallet"));
        if (QFileInfo(adjacent).isExecutable()) path = adjacent;
    }
    return path;
}

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

struct DiffGroup
{
    int startRow = 0;
    int endRow = 0;
    int firstLine = std::numeric_limits<int>::max();
    int lastLine = 0;
    int added = 0;
    int removed = 0;
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

QVector<DiffGroup> makeDiffGroups(const QVector<DiffLine> &rows)
{
    QVector<DiffGroup> groups;
    DiffGroup current;
    bool active = false;
    const auto finishGroup = [&] {
        if (active) groups.append(current);
        current = DiffGroup{};
        active = false;
    };
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const DiffLine &row = rows.at(rowIndex);
        const bool changed = row.leftKind != DiffLineKind::Unchanged || row.rightKind != DiffLineKind::Unchanged;
        if (!changed) {
            finishGroup();
            continue;
        }
        if (!active) {
            current.startRow = rowIndex;
            active = true;
        }
        current.endRow = rowIndex;
        if (row.leftKind == DiffLineKind::Removed) ++current.removed;
        if (row.rightKind == DiffLineKind::Added) ++current.added;
        for (int line : {row.leftNumber, row.rightNumber}) {
            if (line <= 0) continue;
            current.firstLine = qMin(current.firstLine, line);
            current.lastLine = qMax(current.lastLine, line);
        }
    }
    finishGroup();
    return groups;
}

QString diffGroupLabel(int index, const DiffGroup &group)
{
    const QString lineRange = group.firstLine == group.lastLine
        ? QStringLiteral("line %1").arg(group.firstLine)
        : QStringLiteral("lines %1–%2").arg(group.firstLine).arg(group.lastLine);
    return QStringLiteral("Change %1 · %2 (+%3/−%4)")
        .arg(index + 1).arg(lineRange).arg(group.added).arg(group.removed);
}

void populateChangeGroups(QListWidget *list, const QVector<DiffLine> &rows)
{
    list->clear();
    const QVector<DiffGroup> groups = makeDiffGroups(rows);
    for (int index = 0; index < groups.size(); ++index) {
        auto *item = new QListWidgetItem(diffGroupLabel(index, groups.at(index)), list);
        item->setData(Qt::UserRole, groups.at(index).startRow);
    }
    if (!groups.isEmpty())
        list->setCurrentRow(0);
}

void sizeFileListToRows(QListWidget *list)
{
    const int rowHeight = qMax(22, list->sizeHintForRow(0));
    const int rowCount = qMax(1, list->count());
    list->setFixedHeight(qMin(180, rowCount * rowHeight + 8));
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

bool launchGitHubCliSetup(QWidget *parent)
{
    QString distroId;
    QFile osRelease(QStringLiteral("/etc/os-release"));
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(osRelease.readAll());
        const QRegularExpression idPattern(QStringLiteral("^ID=(?:\"([^\"]+)\"|'([^']+)'|([^\\n]+))$"), QRegularExpression::MultilineOption);
        const auto match = idPattern.match(content);
        if (match.hasMatch()) distroId = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        if (distroId.isEmpty()) distroId = match.captured(3).trimmed();
    }

    QString installCommands;
    if (distroId == QLatin1String("ubuntu") || distroId == QLatin1String("debian")
        || distroId == QLatin1String("linuxmint") || distroId == QLatin1String("pop")) {
        installCommands = QStringLiteral(
            "if ! command -v wget >/dev/null 2>&1; then apt-get update && apt-get install -y wget || exit $?; fi\n"
            "install -d -m 755 /etc/apt/keyrings\n"
            "tmp_key=$(mktemp) || exit $?\n"
            "wget -qO \"$tmp_key\" https://cli.github.com/packages/githubcli-archive-keyring.gpg || exit $?\n"
            "install -m 644 \"$tmp_key\" /etc/apt/keyrings/githubcli-archive-keyring.gpg; result=$?; rm -f \"$tmp_key\"; [ $result -eq 0 ] || exit $result\n"
            "install -d -m 755 /etc/apt/sources.list.d\n"
            "echo \"deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main\" > /etc/apt/sources.list.d/github-cli.list || exit $?\n"
            "apt-get update && apt-get install -y gh");
    } else if (distroId == QLatin1String("fedora")) {
        installCommands = QStringLiteral("dnf install -y gh");
    } else if (distroId.startsWith(QLatin1String("opensuse")) || distroId == QLatin1String("sles")) {
        installCommands = QStringLiteral(
            "zypper addrepo -f https://cli.github.com/packages/rpm/gh-cli.repo github-cli || zypper modifyrepo -e github-cli; "
            "zypper --gpg-auto-import-keys refresh github-cli && zypper --non-interactive install -y gh");
    } else if (distroId == QLatin1String("arch") || distroId == QLatin1String("manjaro")
               || distroId == QLatin1String("endeavouros")) {
        installCommands = QStringLiteral("pacman -S --needed --noconfirm github-cli");
    }

    if (installCommands.isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Install GitHub CLI"),
            QStringLiteral("Automatic installation is not configured for this Linux distribution. Open GitHub’s official instructions to install gh, then choose Create Repository on Remote again."));
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/cli/cli/blob/trunk/docs/install_linux.md")));
        return false;
    }

    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Polkit is required"),
            QStringLiteral("This distribution does not provide pkexec. Install GitHub CLI with your package manager, then try again; the sign-in step can run inside Shell Scratchpad."));
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/cli/cli/blob/trunk/docs/install_linux.md")));
        return false;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Set up GitHub CLI"));
    dialog.setMinimumSize(620, 390);
    auto *layout = new QVBoxLayout(&dialog);
    auto *status = new QLabel(QStringLiteral("Installing GitHub CLI. Your desktop will ask you to authorize package installation."), &dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *output = new QPlainTextEdit(&dialog);
    output->setReadOnly(true);
    output->setMaximumBlockCount(2000);
    output->setPlaceholderText(QStringLiteral("Package manager and GitHub sign-in output will appear here."));
    layout->addWidget(output, 1);
    auto *progress = new QProgressBar(&dialog);
    progress->setRange(0, 0);
    layout->addWidget(progress);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QProcess process(&dialog);
    int phase = 0;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &dialog, [&] {
        const QString text = QString::fromLocal8Bit(process.readAllStandardOutput());
        static const QRegularExpression deviceCode(QStringLiteral("\\b([A-Z0-9]{4}-[A-Z0-9]{4})\\b"));
        const auto code = deviceCode.match(text);
        if (phase == 1 && code.hasMatch()) QApplication::clipboard()->setText(code.captured(1));
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        output->ensureCursorVisible();
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &dialog, [&] {
        const QString text = QString::fromLocal8Bit(process.readAllStandardError());
        static const QRegularExpression deviceCode(QStringLiteral("\\b([A-Z0-9]{4}-[A-Z0-9]{4})\\b"));
        const auto code = deviceCode.match(text);
        if (phase == 1 && code.hasMatch()) QApplication::clipboard()->setText(code.captured(1));
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(text);
        output->ensureCursorVisible();
    });
    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
        [&](int exitCode, QProcess::ExitStatus exitStatus) {
            if (phase == 0 && exitStatus == QProcess::NormalExit && exitCode == 0) {
                phase = 1;
                status->setText(QStringLiteral("GitHub CLI installed. Sign in at github.com/login/device; the one-time code will be copied to your clipboard."));
                output->appendPlainText(QStringLiteral("\nStarting GitHub browser sign-in…\n"));
                progress->setFormat(QStringLiteral("Waiting for GitHub sign-in…"));
                process.setProgram(QStandardPaths::findExecutable(QStringLiteral("gh")));
                process.setArguments({QStringLiteral("auth"), QStringLiteral("login"), QStringLiteral("--hostname"),
                    QStringLiteral("github.com"), QStringLiteral("--git-protocol"), QStringLiteral("https"),
                    QStringLiteral("--web"), QStringLiteral("--clipboard")});
                QProcessEnvironment authEnvironment = QProcessEnvironment::systemEnvironment();
                const QString noBrowser = QStandardPaths::findExecutable(QStringLiteral("true"));
                if (!noBrowser.isEmpty()) authEnvironment.insert(QStringLiteral("GH_BROWSER"), noBrowser);
                process.setProcessEnvironment(authEnvironment);
                process.start();
                const QUrl deviceLoginUrl(QStringLiteral("https://github.com/login/device"));
                if (!QDesktopServices::openUrl(deviceLoginUrl))
                    output->appendPlainText(QStringLiteral("Could not open the browser automatically. Open this link to finish sign-in: %1\n")
                        .arg(deviceLoginUrl.toString()));
                return;
            }
            progress->hide();
            buttons->clear();
            buttons->addButton(QDialogButtonBox::Close);
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                status->setText(QStringLiteral("GitHub sign-in completed. Continuing repository creation…"));
                output->appendPlainText(QStringLiteral("\nSigned in to GitHub successfully. Continuing in Shell Scratchpad."));
                if (parent) {
                    QWidget *appWindow = parent->window();
                    appWindow->show();
                    appWindow->raise();
                    appWindow->activateWindow();
                }
                dialog.accept();
            } else {
                status->setText(phase == 0 ? QStringLiteral("Installation failed. Review the package-manager output and try again.")
                                           : QStringLiteral("GitHub sign-in did not complete. Review the output and try again."));
            }
        });
    QObject::connect(&process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        progress->hide();
        buttons->clear();
        buttons->addButton(QDialogButtonBox::Close);
        status->setText(QStringLiteral("Could not start the setup command: %1").arg(process.errorString()));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, [&] {
        if (process.state() != QProcess::NotRunning) process.terminate();
        dialog.reject();
    });
    QObject::connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton *button) {
        if (buttons->standardButton(button) == QDialogButtonBox::Close) dialog.accept();
    });
    process.setProgram(pkexec);
    process.setArguments({QStringLiteral("bash"), QStringLiteral("-c"), installCommands});
    process.start();
    dialog.exec();
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
    return phase == 1 && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QIcon gitIcon(const QString &name)
{
    return QIcon(QStringLiteral(":/git/icons/%1.svg").arg(name));
}

bool isSvgDocument(const QByteArray &bytes)
{
    return bytes.left(8192).toLower().contains("<svg");
}

bool isImageFilePath(const QString &path, const QByteArray &contents = {})
{
    const QByteArray suffix = QFileInfo(path).suffix().toLatin1().toLower();
    if (suffix == "svg" || suffix == "svgz" || isSvgDocument(contents)) return true;
    const QList<QByteArray> supportedFormats = QImageReader::supportedImageFormats();
    if (std::any_of(supportedFormats.cbegin(), supportedFormats.cend(), [&suffix](const QByteArray &format) {
        return format.toLower() == suffix;
    })) return true;
    QBuffer buffer;
    buffer.setData(contents);
    return buffer.open(QIODevice::ReadOnly) && !QImageReader::imageFormat(&buffer).isEmpty();
}

QImage decodeImage(const QByteArray &bytes, const QString &path, QString *errorMessage = nullptr)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (buffer.open(QIODevice::ReadOnly)) {
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        if (!image.isNull()) return image;
    }

    if (QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) != 0 && !isSvgDocument(bytes)) {
        if (errorMessage) *errorMessage = QStringLiteral("This image format could not be decoded.");
        return {};
    }

    QTemporaryFile svgFile;
    if (!svgFile.open() || svgFile.write(bytes) != bytes.size() || !svgFile.flush()) {
        if (errorMessage) *errorMessage = QStringLiteral("The SVG data could not be prepared for rendering.");
        return {};
    }
    const QString magick = QStandardPaths::findExecutable(QStringLiteral("magick"));
    const QString convert = magick.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("convert")) : magick;
    if (convert.isEmpty()) {
        if (errorMessage) *errorMessage = QStringLiteral("SVG rendering is unavailable because no SVG image reader is installed.");
        return {};
    }
    QProcess renderer;
    renderer.setProgram(convert);
    renderer.setArguments({QStringLiteral("svg:") + svgFile.fileName(), QStringLiteral("png:-")});
    renderer.start();
    if (!renderer.waitForFinished(15000) || renderer.exitCode() != 0) {
        if (errorMessage) *errorMessage = QStringLiteral("SVG rendering failed: %1").arg(QString::fromLocal8Bit(renderer.readAllStandardError()).trimmed());
        return {};
    }
    const QImage image = QImage::fromData(renderer.readAllStandardOutput(), "PNG");
    if (image.isNull() && errorMessage) *errorMessage = QStringLiteral("The SVG renderer returned no image.");
    return image;
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
        progressBar = new QProgressBar(this);
        progressBar->setRange(0, 0);
        progressBar->setTextVisible(true);
        progressBar->setFormat(QStringLiteral("Cloning…"));
        progressBar->hide();
        layout->addWidget(progressBar);
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
            const QString text = QString::fromLocal8Bit(process->readAllStandardError());
            updateProgress(text);
            output->moveCursor(QTextCursor::End);
            output->insertPlainText(text);
            output->ensureCursorVisible();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) {
            if (fetchingRefs) {
                fetchingRefs = false;
                progressBar->hide();
                progressBar->hide();
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
            progressBar->hide();
            repositoryUrl->setEnabled(true);
            destinationName->setEnabled(true);
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                clonedPath = QDir(parentDirectory).filePath(destinationName->text().trimmed());
                output->appendPlainText(QStringLiteral("\nRefreshing remote branches…"));
                fetchingRefs = true;
                progressBar->setFormat(QStringLiteral("Refreshing remote branches…"));
                progressBar->setRange(0, 0);
                progressBar->show();
                process->setWorkingDirectory(clonedPath);
                process->setProgram(QStringLiteral("git"));
                process->setArguments({QStringLiteral("fetch"), QStringLiteral("--progress"), QStringLiteral("--all"), QStringLiteral("--prune")});
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
            progressBar->hide();
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
    QProgressBar *progressBar = nullptr;

    void updateProgress(const QString &text)
    {
        static const QRegularExpression percentage(QStringLiteral("(\\d{1,3})%"));
        auto matches = percentage.globalMatch(text);
        while (matches.hasNext()) {
            const int value = qBound(0, matches.next().captured(1).toInt(), 100);
            progressBar->setRange(0, 100);
            progressBar->setValue(value);
            progressBar->setFormat(fetchingRefs ? QStringLiteral("Refreshing remote branches… %p%")
                                                 : QStringLiteral("Cloning… %p%"));
            progressBar->show();
        }
    }

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
        progressBar->setRange(0, 0);
        progressBar->setFormat(QStringLiteral("Cloning…"));
        progressBar->show();
        repositoryUrl->setEnabled(false);
        destinationName->setEnabled(false);
        output->appendPlainText(QStringLiteral("Cloning into %1…\n").arg(destination));
        process->setProgram(QStringLiteral("git"));
        process->setArguments({QStringLiteral("clone"), QStringLiteral("--progress"), url, destination});
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

bool GitWorkspaceWindow::showInitDialog(const QString &directory, QWidget *parent)
{
    const QString path = QDir(directory).absolutePath();
    if (!QFileInfo(path).isDir()) {
        QMessageBox::warning(parent, QStringLiteral("Folder unavailable"),
                             QStringLiteral("The selected folder does not exist."));
        return false;
    }
    QProcess rootCheck;
    rootCheck.setProgram(gitExecutable());
    rootCheck.setArguments({QStringLiteral("-C"), path, QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    rootCheck.start();
    const bool alreadyRepositoryRoot = rootCheck.waitForFinished(2000) && rootCheck.exitCode() == 0
        && QFileInfo(QString::fromLocal8Bit(rootCheck.readAllStandardOutput()).trimmed()).canonicalFilePath()
            == QFileInfo(path).canonicalFilePath();
    if (alreadyRepositoryRoot) {
        QMessageBox::information(parent, QStringLiteral("Already a repository"),
                                 QStringLiteral("This folder is already inside a Git repository."));
        return false;
    }
    if (QMessageBox::question(parent, QStringLiteral("Create Git repository"),
            QStringLiteral("Initialize a new Git repository in %1?").arg(path),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes)
        return false;
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("-C"), path, QStringLiteral("init")});
    process.start();
    if (!process.waitForFinished(-1) || process.exitCode() != 0) {
        QMessageBox::critical(parent, QStringLiteral("Could not initialize repository"),
            QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return false;
    }
    auto *window = new GitWorkspaceWindow(path);
    window->show();
    window->raise();
    window->activateWindow();
    return true;
}

GitWorkspaceWindow::GitWorkspaceWindow(const QString &repositoryPath, QWidget *parent)
    : QMainWindow(parent), repoPath(RepositoryWorkspaceStore::repositoryRoot(repositoryPath))
{
    if (repoPath.isEmpty()) repoPath = QDir(repositoryPath).absolutePath();
    const QVector<RepositoryWorkspace> workspaceMatches = RepositoryWorkspaceStore::workspacesForRepository(repoPath);
    if (workspaceMatches.size() == 1) {
        workspace = workspaceMatches.first();
    } else if (workspaceMatches.size() > 1) {
        QStringList choices;
        for (const RepositoryWorkspace &candidate : workspaceMatches) {
            QStringList memberNames;
            for (const WorkspaceRepository &entry : candidate.repositories)
                memberNames.append(QFileInfo(entry.path).fileName());
            choices.append(QStringLiteral("%1 — %2").arg(candidate.id, memberNames.join(QStringLiteral(", "))));
        }
        bool accepted = false;
        const QString selected = QInputDialog::getItem(this, QStringLiteral("Choose Repository Workspace"),
            QStringLiteral("This repository appears in more than one configured workspace:"), choices, 0, false, &accepted);
        const int selectedIndex = accepted ? choices.indexOf(selected) : -1;
        if (selectedIndex >= 0) workspace = workspaceMatches.at(selectedIndex);
        else workspaceAmbiguous = true;
    } else {
        workspace = RepositoryWorkspaceStore::workspaceForRepository(repoPath, true);
    }
    for (const WorkspaceRepository &repository : workspace.repositories) {
        if (QFileInfo(repository.path).canonicalFilePath() == QFileInfo(repoPath).canonicalFilePath()) {
            repoPath = repository.path;
            break;
        }
    }
    recordRecentWorkspace();
    lastRepositoryFocusTimer = new QTimer(this);
    lastRepositoryFocusTimer->setSingleShot(true);
    lastRepositoryFocusTimer->setInterval(5000);
    connect(lastRepositoryFocusTimer, &QTimer::timeout, this, [this] {
        if (isActiveWindow())
            QSettings().setValue(QStringLiteral("gitWorkspace/lastRepository"), repoPath);
    });
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
        const auto branchLeaves = branchesList->findItems(QString(), Qt::MatchContains | Qt::MatchRecursive, 0);
        for (QTreeWidgetItem *item : branchLeaves) {
            if (!item->data(0, Qt::UserRole + 3).toBool()) continue;
            const QString name = item->data(0, Qt::UserRole).toString();
            const bool remote = item->data(0, Qt::UserRole + 1).toBool();
            const bool current = item->data(0, Qt::UserRole + 2).toBool();
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
        if (actionsTargetOnlyActiveRepository()) showCommitDialog();
        else showBatchOperations(QStringLiteral("commit"));
    });
    stageAllButton = new QToolButton(root);
    stageAllButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    stageAllButton->setIcon(gitIcon(QStringLiteral("stage")));
    stageAllButton->setText(QStringLiteral("Stage All"));
    stageAllButton->setToolTip(QStringLiteral("Stage all modified, deleted, and untracked files"));
    stageAllButton->setVisible(false);
    connect(stageAllButton, &QToolButton::clicked, this, [this] {
        if (actionsTargetOnlyActiveRepository()) runGit({QStringLiteral("add"), QStringLiteral("-A")});
        else showBatchOperations(QStringLiteral("stage"));
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
        if (workspace.repositories.size() > 1 && !workspaceAmbiguous) {
            QAction *workspaceSync = syncMenu->addAction(QStringLiteral("Synchronize selected repositories…"));
            connect(workspaceSync, &QAction::triggered, this, [this] { showBatchOperations(QStringLiteral("sync")); });
            syncMenu->addSeparator();
        }
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
        }
    });
    pullButton = new QToolButton(root);
    pullButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pullButton->setIcon(gitIcon(QStringLiteral("pull")));
    pullButton->setText(QStringLiteral("Pull"));
    pullButton->setToolTip(QStringLiteral("Pull changes from a remote branch"));
    connect(pullButton, &QToolButton::clicked, this, [this] {
        if (actionsTargetOnlyActiveRepository()) pullCurrentBranch();
        else showBatchOperations(QStringLiteral("pull"));
    });
    pushButton = new QToolButton(root);
    pushButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pushButton->setIcon(gitIcon(QStringLiteral("push")));
    pushButton->setText(QStringLiteral("Push"));
    pushButton->setToolTip(QStringLiteral("Push local commits to a remote branch"));
    connect(pushButton, &QToolButton::clicked, this, [this] {
        if (actionsTargetOnlyActiveRepository()) pushCurrentBranch();
        else showBatchOperations(QStringLiteral("push"));
    });
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
        if (hasRemote && !hasUnpushedCommits) {
            QAction *push = moreMenu->addAction(QStringLiteral("Push"));
            push->setEnabled(!gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")}).isEmpty());
            push->setToolTip(QStringLiteral("Push the current branch; Git will report if it is already up to date"));
            connect(push, &QAction::triggered, this, [this] { pushCurrentBranch(); });
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
        stash->setIcon(gitIcon(QStringLiteral("stash")));
        stash->setEnabled(hasStagedChanges || hasUnstagedChanges);
        connect(stash, &QAction::triggered, this, [this] {
            runGit({QStringLiteral("stash"), QStringLiteral("push")});
        });
        QAction *applyStash = moreMenu->addAction(gitIcon(QStringLiteral("stash-apply")), QStringLiteral("Apply Stash Options…"));
        applyStash->setEnabled(hasStashes);
        connect(applyStash, &QAction::triggered, this, [this] { if (applyStashButton) applyStashButton->showMenu(); });
    });
    applyStashButton = new QToolButton(root);
    applyStashButton->setObjectName(QStringLiteral("applyStashButton"));
    applyStashButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    applyStashButton->setText(QStringLiteral("Apply Stash"));
    applyStashButton->setIcon(gitIcon(QStringLiteral("stash-apply")));
    applyStashButton->setToolTip(QStringLiteral("Apply or pop a stash in the active repository"));
    applyStashButton->setPopupMode(QToolButton::InstantPopup);
    auto *applyStashMenu = new QMenu(applyStashButton);
    applyStashButton->setMenu(applyStashMenu);
    connect(applyStashMenu, &QMenu::aboutToShow, this, [this, applyStashMenu] {
        applyStashMenu->clear();
        const QStringList stashLines = gitOutput({QStringLiteral("stash"), QStringLiteral("list"),
            QStringLiteral("--format=%gd%x09%gs")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (stashLines.isEmpty()) {
            QAction *empty = applyStashMenu->addAction(QStringLiteral("No stashes in this repository"));
            empty->setEnabled(false);
            return;
        }
        for (const QString &line : stashLines) {
            const QStringList fields = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
            if (fields.isEmpty()) continue;
            const QString reference = fields.first();
            const QString description = fields.size() > 1 ? fields.mid(1).join(QLatin1Char('\t')) : QString();
            QMenu *stashOptions = applyStashMenu->addMenu(QStringLiteral("%1 — %2").arg(reference, description));
            QAction *apply = stashOptions->addAction(QStringLiteral("Apply (keep stash)"));
            connect(apply, &QAction::triggered, this, [this, reference] {
                runGit({QStringLiteral("stash"), QStringLiteral("apply"), reference});
            });
            QAction *pop = stashOptions->addAction(QStringLiteral("Pop (apply and remove)"));
            connect(pop, &QAction::triggered, this, [this, reference] {
                runGit({QStringLiteral("stash"), QStringLiteral("pop"), reference});
            });
        }
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
    actionRepositoriesButton = new QToolButton(root);
    actionRepositoriesButton->setObjectName(QStringLiteral("actionRepositoriesButton"));
    actionRepositoriesButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    actionRepositoriesButton->setPopupMode(QToolButton::InstantPopup);
    actionRepositoriesButton->setText(QStringLiteral("Action Repositories"));
    actionRepositoriesButton->setToolTip(QStringLiteral("Choose which repositories workspace actions target"));
    auto *actionRepositoriesMenu = new QMenu(actionRepositoriesButton);
    actionRepositoriesButton->setMenu(actionRepositoriesMenu);
    connect(actionRepositoriesMenu, &QMenu::aboutToShow, this, [this, actionRepositoriesMenu] {
        actionRepositoriesMenu->clear();
        QStringList selectedIds = workspace.actionRepositoryIds;
        if (selectedIds.isEmpty())
            for (const WorkspaceRepository &repository : workspace.repositories) selectedIds.append(repository.id);
        for (const WorkspaceRepository &repository : workspace.repositories) {
            QAction *action = actionRepositoriesMenu->addAction(QFileInfo(repository.path).fileName());
            action->setCheckable(true);
            action->setChecked(selectedIds.contains(repository.id));
            action->setData(repository.id);
            action->setToolTip(repository.path);
            connect(action, &QAction::triggered, this, [this, actionRepositoriesMenu, action] {
                QStringList selected;
                for (QAction *candidate : actionRepositoriesMenu->actions())
                    if (candidate->isCheckable() && candidate->isChecked()) selected.append(candidate->data().toString());
                if (selected.isEmpty()) {
                    action->setChecked(true);
                    statusBar()->showMessage(QStringLiteral("Select at least one repository for actions."), 4000);
                    return;
                }
                workspace.actionRepositoryIds = selected;
                RepositoryWorkspaceStore::saveWorkspace(workspace);
                updateActionRepositoriesButton();
            });
        }
    });
    updateActionRepositoriesButton();
    toolbar->addWidget(branchButton);
    toolbar->addWidget(commitButton);
    toolbar->addWidget(stageAllButton);
    toolbar->addWidget(syncButton);
    toolbar->addWidget(pullButton);
    toolbar->addWidget(pushButton);
    toolbar->addWidget(moreButton);
    toolbar->addWidget(applyStashButton);
    toolbar->addWidget(conflictsButton);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(actionRepositoriesButton);
    toolbar->addStretch(1);
    toolbar->addWidget(new QLabel(QDir(repoPath).dirName(), root));
    auto *openRepositoryFolderButton = new QToolButton(root);
    openRepositoryFolderButton->setObjectName(QStringLiteral("openRepositoryFolderButton"));
    openRepositoryFolderButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    openRepositoryFolderButton->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
    openRepositoryFolderButton->setText(QStringLiteral("Open Folder"));
    openRepositoryFolderButton->setToolTip(QStringLiteral("Open this repository folder in the default file manager"));
    connect(openRepositoryFolderButton, &QToolButton::clicked, this, [this] {
        const QUrl folderUrl = QUrl::fromLocalFile(QDir(repoPath).absolutePath());
        if (!QDesktopServices::openUrl(folderUrl)) {
            QMessageBox::warning(this, QStringLiteral("Could not open repository folder"),
                QStringLiteral("The default file manager could not open:\n%1").arg(repoPath));
        }
    });
    toolbar->addWidget(openRepositoryFolderButton);
    layout->addLayout(toolbar);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, root);
    auto *branchTabs = new QTabWidget(mainSplitter);
    branchesList = new QTreeWidget(branchTabs);
    branchesList->setHeaderHidden(true);
    branchTabs->addTab(branchesList, QStringLiteral("Branches"));
    auto *workspaceRepositoryPanel = new QWidget(branchTabs);
    auto *workspaceRepositoryLayout = new QVBoxLayout(workspaceRepositoryPanel);
    workspaceRepositoryLayout->setContentsMargins(4, 4, 4, 4);
    workspaceRepositoryLayout->addWidget(new QLabel(QStringLiteral("Repositories in this workspace"), workspaceRepositoryPanel));
    workspaceRepositoryList = new QTreeWidget(workspaceRepositoryPanel);
    workspaceRepositoryList->setHeaderLabels({QStringLiteral("Repository"), QStringLiteral("Status")});
    workspaceRepositoryList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    workspaceRepositoryList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workspaceRepositoryLayout->addWidget(workspaceRepositoryList, 1);
    auto *workspaceButtons = new QGridLayout;
    auto *addWorkspaceRepositoryButton = new QPushButton(QStringLiteral("Add…"), workspaceRepositoryPanel);
    auto *removeWorkspaceRepositoryButton = new QPushButton(QStringLiteral("Remove"), workspaceRepositoryPanel);
    auto *locateWorkspaceRepositoryButton = new QPushButton(QStringLiteral("Locate…"), workspaceRepositoryPanel);
    auto *batchOperationsButton = new QPushButton(QStringLiteral("Batch Operations…"), workspaceRepositoryPanel);
    workspaceButtons->addWidget(addWorkspaceRepositoryButton, 0, 0);
    workspaceButtons->addWidget(removeWorkspaceRepositoryButton, 0, 1);
    workspaceButtons->addWidget(locateWorkspaceRepositoryButton, 1, 0);
    workspaceButtons->addWidget(batchOperationsButton, 1, 1);
    workspaceRepositoryLayout->addLayout(workspaceButtons);
    branchTabs->addTab(workspaceRepositoryPanel, QStringLiteral("Repositories"));
    connect(addWorkspaceRepositoryButton, &QPushButton::clicked, this, [this] { addWorkspaceRepository(); });
    connect(removeWorkspaceRepositoryButton, &QPushButton::clicked, this, [this] { removeWorkspaceRepository(); });
    connect(locateWorkspaceRepositoryButton, &QPushButton::clicked, this, [this] { locateWorkspaceRepository(); });
    connect(batchOperationsButton, &QPushButton::clicked, this, [this] { showBatchOperations(); });
    connect(workspaceRepositoryList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item) switchRepository(item->data(0, Qt::UserRole).toString());
    });
    refreshWorkspaceRepositoryList();
    if (workspaceAmbiguous) {
        addWorkspaceRepositoryButton->setEnabled(false);
        removeWorkspaceRepositoryButton->setEnabled(false);
        locateWorkspaceRepositoryButton->setEnabled(false);
        batchOperationsButton->setEnabled(false);
    }
    connect(workspaceRepositoryList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item) switchRepository(item->data(0, Qt::UserRole).toString());
    });
    connect(branchTabs, &QTabWidget::currentChanged, this, [this, branchTabs](int index) {
        if (branchTabs->widget(index) == workspaceRepositoryList->parentWidget()) refreshWorkspaceRepositoryList();
    });
    branchTabs->setMinimumWidth(220);

    auto *leftTabs = new QTabWidget(mainSplitter);
    changesTree = new QTreeWidget(leftTabs);
    changesTree->setHeaderLabels({QStringLiteral("Working tree changes"), QStringLiteral("Status")});
    changesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    changesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    leftTabs->addTab(changesTree, QStringLiteral("Changes"));
    auto *historyPanel = new QWidget(leftTabs);
    auto *historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(4, 4, 4, 4);
    historyOverviewButton = new QPushButton(QStringLiteral("Show graph"), historyPanel);
    historyOverviewButton->setObjectName(QStringLiteral("historyOverviewButton"));
    historyLayout->addWidget(historyOverviewButton);
    historyViews = new QStackedWidget(historyPanel);
    historyList = new QListWidget(historyViews);
    historyViews->addWidget(historyList);
    auto *graphScroll = new QScrollArea(historyViews);
    graphScroll->setWidgetResizable(true);
    historyGraph = new HistoryGraphWidget;
    graphScroll->setWidget(historyGraph);
    historyViews->addWidget(graphScroll);
    historyLayout->addWidget(historyViews, 1);
    connect(historyOverviewButton, &QPushButton::clicked, this, [this] { showHistoryOverview(); });
    leftTabs->addTab(historyPanel, QStringLiteral("History"));
    leftTabs->setMinimumWidth(280);

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
    commitMessageView = new QPlainTextEdit(diffContent);
    commitMessageView->setReadOnly(true);
    commitMessageView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    commitMessageView->setMaximumHeight(150);
    commitMessageView->hide();
    diffContentLayout->addWidget(commitMessageView);
    diffSummaryLabel = new QLabel(diffContent);
    diffContentLayout->addWidget(diffSummaryLabel);
    changeGroupsList = new QListWidget;
    changeGroupsList->setSelectionMode(QAbstractItemView::SingleSelection);
    changeGroupsList->setToolTip(QStringLiteral("Select a change group to jump to it in the diff"));
    connect(changeGroupsList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item) return;
        const int targetRow = item->data(Qt::UserRole).toInt();
        for (QTextEdit *view : {originalFileView, workingFileView}) {
            QTextCursor cursor(view->document());
            cursor.movePosition(QTextCursor::Start);
            for (int row = 0; row < targetRow; ++row)
                cursor.movePosition(QTextCursor::NextBlock);
            view->setTextCursor(cursor);
            view->ensureCursorVisible();
        }
    });
    auto *diffLabels = new QHBoxLayout;
    originalDiffLabel = new QLabel(QStringLiteral("Original"), diffContent);
    changedDiffLabel = new QLabel(QStringLiteral("Changed"), diffContent);
    diffLabels->addWidget(originalDiffLabel);
    diffLabels->addWidget(changedDiffLabel);
    diffContentLayout->addLayout(diffLabels);
    svgViewModePanel = new QWidget(diffContent);
    auto *svgViewModeLayout = new QHBoxLayout(svgViewModePanel);
    svgViewModeLayout->setContentsMargins(0, 0, 0, 0);
    svgViewModeLayout->addWidget(new QLabel(QStringLiteral("SVG view:"), svgViewModePanel));
    svgViewModeCombo = new QComboBox(svgViewModePanel);
    svgViewModeCombo->setObjectName(QStringLiteral("svgViewModeCombo"));
    svgViewModeCombo->addItem(QStringLiteral("Rendered image"));
    svgViewModeCombo->addItem(QStringLiteral("Source code"));
    svgViewModeLayout->addWidget(svgViewModeCombo);
    svgViewModeLayout->addStretch(1);
    svgViewModePanel->hide();
    diffContentLayout->addWidget(svgViewModePanel);
    connect(svgViewModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (currentDiffIsSvg) displayImageDiff();
    });
    auto *diffColumns = new QSplitter(Qt::Horizontal, diffContent);
    originalViewStack = new QStackedWidget(diffColumns);
    workingViewStack = new QStackedWidget(diffColumns);
    originalFileView = new QTextEdit(originalViewStack);
    workingFileView = new QTextEdit(workingViewStack);
    originalImageView = new ImagePreviewLabel(originalViewStack);
    workingImageView = new ImagePreviewLabel(workingViewStack);
    originalViewStack->addWidget(originalFileView);
    originalViewStack->addWidget(originalImageView);
    workingViewStack->addWidget(workingFileView);
    workingViewStack->addWidget(workingImageView);
    for (QTextEdit *view : {originalFileView, workingFileView}) {
        view->setReadOnly(true);
        view->setLineWrapMode(QTextEdit::NoWrap);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }
    diffColumns->addWidget(originalViewStack);
    diffColumns->addWidget(workingViewStack);
    synchronizeDiffViews(originalFileView, workingFileView);
    diffContentLayout->addWidget(diffColumns, 1);
    auto *diffNavigation = new QWidget(diffPanel);
    auto *diffNavigationLayout = new QVBoxLayout(diffNavigation);
    diffNavigationLayout->setContentsMargins(0, 0, 0, 0);
    diffNavigationLayout->setSpacing(4);
    diffNavigationLayout->addWidget(diffFilesList);
    diffNavigationLayout->addWidget(changeGroupsList, 1);
    auto *diffOuterSplitter = new QSplitter(Qt::Horizontal, diffPanel);
    diffOuterSplitter->addWidget(diffNavigation);
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

    commandPanel = new QWidget(rightTabs);
    auto *commandLayout = new QVBoxLayout(commandPanel);
    commandLayout->setContentsMargins(4, 4, 4, 4);
    commandLayout->addWidget(new QLabel(QStringLiteral("Run a Git command in this repository. Suggestions include subcommands and common options."), commandPanel));
    commandEditor = new CodeEditor(commandPanel);
    commandEditor->setMaximumBlockCount(2);
    commandEditor->setFixedHeight(72);
    commandEditor->setPlaceholderText(QStringLiteral("status --short --branch"));
    commandEditor->setPlainText(QStringLiteral("status --short --branch"));
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

    remotesPanel = new QWidget(rightTabs);
    auto *remotesLayout = new QVBoxLayout(remotesPanel);
    remotesLayout->addWidget(new QLabel(QStringLiteral("Manage any number of Git remotes for this repository."), remotesPanel));
    remotesList = new QListWidget(remotesPanel);
    remotesList->setContextMenuPolicy(Qt::CustomContextMenu);
    remotesLayout->addWidget(remotesList, 1);
    auto *remoteActions = new QHBoxLayout;
    auto *addRemoteButton = new QPushButton(QStringLiteral("Add Remote…"), remotesPanel);
    auto *openRemoteButton = new QPushButton(QStringLiteral("Open in Browser"), remotesPanel);
    auto *editRemoteButton = new QPushButton(QStringLiteral("Change URL…"), remotesPanel);
    auto *removeRemoteButton = new QPushButton(QStringLiteral("Remove Remote…"), remotesPanel);
    auto *createRepositoryButton = new QPushButton(QStringLiteral("Create Repository on Remote…"), remotesPanel);
    remoteActions->addWidget(addRemoteButton);
    remoteActions->addWidget(openRemoteButton);
    remoteActions->addWidget(editRemoteButton);
    remoteActions->addWidget(removeRemoteButton);
    remoteActions->addWidget(createRepositoryButton);
    remoteActions->addStretch(1);
    remotesLayout->addLayout(remoteActions);
    rightTabs->addTab(commandPanel, QStringLiteral("Git Command"));
    rightTabs->addTab(remotesPanel, QStringLiteral("Remotes"));
    connect(addRemoteButton, &QPushButton::clicked, this, [this] { addRemote(); });
    openRemoteButton->setEnabled(false);
    connect(remotesList, &QListWidget::currentItemChanged, this, [openRemoteButton](QListWidgetItem *current) {
        openRemoteButton->setEnabled(current != nullptr);
    });
    connect(openRemoteButton, &QPushButton::clicked, this, [this] {
        if (remotesList->currentItem()) openRemoteInBrowser(remotesList->currentItem()->data(Qt::UserRole).toString());
    });
    connect(createRepositoryButton, &QPushButton::clicked, this, [this] { createRemoteRepository(); });
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
        QAction *open = menu.addAction(QStringLiteral("Open in Browser"));
        connect(open, &QAction::triggered, this, [this, name] { openRemoteInBrowser(name); });
        QAction *edit = menu.addAction(QStringLiteral("Change URL…"));
        connect(edit, &QAction::triggered, this, [this, name] { changeRemoteUrl(name); });
        QAction *remove = menu.addAction(QStringLiteral("Remove Remote…"));
        connect(remove, &QAction::triggered, this, [this, name] { removeRemote(name); });
        menu.exec(remotesList->viewport()->mapToGlobal(position));
    });

    credentialsPanel = new QWidget(rightTabs);
    credentialsPanel->hide();
    auto *credentialsForm = new QFormLayout(credentialsPanel);
    userNameEdit = new QLineEdit(credentialsPanel);
    emailEdit = new QLineEdit(credentialsPanel);
    credentialHelperCombo = new QComboBox(credentialsPanel);
    credentialHelperCombo->addItem(QStringLiteral("Keep current helper"), QString());
    credentialHelperCombo->addItem(QStringLiteral("Git Credential Manager"), QStringLiteral("manager"));
#ifndef Q_OS_WIN
    credentialHelperCombo->addItem(QStringLiteral("GNOME Keyring / libsecret"), QStringLiteral("/usr/lib/git-core/git-credential-libsecret"));
    credentialHelperCombo->addItem(QStringLiteral("KWallet"), QStringLiteral("kwallet"));
    credentialHelperCombo->addItem(QStringLiteral("Temporary in-memory cache"), QStringLiteral("cache --timeout=3600"));
#endif
    credentialHelperCombo->addItem(QStringLiteral("Plain-text store (not recommended)"), QStringLiteral("store"));
    credentialWalletEdit = new QLineEdit(QStringLiteral("git-credentials"), credentialsPanel);
    credentialWalletEdit->setPlaceholderText(QStringLiteral("git-credentials"));
    credentialFolderEdit = new QLineEdit(QStringLiteral("Git Credentials"), credentialsPanel);
    credentialFolderEdit->setPlaceholderText(QStringLiteral("Git Credentials"));
    createCredentialWalletButton = new QPushButton(QStringLiteral("Create / Open Wallet…"), credentialsPanel);
    credentialsForm->addRow(QStringLiteral("Commit name (global)"), userNameEdit);
    credentialsForm->addRow(QStringLiteral("Commit email (global)"), emailEdit);
    credentialsForm->addRow(QStringLiteral("Credential storage (global)"), credentialHelperCombo);
#ifndef Q_OS_WIN
    credentialsForm->addRow(QStringLiteral("KWallet wallet"), credentialWalletEdit);
    credentialsForm->addRow(QStringLiteral("KWallet folder"), credentialFolderEdit);
    credentialsForm->addRow(createCredentialWalletButton);
#endif
    auto *saveIdentityButton = new QPushButton(QStringLiteral("Save Git Identity"), credentialsPanel);
    auto *configureHelperButton = new QPushButton(QStringLiteral("Configure Helper"), credentialsPanel);
    auto *addCredentialButton = new QPushButton(QStringLiteral("Manage Access Tokens…"), credentialsPanel);
    auto *manageSshKeysButton = new QPushButton(QStringLiteral("Manage SSH Keys…"), credentialsPanel);
    credentialsForm->addRow(saveIdentityButton, configureHelperButton);
    credentialsForm->addRow(addCredentialButton);
    credentialsForm->addRow(manageSshKeysButton);
    credentialsForm->addRow(new QLabel(QStringLiteral("Credentials are passed to Git’s configured credential helper. The app does not save passwords in its own settings."), credentialsPanel));
    const auto updateWalletControls = [this] {
#ifdef Q_OS_WIN
        const bool visible = false;
#else
        const bool visible = credentialHelperCombo->currentData().toString() == QLatin1String("kwallet");
#endif
        credentialWalletEdit->setVisible(visible);
        credentialFolderEdit->setVisible(visible);
        createCredentialWalletButton->setVisible(visible);
    };
    connect(credentialHelperCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [updateWalletControls] { updateWalletControls(); });
    connect(createCredentialWalletButton, &QPushButton::clicked, this, [this] { createCredentialWallet(); });
    updateWalletControls();
    repositorySettingsPanel = new QWidget(rightTabs);
    repositorySettingsPanel->hide();
    auto *settingsLayout = new QVBoxLayout(repositorySettingsPanel);
    settingsLayout->addWidget(new QLabel(QStringLiteral("Common repository behavior settings. “Use inherited setting” removes this repository’s override. Git has many additional options; less common options remain available under Advanced."), repositorySettingsPanel));
    auto *curatedSettingsForm = new QFormLayout;
    for (const RepositorySettingDefinition &setting : repositorySettingDefinitions()) {
        auto *control = new QComboBox(repositorySettingsPanel);
        for (qsizetype i = 0; i < setting.labels.size(); ++i)
            control->addItem(setting.labels.at(i), setting.values.at(i));
        repositorySettingControls.insert(setting.key, control);
        curatedSettingsForm->addRow(setting.label, control);
    }
    settingsLayout->addLayout(curatedSettingsForm);
    applyRepositorySettingsButton = new QPushButton(QStringLiteral("Apply Repository Settings"), repositorySettingsPanel);
    settingsLayout->addWidget(applyRepositorySettingsButton, 0, Qt::AlignLeft);
    connect(applyRepositorySettingsButton, &QPushButton::clicked, this, [this] { applyRepositorySettings(); });
    settingsLayout->addWidget(new QLabel(QStringLiteral("Advanced repository settings"), repositorySettingsPanel));
    repositorySettingsTree = new QTreeWidget(repositorySettingsPanel);
    repositorySettingsTree->setHeaderLabels({QStringLiteral("Setting"), QStringLiteral("Value")});
    repositorySettingsTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    repositorySettingsTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    settingsLayout->addWidget(repositorySettingsTree, 1);
    auto *settingsActions = new QHBoxLayout;
    auto *addSettingButton = new QPushButton(QStringLiteral("Add Setting…"), repositorySettingsPanel);
    auto *editSettingButton = new QPushButton(QStringLiteral("Edit…"), repositorySettingsPanel);
    auto *removeSettingButton = new QPushButton(QStringLiteral("Remove"), repositorySettingsPanel);
    auto *refreshSettingsButton = new QPushButton(QStringLiteral("Refresh"), repositorySettingsPanel);
    settingsActions->addWidget(addSettingButton);
    settingsActions->addWidget(editSettingButton);
    settingsActions->addWidget(removeSettingButton);
    settingsActions->addStretch(1);
    settingsActions->addWidget(refreshSettingsButton);
    settingsLayout->addLayout(settingsActions);
    connect(addSettingButton, &QPushButton::clicked, this, [this] { addRepositorySetting(); });
    connect(editSettingButton, &QPushButton::clicked, this, [this] { editRepositorySetting(repositorySettingsTree->currentItem()); });
    connect(removeSettingButton, &QPushButton::clicked, this, [this] { removeRepositorySetting(repositorySettingsTree->currentItem()); });
    connect(refreshSettingsButton, &QPushButton::clicked, this, [this] { refreshRepositorySettings(); });
    connect(repositorySettingsTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) { editRepositorySetting(item); });
    refreshRepositorySettings();

    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *closeWorkspaceAction = fileMenu->addAction(QStringLiteral("Close Git Workspace"));
    closeWorkspaceAction->setShortcut(QKeySequence::Close);
    closeWorkspaceAction->setShortcutContext(Qt::WindowShortcut);
    connect(closeWorkspaceAction, &QAction::triggered, this, &QWidget::close);

    gitActionsMenu = menuBar()->addMenu(QStringLiteral("&Git"));
    const auto addGitAction = [this](const QString &label, const QString &shortcut,
                                                     const std::function<void()> &callback) {
        QAction *action = gitActionsMenu->addAction(label);
        action->setShortcut(QKeySequence::fromString(shortcut, QKeySequence::PortableText));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };
    stageAllShortcutAction = addGitAction(QStringLiteral("Stage All Changes"), QStringLiteral("Ctrl+Alt+A"), [this] {
        runGit({QStringLiteral("add"), QStringLiteral("-A")});
    });
    commitShortcutAction = addGitAction(QStringLiteral("Commit Staged Changes…"), QStringLiteral("Ctrl+Alt+C"), [this] {
        showCommitDialog();
    });
    gitActionsMenu->addSeparator();
    pushShortcutAction = addGitAction(QStringLiteral("Push"), QStringLiteral("Ctrl+Alt+P"), [this] { pushCurrentBranch(); });
    pullShortcutAction = addGitAction(QStringLiteral("Pull"), QStringLiteral("Ctrl+Alt+L"), [this] { pullCurrentBranch(); });
    fetchShortcutAction = addGitAction(QStringLiteral("Fetch All Remotes"), QStringLiteral("Ctrl+Alt+F"), [this] {
        runGit({QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")});
    });
    gitActionsMenu->addSeparator();
    stashShortcutAction = addGitAction(QStringLiteral("Stash Changes"), QStringLiteral("Ctrl+G, Ctrl+S"), [this] {
        runGit({QStringLiteral("stash"), QStringLiteral("push")});
    });
    stashShortcutAction->setIcon(gitIcon(QStringLiteral("stash")));
    QAction *applyStashShortcutAction = addGitAction(QStringLiteral("Apply Stash Options…"), QStringLiteral("Ctrl+G, Ctrl+U"), [this] {
        if (applyStashButton && hasStashes) applyStashButton->showMenu();
    });
    applyStashShortcutAction->setIcon(gitIcon(QStringLiteral("stash-apply")));
    addGitAction(QStringLiteral("Refresh Status"), QStringLiteral("Ctrl+Alt+R"), [this] { refreshAll(); });
    resolveShortcutAction = addGitAction(QStringLiteral("Resolve Conflicts"), QStringLiteral("Ctrl+G, Ctrl+X"), [this] {
        if (conflictsButton->isEnabled()) conflictsButton->click();
    });

    QMenu *navigateMenu = menuBar()->addMenu(QStringLiteral("&Navigate"));
    previousRightPage = detailsStack;
    const auto showRightPage = [this](QWidget *page, const QString &title, QWidget *focusWidget = nullptr) {
        if (rightTabs->indexOf(page) < 0) rightTabs->addTab(page, title);
        rightTabs->setCurrentWidget(page);
        page->show();
        previousRightPage = page;
        if (focusWidget) focusWidget->setFocus();
    };
    rightTabs->setTabsClosable(false);
    connect(rightTabs, &QTabWidget::currentChanged, this, [this](int index) {
        QWidget *newPage = index >= 0 ? rightTabs->widget(index) : nullptr;
        QWidget *oldPage = previousRightPage;
        previousRightPage = newPage;
        if (oldPage && oldPage != detailsStack && oldPage != commandPanel && oldPage != remotesPanel && oldPage != newPage) {
            const int oldIndex = rightTabs->indexOf(oldPage);
            if (oldIndex >= 0) {
                rightTabs->removeTab(oldIndex);
                oldPage->hide();
            }
        }
    });
    const auto addNavigationAction = [this, navigateMenu](const QString &label, const QString &shortcut,
                                                           const std::function<void()> &callback) {
        QAction *action = navigateMenu->addAction(label);
        action->setShortcut(QKeySequence::fromString(shortcut, QKeySequence::PortableText));
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, callback);
    };
    addNavigationAction(QStringLiteral("Changes"), QStringLiteral("Ctrl+G, Ctrl+C"), [this, leftTabs] {
        leftTabs->setCurrentIndex(0);
        changesTree->setFocus();
    });
    addNavigationAction(QStringLiteral("Branches"), QStringLiteral("Ctrl+G, Ctrl+B"), [this, branchTabs] {
        branchTabs->setCurrentIndex(0);
        branchesList->setFocus();
    });
    addNavigationAction(QStringLiteral("History"), QStringLiteral("Ctrl+G, Ctrl+H"), [this, leftTabs] {
        leftTabs->setCurrentIndex(1);
        historyList->setFocus();
    });
    addNavigationAction(QStringLiteral("Details / Diff"), QStringLiteral("Ctrl+G, Ctrl+D"), [this] {
        rightTabs->setCurrentWidget(detailsStack);
        detailsStack->setFocus();
    });
    addNavigationAction(QStringLiteral("Git Command"), QStringLiteral("Ctrl+G, Ctrl+T"), [this, showRightPage] {
        showRightPage(commandPanel, QStringLiteral("Git Command"), commandEditor);
    });
    addNavigationAction(QStringLiteral("Remotes"), QStringLiteral("Ctrl+G, Ctrl+R"), [this, showRightPage] {
        showRightPage(remotesPanel, QStringLiteral("Remotes"), remotesList);
    });
    addNavigationAction(QStringLiteral("Identity && Credentials"), QStringLiteral("Ctrl+G, Ctrl+I"), [this, showRightPage] {
        showRightPage(credentialsPanel, QStringLiteral("Identity && Credentials"), userNameEdit);
    });
    addNavigationAction(QStringLiteral("Repository Settings…"), QStringLiteral("Ctrl+G, Ctrl+E"), [this, showRightPage] {
        showRightPage(repositorySettingsPanel, QStringLiteral("Repository Settings"), repositorySettingsTree);
    });
    QMenu *workspaceMenu = menuBar()->addMenu(QStringLiteral("&Workspace"));
    QAction *commandMenuAction = workspaceMenu->addAction(QStringLiteral("Git Command"));
    connect(commandMenuAction, &QAction::triggered, this, [this] { rightTabs->setCurrentWidget(commandPanel); commandEditor->setFocus(); });
    QAction *remotesMenuAction = workspaceMenu->addAction(QStringLiteral("Remotes"));
    connect(remotesMenuAction, &QAction::triggered, this, [this] { rightTabs->setCurrentWidget(remotesPanel); remotesList->setFocus(); });
    workspaceMenu->addSeparator();
    QAction *identityMenuAction = workspaceMenu->addAction(QStringLiteral("Identity && Credentials"));
    connect(identityMenuAction, &QAction::triggered, this, [showRightPage, this] { showRightPage(credentialsPanel, QStringLiteral("Identity && Credentials"), userNameEdit); });
    QAction *settingsMenuAction = workspaceMenu->addAction(QStringLiteral("Repository Settings…"));
    connect(settingsMenuAction, &QAction::triggered, this, [showRightPage, this] { showRightPage(repositorySettingsPanel, QStringLiteral("Repository Settings"), repositorySettingsTree); });

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction *shortcutsAction = helpMenu->addAction(QStringLiteral("Keyboard Shortcuts"));
    shortcutsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    shortcutsAction->setShortcutContext(Qt::WindowShortcut);
    connect(shortcutsAction, &QAction::triggered, this, [this] {
        QMessageBox::information(this, QStringLiteral("Git Workspace Shortcuts"),
            QStringLiteral("<b>Git actions</b><br>"
                           "Ctrl+Alt+A — Stage all changes<br>"
                           "Ctrl+Alt+C — Commit staged changes<br>"
                           "Ctrl+Alt+P — Push<br>"
                           "Ctrl+Alt+L — Pull<br>"
                           "Ctrl+Alt+F — Fetch all remotes<br>"
                           "Ctrl+Alt+R — Refresh status<br>"
                           "Ctrl+W — Close Git Workspace<br>"
                           "Ctrl+Enter — Run Git command<br>"
                           "Ctrl+G, Ctrl+S — Stash changes<br>"
                           "Ctrl+G, Ctrl+U — Apply latest stash<br>"
                           "Ctrl+G, Ctrl+X — Resolve conflicts<br><br>"
                           "<b>Navigate</b><br>"
                           "Ctrl+G, Ctrl+C — Changes<br>"
                           "Ctrl+G, Ctrl+B — Branches<br>"
                           "Ctrl+G, Ctrl+H — History<br>"
                           "Ctrl+G, Ctrl+D — Details / Diff<br>"
                           "Ctrl+G, Ctrl+T — Git Command<br>"
                           "Ctrl+G, Ctrl+R — Remotes<br>"
                           "Ctrl+G, Ctrl+I — Identity &amp; Credentials"));
    });

    recentRepositoriesMenu = menuBar()->addMenu(QStringLiteral("&Recent Repositories"));
    connect(recentRepositoriesMenu, &QMenu::aboutToShow, this, [this] { refreshRecentRepositoriesMenu(); });
    refreshRecentRepositoriesMenu();

    mainSplitter->addWidget(branchTabs);
    mainSplitter->addWidget(leftTabs);
    mainSplitter->addWidget(rightTabs);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 0);
    mainSplitter->setStretchFactor(2, 2);
    mainSplitter->setSizes({250, 350, 500});
    layout->addWidget(mainSplitter, 1);
    setCentralWidget(root);
    setStyleSheet(QStringLiteral(
        "QPlainTextEdit#outputEdit { border: 1px solid palette(mid); border-radius: 6px; "
        "padding: 6px; background: palette(base); }"));
    branchStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(branchStatusLabel);
    gitProgressBar = new QProgressBar(this);
    gitProgressBar->setRange(0, 0);
    gitProgressBar->setTextVisible(true);
    gitProgressBar->setMinimumWidth(190);
    gitProgressBar->hide();
    statusBar()->addPermanentWidget(gitProgressBar);
    if (workspaceAmbiguous)
        statusBar()->showMessage(QStringLiteral("Workspace selection was cancelled. Repository workspace controls are disabled until an association is selected."), 10000);
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
        updateGitProgress(output);
        if (actionCommandActive) actionOutput += output;
        else appendOutput(output);
    });
    connect(commandProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        setGitBusy(false);
        if (operationLockHeld) {
            RepositoryOperationCoordinator::release(operationLockPath);
            operationLockHeld = false;
        }
        const bool conflictsExistedBeforeCommand = hadConflictsBeforeCommand;
        const bool pushAfterCommit = commitPushPending;
        const bool pushAfterSync = pushAfterSyncPending;
        const QStringList pushArguments = deferredPushArguments;
        commitPushPending = false;
        pushAfterSyncPending = false;
        deferredPushArguments.clear();
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
        hadConflictsBeforeCommand = hasConflicts;
        if (!conflictsExistedBeforeCommand && hasConflicts) {
            statusBar()->showMessage(QStringLiteral("Merge conflicts need your attention (%1 file(s))")
                                         .arg(conflictedFiles.size()), 10000);
            showMergeConflictsDialog();
        }
        if (code == 0 && !hasConflicts) {
            if (pushAfterCommit)
                runGit({QStringLiteral("push")});
            else if (pushAfterSync && !pushArguments.isEmpty())
                runGit(pushArguments);
        } else if (pushAfterCommit || pushAfterSync) {
            statusBar()->showMessage(QStringLiteral("Push was not started because the preceding Git operation did not complete cleanly."), 8000);
        }
    });
    connect(commandProcess, &QProcess::started, this, [this] {
        commandInput->setEnabled(false);
    });
    connect(commandProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        setGitBusy(false);
        if (operationLockHeld) {
            RepositoryOperationCoordinator::release(operationLockPath);
            operationLockHeld = false;
        }
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
    connect(branchesList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item || !item->data(0, Qt::UserRole + 3).toBool()) return;
        switchToBranch(item->data(0, Qt::UserRole).toString(), item->data(0, Qt::UserRole + 1).toBool());
    });
    connect(historyList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item) showCommit(item->data(Qt::UserRole).toString());
    });
    historyGraph->commitClicked = [this](const QString &hash) { showCommit(hash); rightTabs->setCurrentIndex(0); };
    branchesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(branchesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = branchesList->itemAt(position);
        if (!item || !item->data(0, Qt::UserRole + 3).toBool()) return;
        branchesList->setCurrentItem(item);
        showBranchContextMenu(item->data(0, Qt::UserRole).toString(), item->data(0, Qt::UserRole + 1).toBool(), branchesList->viewport()->mapToGlobal(position));
    });
    historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(historyList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QListWidgetItem *item = historyList->itemAt(position);
        if (item) showHistoryContextMenu(item, historyList->viewport()->mapToGlobal(position));
    });
    connect(saveIdentityButton, &QPushButton::clicked, this, [this] { saveIdentity(); });
    connect(configureHelperButton, &QPushButton::clicked, this, [this] { configureCredentialHelper(); });
    connect(addCredentialButton, &QPushButton::clicked, this, [this] { addCredential(); });
    connect(manageSshKeysButton, &QPushButton::clicked, this, [this] { manageSshKeys(); });
    changesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(changesTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = changesTree->itemAt(position);
        if (!item) return;
        changesTree->setCurrentItem(item);
        const QString path = item->data(0, Qt::UserRole).toString();
        if (path.isEmpty()) return;
        const QString status = item->data(0, Qt::UserRole + 2).toString();
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
    showHistoryOverview();
    lastObservedStatus = readStatusSnapshot();
    auto *statusWatchTimer = new QTimer(this);
    statusWatchTimer->setInterval(1000);
    connect(statusWatchTimer, &QTimer::timeout, this, [this] { pollRepositoryStatus(); });
    statusWatchTimer->start();
}

bool GitWorkspaceWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) {
        lastRepositoryFocusTimer->start();
    } else if (event->type() == QEvent::WindowDeactivate) {
        lastRepositoryFocusTimer->stop();
    }
    return QMainWindow::event(event);
}

void GitWorkspaceWindow::closeEvent(QCloseEvent *event)
{
    if (commandProcess && commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git operation still running"),
            QStringLiteral("Wait for the active Git operation to finish before closing this workspace."));
        event->ignore();
        return;
    }
    lastRepositoryFocusTimer->stop();
    QMainWindow::closeEvent(event);
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
    const QString selectedDiffPath = selectedPath();
    const bool showingWorkingDiff = !selectedDiffPath.isEmpty() && currentDiffCommitHash.isEmpty()
        && detailsStack->currentWidget() == detailsStack->widget(1);
    refreshStatus();
    refreshBranches();
    refreshRemotes();
    refreshHistory();
    if (historyViews && historyViews->currentWidget() != historyList) refreshHistoryGraph();
    refreshIdentity();
    refreshRepositorySettings();
    hasStashes = !gitOutput({QStringLiteral("stash"), QStringLiteral("list")}).isEmpty();
    updateToolbarState();
    updateStatusBar();
    lastObservedStatus = readStatusSnapshot();
    if (showingWorkingDiff) {
        QTreeWidgetItem *matchingItem = nullptr;
        const auto findPath = [&selectedDiffPath](QTreeWidgetItem *parent, auto &&self) -> QTreeWidgetItem * {
            for (int index = 0; index < parent->childCount(); ++index) {
                QTreeWidgetItem *item = parent->child(index);
                if (item->data(0, Qt::UserRole).toString() == selectedDiffPath) return item;
                if (QTreeWidgetItem *nested = self(item, self)) return nested;
            }
            return nullptr;
        };
        for (int index = 0; index < changesTree->topLevelItemCount() && !matchingItem; ++index) {
            QTreeWidgetItem *item = changesTree->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == selectedDiffPath) matchingItem = item;
            else matchingItem = findPath(item, findPath);
        }
        if (matchingItem) {
            changesTree->setCurrentItem(matchingItem);
            showDiff(selectedDiffPath);
        } else {
            detailsView->setPlainText(QStringLiteral("This file is no longer listed as changed."));
            detailsStack->setCurrentWidget(detailsView);
            lastObservedDiffFile.clear();
        }
    }
}

void GitWorkspaceWindow::refreshRepositorySettings()
{
    if (!repositorySettingsTree) return;
    repositorySettingsTree->clear();
    const QString output = gitOutput({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--list")});
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) continue;
        const QString key = line.left(equals);
        if (isManagedRepositorySetting(key) || isCuratedRepositorySetting(key)) continue;
        auto *item = new QTreeWidgetItem(repositorySettingsTree);
        item->setText(0, key);
        item->setText(1, line.mid(equals + 1));
        item->setToolTip(1, line.mid(equals + 1));
    }
    for (const RepositorySettingDefinition &setting : repositorySettingDefinitions()) {
        int code = -1;
        const QString value = gitOutput({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--get"), setting.key}, &code);
        QComboBox *control = repositorySettingControls.value(setting.key);
        if (!control) continue;
        while (control->count() > setting.labels.size()) control->removeItem(control->count() - 1);
        const int valueIndex = code == 0 ? setting.values.indexOf(value) : 0;
        if (valueIndex < 0) {
            control->addItem(QStringLiteral("Other value: %1").arg(value), value);
            control->setCurrentIndex(control->count() - 1);
        } else control->setCurrentIndex(valueIndex);
    }
}

void GitWorkspaceWindow::applyRepositorySettings()
{
    if (commandProcess->state() != QProcess::NotRunning || RepositoryOperationCoordinator::isBusy(repoPath)) {
        statusBar()->showMessage(QStringLiteral("Wait for Git to finish before applying repository settings."), 4000);
        return;
    }
    for (const RepositorySettingDefinition &setting : repositorySettingDefinitions()) {
        QComboBox *control = repositorySettingControls.value(setting.key);
        if (!control) continue;
        const QString value = control->currentData().toString();
        const bool inherited = value == QLatin1String("__inherit__");
        int existingCode = -1;
        const QString existing = gitOutput({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--get"), setting.key}, &existingCode);
        if ((inherited && existingCode != 0) || (!inherited && existingCode == 0 && existing == value)) continue;

        QProcess process;
        process.setProgram(gitExecutable());
        QStringList arguments{QStringLiteral("-C"), repoPath, QStringLiteral("config"), QStringLiteral("--local")};
        if (inherited) arguments << QStringLiteral("--unset-all") << setting.key;
        else arguments << QStringLiteral("--replace-all") << setting.key << value;
        process.setArguments(arguments);
        process.start();
        if (!process.waitForFinished(5000) || process.exitCode() != 0) {
            QMessageBox::warning(this, QStringLiteral("Could not apply repository setting"),
                QStringLiteral("Git could not update %1:\n%2").arg(setting.key, QString::fromLocal8Bit(process.readAllStandardError()).trimmed()));
            refreshRepositorySettings();
            return;
        }
    }
    refreshRepositorySettings();
    statusBar()->showMessage(QStringLiteral("Repository settings applied."), 3000);
}

void GitWorkspaceWindow::addRepositorySetting()
{
    bool accepted = false;
    const QString key = QInputDialog::getText(this, QStringLiteral("Add repository setting"), QStringLiteral("Git config key:"), QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || key.isEmpty()) return;
    if (isManagedRepositorySetting(key) || isCuratedRepositorySetting(key)) {
        QMessageBox::information(this, QStringLiteral("Setting has a dedicated control"),
            QStringLiteral("%1 is managed in its dedicated repository settings control or manager.").arg(key));
        return;
    }
    const QString value = QInputDialog::getText(this, QStringLiteral("Add repository setting"), QStringLiteral("Value:"), QLineEdit::Normal, QString(), &accepted);
    if (accepted) runGit({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--replace-all"), key, value});
}

void GitWorkspaceWindow::editRepositorySetting(QTreeWidgetItem *item)
{
    if (!item) return;
    if (isManagedRepositorySetting(item->text(0)) || isCuratedRepositorySetting(item->text(0))) return;
    bool accepted = false;
    const QString value = QInputDialog::getText(this, QStringLiteral("Edit repository setting"), item->text(0), QLineEdit::Normal, item->text(1), &accepted);
    if (accepted) runGit({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--replace-all"), item->text(0), value});
}

void GitWorkspaceWindow::removeRepositorySetting(QTreeWidgetItem *item)
{
    if (!item) return;
    const QString key = item->text(0);
    if (isManagedRepositorySetting(key) || isCuratedRepositorySetting(key)) return;
    if (QMessageBox::question(this, QStringLiteral("Remove repository setting"), QStringLiteral("Remove all repository values for %1?").arg(key)) == QMessageBox::Yes)
        runGit({QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("--unset-all"), key});
}

void GitWorkspaceWindow::refreshWorkspaceRepositoryList()
{
    QStringList validActionIds;
    for (const QString &id : workspace.actionRepositoryIds)
        if (std::any_of(workspace.repositories.cbegin(), workspace.repositories.cend(), [&id](const WorkspaceRepository &repository) { return repository.id == id; }))
            validActionIds.append(id);
    if (validActionIds != workspace.actionRepositoryIds) {
        workspace.actionRepositoryIds = validActionIds;
        RepositoryWorkspaceStore::saveWorkspace(workspace);
    }
    updateActionRepositoriesButton();
    if (!workspaceRepositoryList) return;
    const bool wasBlocked = workspaceRepositoryList->blockSignals(true);
    workspaceRepositoryList->clear();
    for (const WorkspaceRepository &repository : workspace.repositories) {
        const bool available = QFileInfo(repository.path).isDir() && isGitRepository(repository.path);
        auto *item = new QTreeWidgetItem(workspaceRepositoryList);
        const QString name = QFileInfo(repository.path).fileName().isEmpty() ? repository.path : QFileInfo(repository.path).fileName();
        item->setText(0, name);
        item->setText(1, available ? (repository.path == repoPath ? QStringLiteral("Active") : QStringLiteral("Available")) : QStringLiteral("Missing"));
        item->setData(0, Qt::UserRole, repository.id);
        item->setToolTip(0, repository.path);
        if (!available) item->setToolTip(1, QStringLiteral("Last known path: %1").arg(repository.path));
        if (repository.path == repoPath) workspaceRepositoryList->setCurrentItem(item);
    }
    workspaceRepositoryList->blockSignals(wasBlocked);
}

void GitWorkspaceWindow::addWorkspaceRepository()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Add Existing Git Repository"),
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty()) return;
    QString error;
    if (!RepositoryWorkspaceStore::addRepository(workspace, path, &error)) {
        QMessageBox::information(this, QStringLiteral("Repository Not Added"), error);
        return;
    }
    refreshWorkspaceRepositoryList();
}

void GitWorkspaceWindow::removeWorkspaceRepository()
{
    QTreeWidgetItem *item = workspaceRepositoryList ? workspaceRepositoryList->currentItem() : nullptr;
    if (!item) return;
    const QString repositoryId = item->data(0, Qt::UserRole).toString();
    if (workspace.repositories.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("Workspace Requires a Repository"),
            QStringLiteral("The last repository stays as the normal single-repository workspace."));
        return;
    }
    const auto found = std::find_if(workspace.repositories.cbegin(), workspace.repositories.cend(), [&repositoryId](const WorkspaceRepository &repository) { return repository.id == repositoryId; });
    if (found == workspace.repositories.cend()) return;
    const bool removingActive = found->path == repoPath;
    if (!RepositoryWorkspaceStore::removeRepository(workspace, repositoryId)) return;
    if (removingActive) switchRepository(workspace.primaryRepositoryId);
    refreshWorkspaceRepositoryList();
}

void GitWorkspaceWindow::locateWorkspaceRepository()
{
    QTreeWidgetItem *item = workspaceRepositoryList ? workspaceRepositoryList->currentItem() : nullptr;
    if (!item) return;
    const QString repositoryId = item->data(0, Qt::UserRole).toString();
    const auto found = std::find_if(workspace.repositories.cbegin(), workspace.repositories.cend(), [&repositoryId](const WorkspaceRepository &repository) { return repository.id == repositoryId; });
    if (found == workspace.repositories.cend()) return;
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Locate Git Repository"), QDir::homePath(), QFileDialog::ShowDirsOnly);
    if (path.isEmpty()) return;
    const QString root = RepositoryWorkspaceStore::repositoryRoot(path);
    if (root.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Not a Git Repository"), QStringLiteral("The selected folder is not inside a Git working tree."));
        return;
    }
    const bool replacingActive = found->path == repoPath;
    const QString commonDirectory = RepositoryWorkspaceStore::gitDirectory(root, true);
    if (!found->commonGitDirectory.isEmpty() && commonDirectory != found->commonGitDirectory) {
        if (QMessageBox::question(this, QStringLiteral("Repository Identity Changed"),
                QStringLiteral("The Git metadata does not match the last known identity for this repository. Replace the workspace entry with %1?").arg(root),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    }
    QString error;
    if (!RepositoryWorkspaceStore::updateRepositoryPath(workspace, repositoryId, root, &error)) {
        QMessageBox::warning(this, QStringLiteral("Repository Not Located"), error.isEmpty() ? QStringLiteral("The workspace entry could not be updated.") : error);
        return;
    }
    if (replacingActive) switchRepository(repositoryId);
    refreshWorkspaceRepositoryList();
}

void GitWorkspaceWindow::switchRepository(const QString &repositoryId)
{
    const auto found = std::find_if(workspace.repositories.cbegin(), workspace.repositories.cend(), [&repositoryId](const WorkspaceRepository &repository) { return repository.id == repositoryId; });
    if (found == workspace.repositories.cend()) return;
    if (found->path == repoPath) return;
    if (!QFileInfo(found->path).isDir() || !isGitRepository(found->path)) {
        QMessageBox::information(this, QStringLiteral("Repository Missing"),
            QStringLiteral("This repository is unavailable at its last known path. Use Locate to find it."));
        refreshWorkspaceRepositoryList();
        return;
    }
    if (commandProcess && commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git Operation Running"),
            QStringLiteral("Wait for the current Git operation to finish before switching repositories."));
        refreshWorkspaceRepositoryList();
        return;
    }
    repoPath = found->path;
    setWindowTitle(QStringLiteral("Git Workspace — %1").arg(QDir(repoPath).dirName()));
    if (completionController) completionController->setWorkingDirectory(repoPath);
    currentDiffCommitHash.clear();
    lastObservedDiffFile.clear();
    changesTree->clearSelection();
    changesTree->clear();
    diffFilesList->clear();
    commandOutput->clear();
    detailsStack->setCurrentWidget(detailsView);
    detailsView->setPlainText(QStringLiteral("Select a changed file, branch, or commit to inspect it."));
    refreshWorkspaceRepositoryList();
    refreshAll();
    statusBar()->showMessage(QStringLiteral("Switched to %1").arg(QDir(repoPath).dirName()), 3000);
}

QVector<WorkspaceRepository> GitWorkspaceWindow::actionRepositories() const
{
    QVector<WorkspaceRepository> selected;
    if (workspace.actionRepositoryIds.isEmpty()) return workspace.repositories;
    for (const WorkspaceRepository &repository : workspace.repositories)
        if (workspace.actionRepositoryIds.contains(repository.id)) selected.append(repository);
    if (selected.isEmpty()) return workspace.repositories;
    return selected;
}

bool GitWorkspaceWindow::actionsTargetOnlyActiveRepository() const
{
    const QVector<WorkspaceRepository> selected = actionRepositories();
    return selected.size() == 1
        && QFileInfo(selected.first().path).canonicalFilePath() == QFileInfo(repoPath).canonicalFilePath();
}

void GitWorkspaceWindow::updateActionRepositoriesButton()
{
    if (!actionRepositoriesButton) return;
    const QVector<WorkspaceRepository> selected = actionRepositories();
    actionRepositoriesButton->setVisible(workspace.repositories.size() > 1 && !workspaceAmbiguous);
    if (workspace.actionRepositoryIds.isEmpty())
        actionRepositoriesButton->setText(QStringLiteral("Actions: All repositories"));
    else if (selected.size() == 1)
        actionRepositoriesButton->setText(QStringLiteral("Actions: %1").arg(QFileInfo(selected.first().path).fileName()));
    else
        actionRepositoriesButton->setText(QStringLiteral("Actions: %1 repositories").arg(selected.size()));
    QStringList names;
    for (const WorkspaceRepository &repository : selected) names.append(QFileInfo(repository.path).fileName());
    actionRepositoriesButton->setToolTip(QStringLiteral("Actions apply to: %1").arg(names.join(QStringLiteral(", "))));
}

void GitWorkspaceWindow::showBatchOperations(const QString &initialOperation)
{
    if (commandProcess && commandProcess->state() != QProcess::NotRunning
        && RepositoryOperationCoordinator::isBusy(repoPath)) {
        statusBar()->showMessage(QStringLiteral("The active repository is busy; busy repositories will be unavailable to batch operations."), 5000);
    }
    BatchOperationsDialog dialog(actionRepositories(), this, initialOperation);
    dialog.exec();
    refreshWorkspaceRepositoryList();
    refreshAll();
}

QString GitWorkspaceWindow::readStatusSnapshot() const
{
    QProcess process;
    process.setProgram(gitExecutable());
    process.setArguments({QStringLiteral("status"), QStringLiteral("--porcelain=v2"),
                          QStringLiteral("--branch"), QStringLiteral("-z")});
    process.setWorkingDirectory(repoPath);
    process.start();
    if (!process.waitForFinished(2000) || process.exitCode() != 0) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput());
}

void GitWorkspaceWindow::pollRepositoryStatus()
{
    if (commandProcess->state() != QProcess::NotRunning || RepositoryOperationCoordinator::isBusy(repoPath))
        return;
    const QString snapshot = readStatusSnapshot();
    if (snapshot.isNull())
        return;
    if (snapshot != lastObservedStatus) {
        refreshAll();
        return;
    }
    const QString path = selectedPath();
    if (path.isEmpty() || !currentDiffCommitHash.isEmpty() || detailsStack->currentWidget() != detailsStack->widget(1))
        return;
    QFile file(QDir(repoPath).filePath(path));
    QByteArray fileSnapshot;
    if (file.open(QIODevice::ReadOnly))
        fileSnapshot = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    const QString currentSnapshot = QString::fromLatin1(fileSnapshot);
    if (currentSnapshot != lastObservedDiffFile)
        showDiff(path);
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
    QHash<QString, QTreeWidgetItem *> directories;
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
        const QString oldPath = hasSecondaryRenamePath && recordIndex + 1 < records.size()
            ? QString::fromLocal8Bit(records.at(recordIndex + 1)) : QString();
        const QStringList pathParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QTreeWidgetItem *parentItem = nullptr;
        QString directoryPath;
        for (qsizetype partIndex = 0; partIndex + 1 < pathParts.size(); ++partIndex) {
            directoryPath = directoryPath.isEmpty() ? pathParts.at(partIndex)
                : directoryPath + QLatin1Char('/') + pathParts.at(partIndex);
            if (directories.contains(directoryPath)) {
                parentItem = directories.value(directoryPath);
                continue;
            }
            auto *directoryItem = parentItem
                ? new QTreeWidgetItem(parentItem, {pathParts.at(partIndex), QString()})
                : new QTreeWidgetItem(changesTree, {pathParts.at(partIndex), QString()});
            directoryItem->setToolTip(0, directoryPath);
            directories.insert(directoryPath, directoryItem);
            parentItem = directoryItem;
        }
        const QString fileName = pathParts.isEmpty() ? path : pathParts.last();
        const QString label = hasSecondaryRenamePath ? QStringLiteral("%1 (from %2)").arg(fileName, oldPath) : fileName;
        const QString displayedStatus = xy == QStringLiteral("??") ? QStringLiteral("A") : xy;
        auto *item = parentItem ? new QTreeWidgetItem(parentItem, {label, displayedStatus})
                                : new QTreeWidgetItem(changesTree, {label, displayedStatus});
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, conflicted);
        item->setData(0, Qt::UserRole + 2, xy);
        item->setToolTip(0, path);
        if (conflicted) conflictedFiles.append(path);
        if (hasSecondaryRenamePath)
            ++recordIndex;
    }
    if (changesTree->topLevelItemCount() == 0)
        changesTree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("Working tree clean"), QString()}));
    changesTree->expandAll();
    hasConflicts = !conflictedFiles.isEmpty();
    conflictsButton->setEnabled(hasConflicts);
    conflictsButton->setToolTip(hasConflicts
        ? QStringLiteral("Resolve %1 conflicted file(s)").arg(conflictedFiles.size())
        : QStringLiteral("No merge conflicts to resolve"));
}

void GitWorkspaceWindow::refreshBranches()
{
    QHash<QString, bool> expandedStates;
    const auto itemName = [](const QTreeWidgetItem *item) {
        QString name = item->text(0);
        if (name.startsWith(QStringLiteral("● "))) name.remove(0, 2);
        return name;
    };
    std::function<void(QTreeWidgetItem *, const QString &)> saveExpansion;
    saveExpansion = [&](QTreeWidgetItem *item, const QString &parentPath) {
        const QString path = parentPath.isEmpty() ? itemName(item)
            : parentPath + QLatin1Char('/') + itemName(item);
        expandedStates.insert(path, item->isExpanded());
        for (int i = 0; i < item->childCount(); ++i)
            saveExpansion(item->child(i), path);
    };
    for (int i = 0; i < branchesList->topLevelItemCount(); ++i)
        saveExpansion(branchesList->topLevelItem(i), QString());

    branchesList->clear();
    QTreeWidgetItem *localRoot = new QTreeWidgetItem(branchesList, {QStringLiteral("Local")});
    QTreeWidgetItem *remoteRoot = new QTreeWidgetItem(branchesList, {QStringLiteral("Remotes")});
    const auto insertBranch = [](QTreeWidgetItem *root, const QString &path, const QString &fullName, bool remote, bool current) {
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QTreeWidgetItem *parent = root;
        for (qsizetype i = 0; i < parts.size(); ++i) {
            QTreeWidgetItem *child = nullptr;
            for (int j = 0; j < parent->childCount(); ++j) {
                if (parent->child(j)->text(0) == parts.at(i)) { child = parent->child(j); break; }
            }
            if (!child) child = new QTreeWidgetItem(parent, {parts.at(i)});
            if (i + 1 == parts.size()) {
                child->setData(0, Qt::UserRole, fullName);
                child->setData(0, Qt::UserRole + 1, remote);
                child->setData(0, Qt::UserRole + 2, current);
                child->setData(0, Qt::UserRole + 3, true);
                if (current) child->setText(0, QStringLiteral("● %1").arg(parts.at(i)));
            }
            parent = child;
        }
    };
    const QString text = gitOutput({QStringLiteral("branch"), QStringLiteral("--all"), QStringLiteral("--no-color")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        QString name = line.trimmed();
        const bool current = name.startsWith(QStringLiteral("* "));
        if (name.startsWith(QStringLiteral("* ")) || name.startsWith(QStringLiteral("  "))) name.remove(0, 2);
        const bool remote = name.startsWith(QStringLiteral("remotes/"));
        if (remote) name.remove(0, QStringLiteral("remotes/").size());
        if (name.contains(QStringLiteral(" -> "))) continue;
        insertBranch(remote ? remoteRoot : localRoot, name, name, remote, current);
    }
    std::function<void(QTreeWidgetItem *, const QString &)> restoreExpansion;
    restoreExpansion = [&](QTreeWidgetItem *item, const QString &parentPath) {
        const QString path = parentPath.isEmpty() ? itemName(item)
            : parentPath + QLatin1Char('/') + itemName(item);
        // Keep established user choices. Newly discovered folders are opened
        // by default so branches fetched during this refresh are easy to find.
        item->setExpanded(expandedStates.contains(path)
            ? expandedStates.value(path)
            : (item->childCount() > 0));
        for (int i = 0; i < item->childCount(); ++i)
            restoreExpansion(item->child(i), path);
    };
    for (int i = 0; i < branchesList->topLevelItemCount(); ++i)
        restoreExpansion(branchesList->topLevelItem(i), QString());
    hasRemote = !gitOutput({QStringLiteral("remote")}).isEmpty();
    const QString branch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    const QString upstream = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("@{upstream}")});
    hasUpstream = !upstream.isEmpty() && !upstream.startsWith(QStringLiteral("fatal:"));
    bool countOk = false;
    int unpushedCount = 0;
    if (!branch.isEmpty() && hasRemote) {
        const QStringList countArguments = hasUpstream
            ? QStringList{QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("@{upstream}..HEAD")}
            : QStringList{QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD"), QStringLiteral("--not"), QStringLiteral("--remotes")};
        unpushedCount = gitOutput(countArguments).toInt(&countOk);
    }
    const QStringList remoteBranches = gitOutput({QStringLiteral("for-each-ref"), QStringLiteral("--format=%(refname:short)"), QStringLiteral("refs/remotes")})
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const bool branchPublished = std::any_of(remoteBranches.cbegin(), remoteBranches.cend(), [&branch](const QString &ref) {
        return ref.endsWith(QLatin1Char('/') + branch);
    });
    hasUnpushedCommits = countOk && (unpushedCount > 0 || (!hasUpstream && !branchPublished));
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
    if (!hasRemote) return;
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
    const QString head = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("HEAD")});
    const QString branch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    const QString text = gitOutput({QStringLiteral("log"), QStringLiteral("-30"), QStringLiteral("--format=%H%x09%h%x09%s")});
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList columns = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        if (columns.size() < 3) continue;
        const bool isHead = columns.at(0) == head;
        const QString prefix = isHead ? QStringLiteral("▶ HEAD  ") : QString();
        auto *item = new QListWidgetItem(QStringLiteral("%1%2  %3").arg(prefix, columns.at(1), columns.mid(2).join(QLatin1Char('\t'))));
        item->setData(Qt::UserRole, columns.at(0));
        if (isHead) {
            item->setToolTip(branch.isEmpty()
                ? QStringLiteral("Current commit: %1 (detached HEAD)").arg(head)
                : QStringLiteral("Current branch %1 is at commit %2").arg(branch, head));
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
        historyList->addItem(item);
    }
}

void GitWorkspaceWindow::refreshHistoryGraph()
{
        historyGraph->nodes.clear(); historyGraph->edges.clear(); historyGraph->laneNames.clear(); historyGraph->laneColors.clear();
        QHash<QString,int> nodeRows;
        const QString log = gitOutput({QStringLiteral("log"), QStringLiteral("--all"), QStringLiteral("--topo-order"),
            QStringLiteral("--format=%H%x09%P%x09%s")});
        for (const QString &line : log.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList c=line.split(QLatin1Char('\t'),Qt::KeepEmptyParts); if(c.size()<3) continue;
            HistoryGraphWidget::Node n; n.hash=c[0]; n.parents=c[1].split(QLatin1Char(' '),Qt::SkipEmptyParts); n.subject=c.mid(2).join(QLatin1Char('\t')); n.merge=n.parents.size()>1;
            nodeRows.insert(n.hash,historyGraph->nodes.size()); historyGraph->nodes.append(n);
        }
        const QString head=gitOutput({QStringLiteral("rev-parse"),QStringLiteral("--verify"),QStringLiteral("HEAD")});
        const QString current=gitOutput({QStringLiteral("branch"),QStringLiteral("--show-current")});
        const QString mainName=current.isEmpty()?QStringLiteral("HEAD (detached)"):current;
        QHash<QString,QStringList> branchAliases;
        QStringList branchOrder;
        const QString refs=gitOutput({QStringLiteral("for-each-ref"),QStringLiteral("--format=%(refname:short)%09%(objectname)"),QStringLiteral("refs/heads"),QStringLiteral("refs/remotes")});
        int colorIndex=0;
        for(const QString &refLine:refs.split(QLatin1Char('\n'),Qt::SkipEmptyParts)) {
            const QStringList c=refLine.split(QLatin1Char('\t')); if(c.size()<2 || c[0].endsWith(QStringLiteral("/HEAD"))) continue;
            const QString name=c[0], tip=c[1];
            if(!branchAliases.contains(tip)) { branchAliases.insert(tip,{}); branchOrder.append(tip); }
            branchAliases[tip].append(name);
        }
        QHash<QString,int> commitLanes;
        const QStringList mainChain=gitOutput({QStringLiteral("rev-list"),QStringLiteral("--first-parent"),QStringLiteral("HEAD")}).split(QLatin1Char('\n'),Qt::SkipEmptyParts);
        for(const QString &hash:mainChain) if(nodeRows.contains(hash)) commitLanes.insert(hash,0);
        historyGraph->laneNames.insert(0,mainName);
        historyGraph->laneColors.insert(0,QColor("#146c94"));
        for(const QString &tip:branchOrder) {
            QStringList aliases=branchAliases.value(tip);
            std::sort(aliases.begin(),aliases.end(),[&](const QString &a,const QString &b) {
                if(a==b) return false;
                if(a==current) return true;
                if(b==current) return false;
                const bool aLocal=!a.contains(QLatin1Char('/')), bLocal=!b.contains(QLatin1Char('/'));
                if(aLocal!=bLocal) return aLocal;
                return a<b;
            });
            const QString representative=aliases.isEmpty()?tip:aliases.first();
            const QString label=aliases.join(QStringLiteral("  /  "));
            const QColor color=QColor::fromHsv((colorIndex++*137+24)%360,170,185);
            const QStringList firstParent=gitOutput({QStringLiteral("rev-list"),QStringLiteral("--first-parent"),representative}).split(QLatin1Char('\n'),Qt::SkipEmptyParts);
            int lane=-1;
            for(const QString &hash:firstParent) {
                if(!nodeRows.contains(hash)) continue;
                if(commitLanes.contains(hash)) {
                    const int existingLane=commitLanes.value(hash);
                    if(lane<0 && existingLane>0) lane=existingLane;
                    continue;
                }
                if(lane<0) lane=historyGraph->laneNames.size();
                commitLanes.insert(hash,lane);
            }
            if(lane>0) { historyGraph->laneNames.insert(lane,label); historyGraph->laneColors.insert(lane,color); }
            if(nodeRows.contains(tip)) historyGraph->nodes[nodeRows[tip]].refs.append(label);
        }
        int nextLane=historyGraph->laneNames.size();
        for(const auto &node:historyGraph->nodes) {
            if(commitLanes.contains(node.hash)) continue;
            int lane=commitLanes.value(node.parents.value(0),-1);
            if(lane<0) lane=nextLane++;
            commitLanes.insert(node.hash,lane);
            if(!historyGraph->laneNames.contains(lane)) {
                historyGraph->laneNames.insert(lane,QStringLiteral("Other history"));
                historyGraph->laneColors.insert(lane,QColor("#9299a2"));
            }
        }
        for(auto &node:historyGraph->nodes) node.lane=commitLanes.value(node.hash,0);
        if(nodeRows.contains(head)) historyGraph->nodes[nodeRows[head]].refs.prepend(QStringLiteral("HEAD: ")+mainName);
        for(int i=0;i<historyGraph->nodes.size();++i) for(const QString &parent:historyGraph->nodes[i].parents) {
            if(!nodeRows.contains(parent)) continue;
            const int fromLane=historyGraph->nodes[i].lane, toLane=historyGraph->nodes[nodeRows[parent]].lane;
            const int labelLane=fromLane!=0?fromLane:toLane;
            const QString branchName=historyGraph->laneNames.value(labelLane,mainName);
            historyGraph->edges.append({i,nodeRows[parent],branchName,historyGraph->laneColors.value(labelLane,QColor("#9299a2"))});
        }
        historyGraph->updateGeometry(); historyGraph->update();
}

void GitWorkspaceWindow::showHistoryOverview()
{
    if (historyViews->currentWidget() == historyList) {
        refreshHistoryGraph();
        historyViews->setCurrentIndex(1);
        historyOverviewButton->setText(QStringLiteral("Show list"));
    } else {
        historyViews->setCurrentWidget(historyList);
        historyOverviewButton->setText(QStringLiteral("Show graph"));
    }
}

void GitWorkspaceWindow::recordRecentWorkspace()
{
    if (workspace.id.isEmpty()) return;
    QString path = repoPath;
    for (const WorkspaceRepository &repository : workspace.repositories) {
        if (repository.id == workspace.primaryRepositoryId) {
            path = repository.path;
            break;
        }
    }
    QSettings settings;
    QVariantList recent = settings.value(QStringLiteral("gitWorkspace/recentWorkspaces")).toList();
    for (qsizetype i = recent.size() - 1; i >= 0; --i) {
        if (recent.at(i).toMap().value(QStringLiteral("id")).toString() == workspace.id)
            recent.removeAt(i);
    }
    recent.prepend(QVariantMap{{QStringLiteral("id"), workspace.id}, {QStringLiteral("path"), path}});
    while (recent.size() > 10) recent.removeLast();
    settings.setValue(QStringLiteral("gitWorkspace/recentWorkspaces"), recent);
}

void GitWorkspaceWindow::refreshRecentRepositoriesMenu()
{
    if (!recentRepositoriesMenu) return;
    recentRepositoriesMenu->clear();
    const QVariantList recent = QSettings().value(QStringLiteral("gitWorkspace/recentWorkspaces")).toList();
    if (recent.isEmpty()) {
        QAction *empty = recentRepositoriesMenu->addAction(QStringLiteral("No recent workspaces"));
        empty->setEnabled(false);
        return;
    }
    const QVector<RepositoryWorkspace> configured = RepositoryWorkspaceStore::load();
    for (const QVariant &value : recent) {
        const QVariantMap item = value.toMap();
        const QString id = item.value(QStringLiteral("id")).toString();
        QString path = item.value(QStringLiteral("path")).toString();
        QStringList repositoryPaths;
        for (const RepositoryWorkspace &entry : configured) {
            if (entry.id != id) continue;
            for (const WorkspaceRepository &repository : entry.repositories) {
                repositoryPaths.append(repository.path);
                if (repository.id == entry.primaryRepositoryId) path = repository.path;
            }
            if (path.isEmpty() && !entry.repositories.isEmpty()) path = entry.repositories.first().path;
            break;
        }
        if (id.isEmpty() || path.isEmpty()) continue;
        QString label = QFileInfo(path).fileName();
        if (label.isEmpty()) label = path;
        if (repositoryPaths.size() > 1)
            label += QStringLiteral(" (%1 repositories)").arg(repositoryPaths.size());
        QAction *action = recentRepositoriesMenu->addAction(label);
        action->setToolTip(repositoryPaths.isEmpty() ? path : repositoryPaths.join(QLatin1Char('\n')));
        connect(action, &QAction::triggered, this, [this, path] {
            if (!isGitRepository(path)) {
                QMessageBox::warning(this, QStringLiteral("Recent workspace unavailable"),
                    QStringLiteral("The primary repository for this workspace can no longer be opened:\n%1").arg(path));
                return;
            }
            auto *window = new GitWorkspaceWindow(path);
            window->show();
        });
    }
    if (recentRepositoriesMenu->actions().isEmpty()) {
        QAction *empty = recentRepositoriesMenu->addAction(QStringLiteral("No recent workspaces"));
        empty->setEnabled(false);
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

void GitWorkspaceWindow::openRemoteInBrowser(const QString &name)
{
    const QString remote = gitOutput({QStringLiteral("remote"), QStringLiteral("get-url"), name}).trimmed();
    if (remote.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Remote URL unavailable"),
            QStringLiteral("Could not read the URL for remote %1.").arg(name));
        return;
    }

    QUrl webUrl;
    static const QRegularExpression scpUrlPattern(QStringLiteral("^(?:[^@/]+@)?([^:/]+):(.+)$"));
    const auto scpMatch = scpUrlPattern.match(remote);
    if (!remote.contains(QStringLiteral("://")) && scpMatch.hasMatch()) {
        webUrl.setScheme(QStringLiteral("https"));
        webUrl.setHost(scpMatch.captured(1));
        webUrl.setPath(QLatin1Char('/') + scpMatch.captured(2));
    } else {
        webUrl = QUrl(remote);
        const QString scheme = webUrl.scheme().toLower();
        if (scheme == QStringLiteral("ssh") || scheme == QStringLiteral("git") || scheme == QStringLiteral("git+ssh")) {
            webUrl.setScheme(QStringLiteral("https"));
            webUrl.setUserInfo(QString());
            webUrl.setPort(-1);
        }
    }

    QString path = webUrl.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive)) path.chop(4);
    webUrl.setPath(path);
    webUrl.setQuery(QString());
    webUrl.setFragment(QString());
    if (!webUrl.isValid() || webUrl.host().isEmpty() || path.isEmpty()
        || (webUrl.scheme() != QStringLiteral("http") && webUrl.scheme() != QStringLiteral("https"))) {
        QMessageBox::warning(this, QStringLiteral("Cannot open remote in browser"),
            QStringLiteral("Remote %1 does not have a web-compatible repository URL:\n%2").arg(name, remote));
        return;
    }
    if (!QDesktopServices::openUrl(webUrl)) {
        QMessageBox::warning(this, QStringLiteral("Could not open browser"),
            QStringLiteral("Could not open %1 in the default browser.").arg(webUrl.toString()));
    }
}

void GitWorkspaceWindow::createRemoteRepository()
{
    int headCheckExitCode = -1;
    const QString head = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--verify"),
        QStringLiteral("--quiet"), QStringLiteral("HEAD")}, &headCheckExitCode);
    const bool hasCommits = headCheckExitCode == 0 && !head.isEmpty();
    const QStringList localBranches = gitOutput({QStringLiteral("for-each-ref"),
        QStringLiteral("--format=%(refname:short)"), QStringLiteral("refs/heads")})
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList defaultPushBranches;
    QStringList additionalBranches;
    for (const QString &branch : localBranches) {
        const QString name = branch.trimmed();
        if (name == QStringLiteral("main") || name == QStringLiteral("master"))
            defaultPushBranches.append(name);
        else
            additionalBranches.append(name);
    }
    const bool needsBranchSelection = hasCommits && !additionalBranches.isEmpty();
    const int totalSteps = hasCommits && needsBranchSelection ? 5 : 4;
    const auto stepTitle = [totalSteps](const QString &number, const QString &label) {
        return QStringLiteral("Step %1 of %2 — %3").arg(number).arg(totalSteps).arg(label);
    };
    // Keep provider selection explicit so additional Git hosting services can be
    // added here without tying this workflow to GitHub's command line.
    const QStringList providers{QStringLiteral("GitHub")};
    bool accepted = false;
    const QString provider = QInputDialog::getItem(this, stepTitle(QStringLiteral("1"), QStringLiteral("Git service")),
        QStringLiteral("Git service:"), providers, 0, false, &accepted);
    if (!accepted || provider.isEmpty()) return;
    QString gh = QStandardPaths::findExecutable(QStringLiteral("gh"));
    const QString suggestedName = QFileInfo(repoPath).fileName();
    const QString repositoryName = QInputDialog::getText(this, stepTitle(QStringLiteral("2"), QStringLiteral("Repository name")),
        QStringLiteral("Repository name (or owner/name):"), QLineEdit::Normal, suggestedName, &accepted).trimmed();
    if (!accepted || repositoryName.isEmpty()) return;
    const QStringList visibilityOptions{QStringLiteral("Private"), QStringLiteral("Public")};
    const QString visibility = QInputDialog::getItem(this, stepTitle(QStringLiteral("3"), QStringLiteral("Repository visibility")),
        QStringLiteral("Visibility:"), visibilityOptions, 0, false, &accepted);
    if (!accepted) return;
    QString license;
    if (visibility == QStringLiteral("Public")) {
        license = QInputDialog::getText(this, stepTitle(QStringLiteral("3.5"), QStringLiteral("Public repository license")),
            QStringLiteral("GitHub license identifier (MPL-2.0 by default):"), QLineEdit::Normal,
            QStringLiteral("MPL-2.0"), &accepted).trimmed();
        if (!accepted || license.isEmpty()) return;
    }
    const QStringList existingRemotes = gitOutput({QStringLiteral("remote")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const QString suggestedRemote = existingRemotes.contains(QStringLiteral("origin"))
        ? QStringLiteral("github") : QStringLiteral("origin");
    const QString remoteName = QInputDialog::getText(this, stepTitle(QStringLiteral("4"), QStringLiteral("Local remote name")),
        QStringLiteral("Local remote name:"), QLineEdit::Normal, suggestedRemote, &accepted).trimmed();
    if (!accepted || remoteName.isEmpty()) return;
    if (existingRemotes.contains(remoteName)) {
        QMessageBox::warning(this, QStringLiteral("Remote already exists"),
            QStringLiteral("A remote named %1 already exists. Choose another name or remove that remote first.").arg(remoteName));
        return;
    }
    QStringList branchesToPush = defaultPushBranches;
    if (needsBranchSelection) {
        QDialog branchDialog(this);
        branchDialog.setWindowTitle(stepTitle(QStringLiteral("5"), QStringLiteral("Select local branches")));
        auto *layout = new QVBoxLayout(&branchDialog);
        auto *description = new QLabel(QStringLiteral("Choose which local branches to push to the new remote."), &branchDialog);
        description->setWordWrap(true);
        layout->addWidget(description);
        auto *branchChecksLayout = new QVBoxLayout;
        QList<QCheckBox *> branchChecks;
        QHash<QString, QCheckBox *> checksByBranch;
        for (const QString &branch : localBranches) {
            auto *check = new QCheckBox(branch, &branchDialog);
            check->setChecked(defaultPushBranches.contains(branch));
            branchChecks.append(check);
            checksByBranch.insert(branch, check);
            branchChecksLayout->addWidget(check);
        }
        layout->addLayout(branchChecksLayout);
        auto *quickActions = new QHBoxLayout;
        auto *selectAllButton = new QPushButton(QStringLiteral("Select All"), &branchDialog);
        auto *deselectAllButton = new QPushButton(QStringLiteral("Deselect All"), &branchDialog);
        quickActions->addWidget(selectAllButton);
        quickActions->addWidget(deselectAllButton);
        quickActions->addStretch(1);
        layout->addLayout(quickActions);
        auto *branchDialogButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &branchDialog);
        branchDialogButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Continue"));
        layout->addWidget(branchDialogButtons);
        connect(selectAllButton, &QPushButton::clicked, &branchDialog, [&branchChecks] {
            for (QCheckBox *check : branchChecks) check->setChecked(true);
        });
        connect(deselectAllButton, &QPushButton::clicked, &branchDialog, [&branchChecks] {
            for (QCheckBox *check : branchChecks) check->setChecked(false);
        });
        connect(branchDialogButtons, &QDialogButtonBox::accepted, &branchDialog, &QDialog::accept);
        connect(branchDialogButtons, &QDialogButtonBox::rejected, &branchDialog, &QDialog::reject);
        if (branchDialog.exec() != QDialog::Accepted) return;
        branchesToPush.clear();
        for (const QString &branch : localBranches) {
            if (checksByBranch.value(branch) && checksByBranch.value(branch)->isChecked())
                branchesToPush.append(branch);
        }
    }

    if (gh.isEmpty()) {
        QMessageBox prompt(this);
        prompt.setIcon(QMessageBox::Information);
        prompt.setWindowTitle(QStringLiteral("Set up GitHub CLI"));
        prompt.setText(QStringLiteral("GitHub CLI (gh) is needed to create this repository."));
        prompt.setInformativeText(QStringLiteral("Shell Scratchpad can install gh, open GitHub’s device sign-in page, copy the one-time code, then continue creating %1 automatically.").arg(repositoryName));
        auto *installButton = prompt.addButton(QStringLiteral("Install and Continue…"), QMessageBox::AcceptRole);
        prompt.addButton(QMessageBox::Cancel);
        prompt.exec();
        if (prompt.clickedButton() != installButton || !launchGitHubCliSetup(this)) return;
        gh = QStandardPaths::findExecutable(QStringLiteral("gh"));
        if (gh.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("GitHub CLI unavailable"),
                QStringLiteral("Sign-in completed, but Shell Scratchpad could not find gh. Restart Shell Scratchpad and try again."));
            return;
        }
    }
    const QString trackedLicense = gitOutput({QStringLiteral("ls-files"), QStringLiteral("--"),
        QStringLiteral("LICENSE"), QStringLiteral("LICENSE.md"), QStringLiteral("LICENSE.txt"),
        QStringLiteral("LICENCE"), QStringLiteral("LICENCE.md"), QStringLiteral("COPYING")});
    const bool licenseAlreadyInLocalRepository = !trackedLicense.isEmpty();
    const bool willCreateRemoteLicense = !license.isEmpty() && !licenseAlreadyInLocalRepository;
    const int totalProgressSteps = 6
        + (repositoryName.contains(QLatin1Char('/')) ? 0 : 1)
        + (license.isEmpty() ? 0 : 1)
        + (willCreateRemoteLicense ? 2 : 0)
        + (hasCommits ? branchesToPush.size() : 0);
    QProgressDialog progress(QStringLiteral("Preparing GitHub repository… (0%)"), QString(), 0, totalProgressSteps, this);
    progress.setWindowTitle(QStringLiteral("GitHub"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setValue(0);
    progress.show();
    int completedProgressSteps = 0;
    QString currentProgressLabel = QStringLiteral("Preparing GitHub repository…");
    const auto setProgressStep = [&progress, totalProgressSteps, &completedProgressSteps, &currentProgressLabel](const QString &label) {
        currentProgressLabel = label;
        const int percent = totalProgressSteps > 0 ? (completedProgressSteps * 100) / totalProgressSteps : 100;
        progress.setLabelText(QStringLiteral("%1 (%2%)").arg(label).arg(percent));
    };
    const auto completeProgressStep = [&progress, totalProgressSteps, &completedProgressSteps, &currentProgressLabel] {
        ++completedProgressSteps;
        progress.setValue(completedProgressSteps);
        const int percent = totalProgressSteps > 0 ? (completedProgressSteps * 100) / totalProgressSteps : 100;
        progress.setLabelText(QStringLiteral("%1 (%2%)").arg(currentProgressLabel).arg(percent));
    };
    auto runCommand = [this, &progress](const QString &program, const QStringList &arguments,
                                        QString *standardOutput, QString *standardError) {
        QProcess command;
        command.setProgram(program);
        command.setArguments(arguments);
        command.setWorkingDirectory(repoPath);
        command.start();
        if (!command.waitForStarted(3000)) {
            if (standardError) *standardError = command.errorString();
            return false;
        }
        QByteArray out;
        QByteArray err;
        while (command.state() != QProcess::NotRunning) {
            command.waitForFinished(100);
            out += command.readAllStandardOutput();
            err += command.readAllStandardError();
            QCoreApplication::processEvents();
        }
        out += command.readAllStandardOutput();
        err += command.readAllStandardError();
        if (standardOutput) *standardOutput = QString::fromLocal8Bit(out).trimmed();
        if (standardError) *standardError = QString::fromLocal8Bit(err).trimmed();
        return command.exitStatus() == QProcess::NormalExit && command.exitCode() == 0;
    };

    QString repositorySlug = repositoryName;
    if (!repositoryName.contains(QLatin1Char('/'))) {
        setProgressStep(QStringLiteral("Checking your GitHub account…"));
        QString login;
        QString error;
        if (!runCommand(gh, {QStringLiteral("api"), QStringLiteral("user"), QStringLiteral("--jq"), QStringLiteral(".login")}, &login, &error)
            || login.isEmpty()) {
            progress.close();
            QMessageBox::critical(this, QStringLiteral("Could not determine GitHub account"), error);
            return;
        }
        completeProgressStep();
        repositorySlug = login + QLatin1Char('/') + repositoryName;
    } else if (repositoryName.count(QLatin1Char('/')) != 1
               || repositoryName.startsWith(QLatin1Char('/')) || repositoryName.endsWith(QLatin1Char('/'))) {
        progress.close();
        QMessageBox::warning(this, QStringLiteral("Invalid repository name"),
            QStringLiteral("Use a repository name or owner/repository."));
        return;
    }

    QString licenseBody;
    if (!license.isEmpty()) {
        static const QRegularExpression validLicenseId(QStringLiteral("^[A-Za-z0-9.-]+$"));
        if (!validLicenseId.match(license).hasMatch()) {
            progress.close();
            QMessageBox::warning(this, QStringLiteral("Invalid license identifier"),
                QStringLiteral("Use a GitHub license identifier such as MPL-2.0."));
            return;
        }
        setProgressStep(QStringLiteral("Checking the %1 license template…").arg(license));
        QString error;
        if (!runCommand(gh, {QStringLiteral("api"), QStringLiteral("licenses/%1").arg(license.toLower()),
                             QStringLiteral("--jq"), QStringLiteral(".body")}, &licenseBody, &error)
            || licenseBody.isEmpty()) {
            progress.close();
            QMessageBox::critical(this, QStringLiteral("GitHub license unavailable"),
                error.isEmpty() ? QStringLiteral("GitHub did not return a license template for %1.").arg(license) : error);
            return;
        }
        completeProgressStep();
    }

    setProgressStep(QStringLiteral("Creating GitHub repository…"));
    QStringList createArguments{QStringLiteral("repo"), QStringLiteral("create"), repositorySlug,
        visibility == QStringLiteral("Private") ? QStringLiteral("--private") : QStringLiteral("--public")};
    QString output;
    QString error;
    if (!runCommand(gh, createArguments, &output, &error)) {
        progress.close();
        QMessageBox::critical(this, QStringLiteral("GitHub repository creation failed"),
            error.isEmpty() ? output : error);
        return;
    }
    completeProgressStep();

    const QString remoteUrl = QStringLiteral("https://github.com/%1.git").arg(repositorySlug);
    setProgressStep(QStringLiteral("Adding remote %1…").arg(remoteName));
    if (!runCommand(gitExecutable(), {QStringLiteral("remote"), QStringLiteral("add"), remoteName, remoteUrl}, nullptr, &error)) {
        progress.close();
        QMessageBox::critical(this, QStringLiteral("Repository created, but remote setup failed"), error);
        return;
    }
    completeProgressStep();
    QString licenseError;
    QString remoteBranch;
    if (!licenseBody.isEmpty() && !licenseAlreadyInLocalRepository) {
        QStringList licenseArguments{QStringLiteral("api"), QStringLiteral("repos/%1/contents/LICENSE").arg(repositorySlug),
            QStringLiteral("--method"), QStringLiteral("PUT"),
            QStringLiteral("--field"), QStringLiteral("message=Add %1 license").arg(license),
            QStringLiteral("--field"), QStringLiteral("content=%1").arg(QString::fromLatin1(licenseBody.toUtf8().toBase64()))};
        setProgressStep(QStringLiteral("Adding the %1 license…").arg(license));
        if (!runCommand(gh, licenseArguments, nullptr, &licenseError)) {
            if (licenseError.isEmpty()) licenseError = QStringLiteral("GitHub could not add the selected license file.");
        } else {
            completeProgressStep();
            setProgressStep(QStringLiteral("Checking the remote default branch…"));
            if (!runCommand(gh, {QStringLiteral("api"), QStringLiteral("repos/%1").arg(repositorySlug),
                                 QStringLiteral("--jq"), QStringLiteral(".default_branch")}, &remoteBranch, &licenseError)
                || remoteBranch.isEmpty()) {
                if (licenseError.isEmpty()) licenseError = QStringLiteral("GitHub did not return the repository's default branch.");
            } else completeProgressStep();
        }
    }

    // Pull the license commit into the local workspace before pushing any local
    // history. This also works for an unborn local branch: the pull establishes
    // its first commit, while a push is intentionally skipped below.
    QString pullError;
    bool pullPhaseCompleted = false;
    if (licenseError.isEmpty()) {
        QString remoteHeads;
        setProgressStep(QStringLiteral("Checking remote branches…"));
        if (!runCommand(gitExecutable(), {QStringLiteral("ls-remote"), QStringLiteral("--heads"), remoteName}, &remoteHeads, &pullError)) {
            if (pullError.isEmpty()) pullError = remoteHeads;
        } else {
            completeProgressStep();
        }
        if (pullError.isEmpty() && !remoteHeads.trimmed().isEmpty()) {
            if (remoteBranch.isEmpty()) {
                const QStringList refs = remoteHeads.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                const QString ref = refs.isEmpty() ? QString() : refs.first().section(QLatin1Char('\t'), 1).trimmed();
                remoteBranch = ref.startsWith(QStringLiteral("refs/heads/")) ? ref.mid(11) : QString();
            }
            if (remoteBranch.isEmpty()) {
                pullError = QStringLiteral("The remote has commits, but its branch could not be determined.");
            } else {
                setProgressStep(QStringLiteral("Pulling remote changes into the local repository…"));
                // This repository may inherit a user-wide pull.rebase or
                // pull.ff setting. Force a merge here: the remote license
                // commit and existing local history can be divergent.
                QStringList pullArguments{QStringLiteral("pull"), QStringLiteral("--no-rebase"),
                    QStringLiteral("--no-edit"), remoteName, remoteBranch};
                if (hasCommits) {
                    pullArguments.insert(1, QStringLiteral("--allow-unrelated-histories"));
                    // --ff explicitly overrides a pull.ff=only preference
                    // while still fast-forwarding whenever that is possible.
                    pullArguments.insert(1, QStringLiteral("--ff"));
                } else {
                    pullArguments.insert(1, QStringLiteral("--ff-only"));
                }
                if (!runCommand(gitExecutable(), pullArguments, &output, &pullError))
                    pullError = QStringLiteral("Could not pull remote changes into the local repository: %1")
                                    .arg(pullError.isEmpty() ? output : pullError);
                else {
                    completeProgressStep();
                    pullPhaseCompleted = true;
                }
            }
        } else if (pullError.isEmpty()) {
            completeProgressStep();
            pullPhaseCompleted = true;
        }
        // Count the pull phase even when the newly created remote has no refs.
        if (pullError.isEmpty() && !pullPhaseCompleted)
            completeProgressStep();
    }

    QStringList pushErrors;
    QStringList pushedBranches;
    if (pullError.isEmpty() && licenseError.isEmpty() && hasCommits) {
        for (const QString &branch : branchesToPush) {
            setProgressStep(QStringLiteral("Pushing branch %1…").arg(branch));
            QString branchError;
            if (!runCommand(gitExecutable(), {QStringLiteral("push"), QStringLiteral("--set-upstream"), remoteName, branch}, &output, &branchError)) {
                if (branchError.isEmpty()) branchError = output;
                pushErrors.append(QStringLiteral("%1: %2").arg(branch, branchError));
                completeProgressStep();
                continue;
            }
            pushedBranches.append(branch);
            completeProgressStep();
        }
    }

    QString fetchError;
    if (pullError.isEmpty() && licenseError.isEmpty()) {
        setProgressStep(QStringLiteral("Fetching all remotes…"));
        if (!runCommand(gitExecutable(), {QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")},
                        &output, &fetchError)
            && fetchError.isEmpty())
            fetchError = output;
        completeProgressStep();

        // Explicitly refresh the new remote's branch refs as well. This avoids
        // depending on a user's customized remote.fetch refspec for visibility
        // in the Branches panel after a push.
        QString targetFetchError;
        setProgressStep(QStringLiteral("Refreshing branches from %1…").arg(remoteName));
        const QString refspec = QStringLiteral("+refs/heads/*:refs/remotes/%1/*").arg(remoteName);
        if (!runCommand(gitExecutable(), {QStringLiteral("fetch"), QStringLiteral("--prune"), remoteName, refspec},
                        &output, &targetFetchError)
            && targetFetchError.isEmpty())
            targetFetchError = output;
        if (!targetFetchError.isEmpty()) {
            if (!fetchError.isEmpty()) fetchError += QLatin1Char('\n');
            fetchError += targetFetchError;
        }
        completeProgressStep();
    }

    progress.close();
    refreshAll();
    if (!pushErrors.isEmpty() || !licenseError.isEmpty() || !pullError.isEmpty() || !fetchError.isEmpty()) {
        QStringList details;
        if (!pushErrors.isEmpty()) details.append(QStringLiteral("Some branches could not be pushed:\n%1")
            .arg(pushErrors.join(QLatin1Char('\n'))));
        if (!pushedBranches.isEmpty()) details.append(QStringLiteral("Pushed successfully: %1")
            .arg(pushedBranches.join(QStringLiteral(", "))));
        if (!fetchError.isEmpty()) details.append(QStringLiteral("Could not fetch all remotes after pushing: %1").arg(fetchError));
        if (!licenseError.isEmpty()) details.append(QStringLiteral("License file could not be added: %1").arg(licenseError));
        if (!pullError.isEmpty()) details.append(pullError);
        QMessageBox::warning(this, QStringLiteral("Repository created with follow-up needed"),
            QStringLiteral("Created %1 on GitHub and added it as remote %2.\n\n%3")
                .arg(repositorySlug, remoteName, details.join(QStringLiteral("\n"))));
        return;
    }
    QString summary = QStringLiteral("Created %1 on GitHub and added it as remote %2.").arg(repositorySlug, remoteName);
    if (!license.isEmpty()) summary += QStringLiteral(" Added the %1 license.").arg(license);
    if (licenseAlreadyInLocalRepository) summary += QStringLiteral(" Kept the existing local license file.");
    if (!hasCommits) summary += QStringLiteral(" No commits were found, so nothing was pushed. Commit your files before pushing.");
    else if (pushedBranches.isEmpty()) summary += QStringLiteral(" No branches were selected, so nothing was pushed.");
    else if (!pushedBranches.isEmpty()) summary += QStringLiteral(" Pushed: %1.").arg(pushedBranches.join(QStringLiteral(", ")));
    QMessageBox::information(this, QStringLiteral("Repository created"), summary);
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

void GitWorkspaceWindow::createBranchFromHere(const QString &branch, bool remote)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Create Branch from Here"),
        QStringLiteral("New branch name (starting from %1):").arg(branch),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;

    int checkExitCode = -1;
    gitOutput({QStringLiteral("check-ref-format"), QStringLiteral("--branch"), name}, &checkExitCode);
    if (checkExitCode != 0) {
        QMessageBox::warning(this, QStringLiteral("Invalid branch name"),
            QStringLiteral("“%1” is not a valid Git branch name.").arg(name));
        return;
    }
    gitOutput({QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
               QStringLiteral("refs/heads/%1").arg(name)}, &checkExitCode);
    if (checkExitCode == 0) {
        QMessageBox::warning(this, QStringLiteral("Branch already exists"),
            QStringLiteral("A local branch named %1 already exists.").arg(name));
        return;
    }

    if (commandProcess->state() != QProcess::NotRunning || RepositoryOperationCoordinator::isBusy(repoPath)) {
        QMessageBox::information(this, QStringLiteral("Git is busy"),
            QStringLiteral("Wait for the current repository operation to finish before creating a branch."));
        return;
    }

    enum class DirtyAction { None, StashAndPop, StashKeep, Commit, Cancel };
    DirtyAction dirtyAction = DirtyAction::None;
    const QString status = gitOutput({QStringLiteral("status"), QStringLiteral("--porcelain=v1")});
    if (!status.isEmpty()) {
        QMessageBox choice(this);
        choice.setIcon(QMessageBox::Question);
        choice.setWindowTitle(QStringLiteral("Changes on Current Branch"));
        choice.setText(QStringLiteral("There are staged, unstaged, or untracked changes on the current branch."));
        choice.setInformativeText(QStringLiteral("Choose how to handle all changes before switching to %1 and creating %2.")
                                      .arg(branch, name));
        QPushButton *stashAndPop = choice.addButton(QStringLiteral("Stash and Apply on New Branch"), QMessageBox::AcceptRole);
        QPushButton *stashKeep = choice.addButton(QStringLiteral("Stash and Keep Stashed"), QMessageBox::ActionRole);
        QPushButton *stageCommit = choice.addButton(QStringLiteral("Stage All and Commit…"), QMessageBox::ActionRole);
        QPushButton *cancel = choice.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        choice.setDefaultButton(stashAndPop);
        choice.exec();
        if (choice.clickedButton() == stashAndPop) dirtyAction = DirtyAction::StashAndPop;
        else if (choice.clickedButton() == stashKeep) dirtyAction = DirtyAction::StashKeep;
        else if (choice.clickedButton() == stageCommit) dirtyAction = DirtyAction::Commit;
        else if (choice.clickedButton() == cancel) dirtyAction = DirtyAction::Cancel;
        if (dirtyAction == DirtyAction::Cancel || dirtyAction == DirtyAction::None) return;
    }

    QString commitMessage;
    if (dirtyAction == DirtyAction::Commit) {
        commitMessage = QInputDialog::getMultiLineText(this, QStringLiteral("Commit Changes Before Branching"),
            QStringLiteral("Commit message:"), QString(), &accepted).trimmed();
        if (!accepted) return;
        if (commitMessage.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Commit message required"),
                QStringLiteral("Enter a commit message before creating the branch."));
            return;
        }
    }

    if (!RepositoryOperationCoordinator::tryAcquire(repoPath)) {
        QMessageBox::information(this, QStringLiteral("Repository is busy"),
            QStringLiteral("Another Git operation is running in this repository."));
        return;
    }

    QProgressDialog progress(QStringLiteral("Preparing branch…"), QString(), 0, 0, this);
    progress.setWindowTitle(QStringLiteral("Create Branch"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    setGitBusy(true, true);
    progress.show();

    const auto run = [this, &progress](const QStringList &arguments, QString *detail) {
        QProcess process;
        process.setProgram(gitExecutable());
        process.setArguments(arguments);
        process.setWorkingDirectory(repoPath);
        process.start();
        if (!process.waitForStarted(3000)) {
            if (detail) *detail = process.errorString();
            return false;
        }
        QByteArray standardOutput;
        QByteArray standardError;
        while (process.state() != QProcess::NotRunning) {
            process.waitForFinished(100);
            standardOutput += process.readAllStandardOutput();
            standardError += process.readAllStandardError();
            QCoreApplication::processEvents();
        }
        standardOutput += process.readAllStandardOutput();
        standardError += process.readAllStandardError();
        const bool succeeded = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        if (!succeeded && detail) {
            *detail = QString::fromLocal8Bit(standardError).trimmed();
            if (detail->isEmpty()) *detail = QString::fromLocal8Bit(standardOutput).trimmed();
            if (detail->isEmpty()) *detail = QStringLiteral("git %1 failed.").arg(arguments.join(QLatin1Char(' ')));
        }
        return succeeded;
    };

    QString failure;
    QString stashObject;
    if (dirtyAction == DirtyAction::StashAndPop || dirtyAction == DirtyAction::StashKeep) {
        progress.setLabelText(QStringLiteral("Stashing staged, unstaged, and untracked changes…"));
        if (!run({QStringLiteral("stash"), QStringLiteral("push"), QStringLiteral("--include-untracked"),
                  QStringLiteral("--message"), QStringLiteral("Shell Scratchpad branch creation: %1").arg(name)}, &failure)) {
            // No branch switch has happened, so the user's working tree is unchanged.
        } else {
            stashObject = gitOutput({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("refs/stash")}).trimmed();
            if (dirtyAction == DirtyAction::StashAndPop && stashObject.isEmpty())
                failure = QStringLiteral("Git created a stash, but its saved changes could not be identified for automatic restoration. The stash is preserved.");
        }
    } else if (dirtyAction == DirtyAction::Commit) {
        progress.setLabelText(QStringLiteral("Staging all changes…"));
        if (run({QStringLiteral("add"), QStringLiteral("--all")}, &failure)) {
            progress.setLabelText(QStringLiteral("Committing changes…"));
            run({QStringLiteral("commit"), QStringLiteral("-m"), commitMessage}, &failure);
        }
    }

    const QString currentBranch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")}).trimmed();
    if (failure.isEmpty() && !remote && currentBranch != branch) {
        progress.setLabelText(QStringLiteral("Switching to %1…").arg(branch));
        run({QStringLiteral("switch"), branch}, &failure);
    } else if (failure.isEmpty() && remote) {
        progress.setLabelText(QStringLiteral("Switching to remote branch %1…").arg(branch));
        run({QStringLiteral("switch"), QStringLiteral("--detach"),
             QStringLiteral("refs/remotes/%1").arg(branch)}, &failure);
    }

    bool branchCreated = false;
    if (failure.isEmpty()) {
        progress.setLabelText(QStringLiteral("Creating and switching to %1…").arg(name));
        branchCreated = run({QStringLiteral("switch"), QStringLiteral("--create"), name}, &failure);
    }

    QString stashRestoreError;
    if (branchCreated && dirtyAction == DirtyAction::StashAndPop && !stashObject.isEmpty()) {
        QString stashReference;
        const QString stashList = gitOutput({QStringLiteral("stash"), QStringLiteral("list"),
            QStringLiteral("--format=%gd%x09%H")});
        for (const QString &line : stashList.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList fields = line.split(QLatin1Char('\t'));
            if (fields.size() == 2 && fields.at(1).trimmed() == stashObject) {
                stashReference = fields.at(0).trimmed();
                break;
            }
        }
        if (stashReference.isEmpty()) {
            stashRestoreError = QStringLiteral("The branch was created, but the saved changes could not be located in the stash list.");
        } else {
            progress.setLabelText(QStringLiteral("Applying saved changes on %1…").arg(name));
            run({QStringLiteral("stash"), QStringLiteral("pop"), QStringLiteral("--index"), stashReference}, &stashRestoreError);
        }
    }

    progress.close();
    setGitBusy(false);
    RepositoryOperationCoordinator::release(repoPath);
    refreshAll();

    if (!failure.isEmpty()) {
        QString message = QStringLiteral("Could not create branch %1 from %2:\n\n%3").arg(name, branch, failure);
        if (!stashObject.isEmpty())
            message += QStringLiteral("\n\nYour changes remain safely saved in Git stash.");
        QMessageBox::warning(this, QStringLiteral("Branch creation failed"), message);
        return;
    }
    if (!stashRestoreError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Branch created; saved changes need attention"),
            QStringLiteral("Created and switched to %1, but Git could not apply the saved changes:\n\n%2\n\nThe stash was kept; you can apply it from the stash controls.")
                .arg(name, stashRestoreError));
        return;
    }
    if (dirtyAction == DirtyAction::StashKeep)
        statusBar()->showMessage(QStringLiteral("Created and switched to %1. Your changes remain in Git stash.").arg(name), 8000);
    else
        statusBar()->showMessage(QStringLiteral("Created and switched to branch %1.").arg(name), 6000);
}

void GitWorkspaceWindow::showBranchContextMenu(const QString &branch, bool remote, const QPoint &globalPosition)
{
    if (branch.isEmpty()) return;
    QMenu menu(this);
    const QString currentBranch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
    QAction *createBranch = menu.addAction(QStringLiteral("Create New Branch from Here…"));
    connect(createBranch, &QAction::triggered, this, [this, branch, remote] {
        createBranchFromHere(branch, remote);
    });
    menu.addSeparator();
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

void GitWorkspaceWindow::showMergeConflictsDialog()
{
    if (conflictedFiles.isEmpty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Merge Conflicts Need Attention"));
    dialog.resize(560, 430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *heading = new QLabel(&dialog);
    heading->setTextFormat(Qt::RichText);
    heading->setText(QStringLiteral("<span style='color:#b42318;font-size:16pt;font-weight:bold'>Git found merge conflicts.</span><br>"
                                   "Git could not complete the merge automatically. Resolve every listed file, then commit to finish the merge."));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *fileList = new QListWidget(&dialog);
    fileList->setMinimumHeight(220);
    layout->addWidget(fileList, 1);
    auto *progress = new QLabel(&dialog);
    progress->setWordWrap(true);
    layout->addWidget(progress);

    auto *buttonRow = new QHBoxLayout;
    auto *applyIncoming = new QPushButton(QStringLiteral("Apply All Incoming"), &dialog);
    auto *applyMine = new QPushButton(QStringLiteral("Apply All Mine"), &dialog);
    auto *resolveSelected = new QPushButton(QStringLiteral("Resolve Selected…"), &dialog);
    auto *resolveNext = new QPushButton(QStringLiteral("Resolve Next Conflict…"), &dialog);
    auto *close = new QPushButton(QStringLiteral("Close"), &dialog);
    buttonRow->addWidget(applyIncoming);
    buttonRow->addWidget(applyMine);
    buttonRow->addWidget(resolveSelected);
    buttonRow->addWidget(resolveNext);
    buttonRow->addStretch(1);
    buttonRow->addWidget(close);
    layout->addLayout(buttonRow);

    const auto updateConflictList = [this, fileList, progress, resolveSelected, resolveNext, applyIncoming, applyMine] {
        const QString selectedPath = fileList->currentItem()
            ? fileList->currentItem()->data(Qt::UserRole).toString() : QString();
        fileList->clear();
        for (const QString &path : conflictedFiles) {
            auto *item = new QListWidgetItem(path, fileList);
            item->setData(Qt::UserRole, path);
            if (path == selectedPath) fileList->setCurrentItem(item);
        }
        if (fileList->count() > 0 && !fileList->currentItem())
            fileList->setCurrentRow(0);
        const bool remaining = !conflictedFiles.isEmpty();
        progress->setText(remaining
            ? QStringLiteral("%1 file(s) still have unresolved conflict markers. Resolve them one at a time.").arg(conflictedFiles.size())
            : QStringLiteral("All file conflicts are resolved and staged. Commit the merge to complete it."));
        const bool canResolve = remaining && commandProcess->state() == QProcess::NotRunning;
        resolveSelected->setEnabled(canResolve);
        resolveNext->setEnabled(canResolve);
        applyIncoming->setEnabled(canResolve);
        applyMine->setEnabled(canResolve);
    };
    updateConflictList();

    const auto openResolver = [this, fileList, resolveSelected, resolveNext](bool firstUnresolved) {
        QListWidgetItem *item = firstUnresolved ? fileList->item(0) : fileList->currentItem();
        if (!item && fileList->count() > 0) item = fileList->item(0);
        if (!item) return;
        resolveConflictFile(item->data(Qt::UserRole).toString());
        if (commandProcess->state() != QProcess::NotRunning) {
            resolveSelected->setEnabled(false);
            resolveNext->setEnabled(false);
        }
    };
    connect(resolveSelected, &QPushButton::clicked, &dialog, [openResolver] { openResolver(false); });
    connect(resolveNext, &QPushButton::clicked, &dialog, [openResolver] { openResolver(true); });
    const auto applySideToAll = [this, &dialog, updateConflictList](bool incoming) {
        if (conflictedFiles.isEmpty() || commandProcess->state() != QProcess::NotRunning) return;
        const QString side = incoming ? QStringLiteral("incoming") : QStringLiteral("mine");
        const QString choice = incoming ? QStringLiteral("theirs") : QStringLiteral("ours");
        const QString warning = QStringLiteral("This will replace the conflicted contents in all %1 listed file(s) with %2 and stage those files. Any conflict resolution already made in these files will be overwritten.")
            .arg(conflictedFiles.size()).arg(side);
        if (QMessageBox::question(&dialog, QStringLiteral("Apply %1 to every conflict?").arg(side), warning,
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QMessageBox finalCheck(QMessageBox::Warning, QStringLiteral("Confirm bulk conflict resolution"),
            QStringLiteral("Final confirmation: replace and stage all %1 conflicted files using %2?").arg(conflictedFiles.size()).arg(side),
            QMessageBox::NoButton, &dialog);
        QPushButton *apply = finalCheck.addButton(QStringLiteral("Apply %1 to All").arg(incoming ? QStringLiteral("Incoming") : QStringLiteral("Mine")), QMessageBox::DestructiveRole);
        finalCheck.addButton(QMessageBox::Cancel);
        finalCheck.setDefaultButton(qobject_cast<QPushButton *>(finalCheck.button(QMessageBox::Cancel)));
        finalCheck.exec();
        if (finalCheck.clickedButton() != apply) return;

        QStringList checkoutArgs{QStringLiteral("checkout"), QStringLiteral("--") + choice, QStringLiteral("--")};
        checkoutArgs.append(conflictedFiles);
        int exitCode = -1;
        const QString output = gitOutput(checkoutArgs, &exitCode);
        if (exitCode != 0) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not apply %1").arg(side), output);
            return;
        }
        QStringList addArgs{QStringLiteral("add"), QStringLiteral("--")};
        addArgs.append(conflictedFiles);
        const QString addOutput = gitOutput(addArgs, &exitCode);
        if (exitCode != 0) {
            QMessageBox::warning(&dialog, QStringLiteral("Could not stage resolved files"), addOutput);
        }
        refreshAll();
        updateConflictList();
    };
    connect(applyIncoming, &QPushButton::clicked, &dialog, [applySideToAll] { applySideToAll(true); });
    connect(applyMine, &QPushButton::clicked, &dialog, [applySideToAll] { applySideToAll(false); });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(commandProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &dialog,
            [updateConflictList](int, QProcess::ExitStatus) { updateConflictList(); });
    dialog.exec();
}

void GitWorkspaceWindow::showCommitDialog()
{
    if (!hasStagedChanges) return;
    if (commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git is busy"),
            QStringLiteral("Wait for the current Git command to finish before committing."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Commit Staged Changes"));
    dialog.resize(620, 420);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Commit message (use multiple lines for a longer description):"), &dialog));
    auto *messageEdit = new QPlainTextEdit(&dialog);
    messageEdit->setPlaceholderText(QStringLiteral("Summary line\n\nOptional commit description"));
    layout->addWidget(messageEdit, 1);
    auto *buttonRow = new QHBoxLayout;
    auto *commitPush = new QPushButton(QStringLiteral("Commit and Push"), &dialog);
    auto *commitOnly = new QPushButton(QStringLiteral("Commit"), &dialog);
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), &dialog);
    buttonRow->addStretch(1);
    buttonRow->addWidget(commitPush);
    buttonRow->addWidget(commitOnly);
    buttonRow->addWidget(cancel);
    layout->addLayout(buttonRow);

    QString message;
    bool pushAfterCommit = false;
    const auto acceptCommit = [&dialog, messageEdit, &message](bool push) {
        message = messageEdit->toPlainText().trimmed();
        if (message.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Commit message required"),
                QStringLiteral("Enter a commit message before committing."));
            messageEdit->setFocus();
            return;
        }
        dialog.setProperty("pushAfterCommit", push);
        dialog.accept();
    };
    connect(commitPush, &QPushButton::clicked, &dialog, [&acceptCommit] { acceptCommit(true); });
    connect(commitOnly, &QPushButton::clicked, &dialog, [&acceptCommit] { acceptCommit(false); });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    messageEdit->installEventFilter(new CommitMessageShortcutFilter(messageEdit,
        [&acceptCommit] { acceptCommit(false); }, [&acceptCommit] { acceptCommit(true); }));
    commitOnly->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    commitOnly->setToolTip(QStringLiteral("Ctrl+Enter"));
    commitPush->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
    commitPush->setToolTip(QStringLiteral("Ctrl+Shift+Enter"));
    messageEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    pushAfterCommit = dialog.property("pushAfterCommit").toBool();
    commitPushPending = pushAfterCommit;
    runGit({QStringLiteral("commit"), QStringLiteral("-m"), message});
}

void GitWorkspaceWindow::refreshIdentity()
{
    userNameEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.name")}));
    emailEdit->setText(gitOutput({QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.email")}));
    const QString helper = gitOutput({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--get"), QStringLiteral("credential.helper")});
    if (helper.contains(QStringLiteral("git-credential-kwallet"))) {
        credentialHelperCombo->setCurrentIndex(credentialHelperCombo->findData(QStringLiteral("kwallet")));
        const QRegularExpression walletExpression(QStringLiteral("--wallet\\s+'([^']*)'\\s+--folder\\s+'([^']*)'"));
        const QRegularExpressionMatch match = walletExpression.match(helper);
        if (match.hasMatch()) {
            credentialWalletEdit->setText(match.captured(1));
            credentialFolderEdit->setText(match.captured(2));
        }
    }
}

void GitWorkspaceWindow::runGit(const QStringList &arguments)
{
    if (commandProcess->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QStringLiteral("Git is busy"), QStringLiteral("Wait for the current Git command to finish."));
        return;
    }
    if (RepositoryOperationCoordinator::isBusy(repoPath)) {
        QMessageBox::information(this, QStringLiteral("Repository is busy"), QStringLiteral("Another Git operation is running in this repository."));
        return;
    }
    if (!arguments.isEmpty() && arguments.first() == QLatin1String("push") && !arguments.contains(QStringLiteral("--delete"))) {
        const QString currentBranch = gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")});
        const QStringList remoteNames = gitOutput({QStringLiteral("remote")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const bool explicitRefspec = arguments.size() >= 3 && !arguments.last().startsWith(QLatin1Char('-'))
            && !remoteNames.contains(arguments.last());
        const bool pushesCurrentBranch = !explicitRefspec || arguments.last() == currentBranch || arguments.last() == QLatin1String("HEAD");
        if (pushesCurrentBranch && hasUpstream) {
            bool countOk = false;
            const int incomingCount = gitOutput({QStringLiteral("rev-list"), QStringLiteral("--count"),
                QStringLiteral("HEAD..@{upstream}")}).toInt(&countOk);
            if (countOk && incomingCount > 0) {
                QMessageBox prompt(this);
                prompt.setIcon(QMessageBox::Warning);
                prompt.setWindowTitle(QStringLiteral("Upstream Commits Need Pulling"));
                prompt.setText(QStringLiteral("Your upstream branch has %1 commit(s) that are not in this repository.").arg(incomingCount));
                prompt.setInformativeText(QStringLiteral("Sync with the upstream using Pull with Rebase before pushing?"));
                QPushButton *syncAndPush = prompt.addButton(QStringLiteral("Sync Then Push"), QMessageBox::AcceptRole);
                prompt.addButton(QStringLiteral("Do Nothing"), QMessageBox::RejectRole);
                prompt.setDefaultButton(syncAndPush);
                prompt.exec();
                if (prompt.clickedButton() == syncAndPush) {
                    pushAfterSyncPending = true;
                    deferredPushArguments = arguments;
                    runGit({QStringLiteral("pull"), QStringLiteral("--rebase")});
                }
                return;
            }
        }
    }
    QStringList fullArguments{QStringLiteral("-C"), repoPath};
    fullArguments.append(arguments);
    commandProcess->setArguments(fullArguments);
    commandProcess->setProgram(gitExecutable());
    commandProcess->setWorkingDirectory(repoPath);
    actionCommandActive = true;
    hadConflictsBeforeCommand = hasConflicts;
    actionOutput.clear();
    statusBar()->showMessage(QStringLiteral("Running git %1…").arg(arguments.join(QLatin1Char(' '))));
    const QString operation = arguments.value(0);
    const bool showsProgress = operation == QLatin1String("clone") || operation == QLatin1String("fetch")
        || operation == QLatin1String("pull") || operation == QLatin1String("push")
        || operation == QLatin1String("submodule");
    if (showsProgress && (operation == QLatin1String("fetch") || operation == QLatin1String("pull")
        || operation == QLatin1String("push")) && !arguments.contains(QStringLiteral("--progress")))
        commandProcess->setArguments(fullArguments + QStringList{QStringLiteral("--progress")});
    if (!RepositoryOperationCoordinator::tryAcquire(repoPath)) {
        QMessageBox::information(this, QStringLiteral("Repository is busy"), QStringLiteral("Another Git operation started in this repository."));
        return;
    }
    operationLockPath = repoPath;
    operationLockHeld = true;
    setGitBusy(true, showsProgress);
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
    if (args.first() == QLatin1String("push")) {
        runGit(args);
        return;
    }
    if (RepositoryOperationCoordinator::isBusy(repoPath)
        || !RepositoryOperationCoordinator::tryAcquire(repoPath)) {
        statusBar()->showMessage(QStringLiteral("Another Git operation is running in this repository."), 4000);
        return;
    }
    operationLockPath = repoPath;
    operationLockHeld = true;
    appendOutput(QStringLiteral("$ git %1\n").arg(args.join(QLatin1Char(' '))));
    actionCommandActive = false;
    commandProcess->setProgram(gitExecutable());
    commandProcess->setArguments(args);
    commandProcess->setWorkingDirectory(repoPath);
    const QString operation = args.value(0);
    const bool showsProgress = operation == QLatin1String("clone") || operation == QLatin1String("fetch")
        || operation == QLatin1String("pull") || operation == QLatin1String("push")
        || operation == QLatin1String("submodule");
    if (showsProgress && (operation == QLatin1String("fetch") || operation == QLatin1String("pull")
        || operation == QLatin1String("push")) && !args.contains(QStringLiteral("--progress")))
        args.append(QStringLiteral("--progress"));
    commandProcess->setArguments(args);
    setGitBusy(true, showsProgress);
    if (rightTabs->indexOf(commandPanel) < 0) rightTabs->addTab(commandPanel, QStringLiteral("Git Command"));
    rightTabs->setCurrentWidget(commandPanel);
    commandPanel->show();
    hadConflictsBeforeCommand = hasConflicts;
    commandProcess->start();
}

void GitWorkspaceWindow::setGitBusy(bool busy, bool showProgress)
{
    if (busy) {
        busyDisabledWidgets.clear();
        const auto disableIfEnabled = [this](QWidget *widget) {
            if (!widget || !widget->isEnabled()) return;
            busyDisabledWidgets.append(widget);
            widget->setEnabled(false);
        };
        const auto buttons = findChildren<QAbstractButton *>();
        for (QAbstractButton *button : buttons) disableIfEnabled(button);
        disableIfEnabled(commandEditor);
        disableIfEnabled(commandInput);
        disableIfEnabled(userNameEdit);
        disableIfEnabled(emailEdit);
        disableIfEnabled(credentialHelperCombo);
    } else {
        for (const QPointer<QWidget> &widget : busyDisabledWidgets) {
            if (widget) widget->setEnabled(true);
        }
        busyDisabledWidgets.clear();
        updateToolbarState();
    }
    if (gitActionsMenu) gitActionsMenu->setEnabled(!busy);
    if (gitProgressBar) {
        if (busy && showProgress) {
            gitProgressBar->setRange(0, 0);
            gitProgressBar->setFormat(QStringLiteral("Git is working…"));
            gitProgressBar->show();
        } else {
            gitProgressBar->hide();
        }
    }
}

void GitWorkspaceWindow::updateGitProgress(const QString &output)
{
    if (!gitProgressBar || !gitProgressBar->isVisible()) return;
    static const QRegularExpression percentage(QStringLiteral("(\\d{1,3})%"));
    auto matches = percentage.globalMatch(output);
    while (matches.hasNext()) {
        const int value = qBound(0, matches.next().captured(1).toInt(), 100);
        gitProgressBar->setRange(0, 100);
        gitProgressBar->setValue(value);
        gitProgressBar->setFormat(QStringLiteral("Git progress: %p%"));
    }
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
    currentDiffIsImage = false;
    currentDiffIsSvg = false;
    svgViewModePanel->hide();
    changeGroupsList->show();
    originalViewStack->setCurrentWidget(originalFileView);
    workingViewStack->setCurrentWidget(workingFileView);
    commitMessageView->clear();
    commitMessageView->hide();
    QFile snapshotFile(QDir(repoPath).filePath(path));
    lastObservedDiffFile = snapshotFile.open(QIODevice::ReadOnly)
        ? QString::fromLatin1(QCryptographicHash::hash(snapshotFile.readAll(), QCryptographicHash::Sha256).toHex())
        : QString();
    diffFilesList->clear();
    changeGroupsList->clear();
    auto *fileItem = new QListWidgetItem(path, diffFilesList);
    fileItem->setData(Qt::UserRole, path);
    sizeFileListToRows(diffFilesList);
    diffSummaryLabel->setText(path);
    originalDiffLabel->setText(QStringLiteral("Latest in repository (HEAD)"));
    changedDiffLabel->setText(QStringLiteral("Working file"));
    QByteArray originalBytes;
    bool originalMissing = true;
    QProcess originalProcess;
    originalProcess.setProgram(gitExecutable());
    originalProcess.setArguments({QStringLiteral("show"), QStringLiteral("HEAD:") + path});
    originalProcess.setWorkingDirectory(repoPath);
    originalProcess.start();
    if (originalProcess.waitForFinished(10000) && originalProcess.exitCode() == 0) {
        originalBytes = originalProcess.readAllStandardOutput();
        originalMissing = false;
    }

    QByteArray workingBytes;
    bool workingMissing = true;
    QFile workingFile(QDir(repoPath).filePath(path));
    if (workingFile.open(QIODevice::ReadOnly)) {
        workingBytes = workingFile.readAll();
        workingMissing = false;
    }
    if (isImageFilePath(path, originalBytes) || isImageFilePath(path, workingBytes)) {
        showImageDiff(path, originalBytes, workingBytes, originalMissing, workingMissing);
        return;
    }
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
    populateChangeGroups(changeGroupsList, rows);
    detailsStack->setCurrentWidget(detailsStack->widget(1));
}

void GitWorkspaceWindow::showImageDiff(const QString &path, const QByteArray &originalBytes,
                                       const QByteArray &workingBytes, bool originalMissing, bool workingMissing)
{
    currentDiffIsImage = true;
    currentDiffIsSvg = QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0
        || isSvgDocument(originalBytes) || isSvgDocument(workingBytes);
    currentImageDiffPath = path;
    originalImageDiffBytes = originalBytes;
    workingImageDiffBytes = workingBytes;
    originalImageMissing = originalMissing;
    workingImageMissing = workingMissing;
    svgViewModePanel->setVisible(currentDiffIsSvg);
    changeGroupsList->hide();
    svgViewModeCombo->blockSignals(true);
    svgViewModeCombo->setCurrentIndex(0);
    svgViewModeCombo->blockSignals(false);
    diffSummaryLabel->setText(path);
    displayImageDiff();
}

void GitWorkspaceWindow::displayImageDiff()
{
    if (!currentDiffIsImage) return;
    if (currentDiffIsSvg && svgViewModeCombo->currentIndex() == 1) {
        originalFileView->setPlainText(originalImageMissing ? QStringLiteral("No previous SVG version.")
            : QString::fromUtf8(originalImageDiffBytes));
        workingFileView->setPlainText(workingImageMissing ? QStringLiteral("SVG removed from this version.")
            : QString::fromUtf8(workingImageDiffBytes));
        originalViewStack->setCurrentWidget(originalFileView);
        workingViewStack->setCurrentWidget(workingFileView);
    } else {
        if (currentDiffIsSvg) {
            if (originalImageMissing) originalImageView->setImage({}, QStringLiteral("No previous SVG version"));
            else originalImageView->setSvgData(originalImageDiffBytes, currentImageDiffPath,
                QStringLiteral("Could not render original SVG"));
            if (workingImageMissing) workingImageView->setImage({}, QStringLiteral("SVG removed in this version"));
            else workingImageView->setSvgData(workingImageDiffBytes, currentImageDiffPath,
                QStringLiteral("Could not render changed SVG"));
        } else {
            QString originalError;
            QString workingError;
            const QImage originalImage = originalImageMissing ? QImage()
                : decodeImage(originalImageDiffBytes, currentImageDiffPath, &originalError);
            const QImage workingImage = workingImageMissing ? QImage()
                : decodeImage(workingImageDiffBytes, currentImageDiffPath, &workingError);
            originalImageView->setImage(originalImage, originalImageMissing
                ? QStringLiteral("No previous image version")
                : (originalError.isEmpty() ? QStringLiteral("Could not render original image") : originalError));
            workingImageView->setImage(workingImage, workingImageMissing
                ? QStringLiteral("Image removed in this version")
                : (workingError.isEmpty() ? QStringLiteral("Could not render changed image") : workingError));
        }
        originalViewStack->setCurrentWidget(originalImageView);
        workingViewStack->setCurrentWidget(workingImageView);
    }
    detailsStack->setCurrentWidget(detailsStack->widget(1));
}

void GitWorkspaceWindow::showCommit(const QString &hash)
{
    if (hash.isEmpty()) return;
    currentDiffCommitHash = hash;
    commitMessageView->setPlainText(gitOutput({QStringLiteral("show"), QStringLiteral("-s"),
        QStringLiteral("--format=%B"), hash}));
    commitMessageView->setVisible(!commitMessageView->toPlainText().isEmpty());
    diffFilesList->clear();
    changeGroupsList->clear();
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
    sizeFileListToRows(diffFilesList);
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
    currentDiffIsImage = false;
    currentDiffIsSvg = false;
    svgViewModePanel->hide();
    changeGroupsList->show();
    originalViewStack->setCurrentWidget(originalFileView);
    workingViewStack->setCurrentWidget(workingFileView);
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
    if (isImageFilePath(path, originalBytes) || isImageFilePath(path, modifiedBytes)) {
        showImageDiff(path, originalBytes, modifiedBytes,
            parentCode != 0 || originalBytes.isEmpty(), modifiedBytes.isEmpty());
        return;
    }
    if (originalBytes.contains('\0') || modifiedBytes.contains('\0')) {
        detailsView->setPlainText(QStringLiteral("This file contains binary data and cannot be shown as a text diff."));
        detailsStack->setCurrentWidget(detailsView);
        return;
    }
    const QVector<DiffLine> rows = makeSideBySideDiff(QString::fromUtf8(originalBytes), QString::fromUtf8(modifiedBytes));
    renderDiffSide(originalFileView, rows, true);
    renderDiffSide(workingFileView, rows, false);
    populateChangeGroups(changeGroupsList, rows);
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
    QString helper = credentialHelperCombo->currentData().toString();
    if (helper.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Credential helper"),
            gitOutput({QStringLiteral("config"), QStringLiteral("--show-origin"), QStringLiteral("--get-all"), QStringLiteral("credential.helper")}));
        return;
    }
    if (helper == QLatin1String("kwallet")) {
        const QString helperPath = kwalletCredentialHelperPath();
        if (helperPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("KWallet helper unavailable"),
                QStringLiteral("The KWallet Git credential helper was not found. Rebuild or reinstall Shell Scratchpad to install it."));
            return;
        }
        const QString wallet = credentialWalletEdit->text().trimmed();
        const QString folder = credentialFolderEdit->text().trimmed();
        if (wallet.isEmpty() || folder.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("KWallet location required"), QStringLiteral("Enter both a wallet and folder name."));
            return;
        }
        const auto quote = [](QString value) {
            value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            return QLatin1Char('\'') + value + QLatin1Char('\'');
        };
        helper = QStringLiteral("!") + quote(helperPath) + QStringLiteral(" --wallet ") + quote(wallet)
            + QStringLiteral(" --folder ") + quote(folder);
    }
    int code = -1;
    const QString output = gitOutput({QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--replace-all"),
        QStringLiteral("credential.helper"), helper}, &code);
    statusBar()->showMessage(code == 0 ? QStringLiteral("Credential helper configured") : output, 5000);
}

void GitWorkspaceWindow::createCredentialWallet()
{
    const QString wallet = credentialWalletEdit->text().trimmed();
    const QString folder = credentialFolderEdit->text().trimmed();
    if (wallet.isEmpty() || folder.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("KWallet location required"), QStringLiteral("Enter both a wallet and folder name."));
        return;
    }
    const QString helperPath = kwalletCredentialHelperPath();
    if (helperPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("KWallet helper unavailable"),
            QStringLiteral("The KWallet Git credential helper was not found. Rebuild or reinstall Shell Scratchpad to install it."));
        return;
    }
    auto *process = new QProcess(this);
    process->setProgram(helperPath);
    process->setArguments({QStringLiteral("--create-wallet"), QStringLiteral("--wallet"), wallet,
        QStringLiteral("--folder"), folder});
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, wallet](int exitCode, QProcess::ExitStatus) {
            const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            if (exitCode == 0)
                statusBar()->showMessage(QStringLiteral("KWallet “%1” is ready. Configure KWallet as the credential helper to use it.").arg(wallet), 6000);
            else QMessageBox::warning(this, QStringLiteral("Could not create or open KWallet"),
                error.isEmpty() ? QStringLiteral("KWallet could not open the requested wallet.") : error);
            process->deleteLater();
        });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        QMessageBox::warning(this, QStringLiteral("Could not start KWallet helper"), process->errorString());
        process->deleteLater();
    });
    process->start();
    statusBar()->showMessage(QStringLiteral("Opening KWallet; enter a new password if prompted…"), 5000);
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
    dialog.setWindowTitle(QStringLiteral("Manage Git Access Tokens"));
    dialog.setMinimumSize(620, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *protocol = new QComboBox(&dialog);
    protocol->addItem(QStringLiteral("HTTPS"), QStringLiteral("https"));
    protocol->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    auto *host = new QLineEdit(&dialog);
    host->setPlaceholderText(QStringLiteral("github.com"));
    auto *path = new QLineEdit(&dialog);
    path->setPlaceholderText(QStringLiteral("Optional; use for repository-specific tokens"));
    auto *username = new QLineEdit(&dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(QStringLiteral("Enter a token to add or replace it"));
    form->addRow(QStringLiteral("Service"), protocol);
    form->addRow(QStringLiteral("Host"), host);
    form->addRow(QStringLiteral("Repository path (optional)"), path);
    form->addRow(QStringLiteral("Username"), username);
    form->addRow(QStringLiteral("Access token"), password);
    layout->addLayout(form);
    layout->addWidget(new QLabel(QStringLiteral("Saved token contexts (token values stay in Git’s configured credential helper):"), &dialog));
    auto *savedContexts = new QListWidget(&dialog);
    layout->addWidget(savedContexts, 1);
    auto *actions = new QHBoxLayout;
    auto *saveButton = new QPushButton(QStringLiteral("Add / Replace Token"), &dialog);
    auto *removeButton = new QPushButton(QStringLiteral("Remove Selected Token"), &dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
    actions->addWidget(saveButton);
    actions->addWidget(removeButton);
    actions->addStretch(1);
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    const auto contextText = [](const QStringList &parts) {
        const QString service = parts.value(0) + QStringLiteral("://") + parts.value(1);
        const QString pathPart = parts.value(2).isEmpty() ? QString() : QStringLiteral("/") + parts.value(2);
        const QString userPart = parts.value(3).isEmpty() ? QString() : QStringLiteral(" — ") + parts.value(3);
        return service + pathPart + userPart;
    };
    const auto loadContexts = [savedContexts, contextText] {
        savedContexts->clear();
        const QStringList records = QSettings().value(QStringLiteral("gitWorkspace/savedAccessTokenContexts")).toStringList();
        for (const QString &record : records) {
            const QStringList parts = record.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
            if (parts.size() != 4) continue;
            auto *item = new QListWidgetItem(contextText(parts), savedContexts);
            item->setData(Qt::UserRole, parts);
        }
    };
    loadContexts();
    connect(savedContexts, &QListWidget::currentItemChanged, &dialog, [=](QListWidgetItem *item) {
        if (!item) return;
        const QStringList parts = item->data(Qt::UserRole).toStringList();
        protocol->setCurrentIndex(protocol->findData(parts.value(0)));
        host->setText(parts.value(1));
        path->setText(parts.value(2));
        username->setText(parts.value(3));
        password->clear();
    });
    const auto runCredential = [this](const QString &operation, const QStringList &context, const QString &secret, QString *error) {
        QProcess process;
        process.setProgram(gitExecutable());
        process.setArguments({QStringLiteral("credential"), operation});
        process.setWorkingDirectory(repoPath);
        process.start();
        if (!process.waitForStarted()) { if (error) *error = process.errorString(); return false; }
        QByteArray input = QByteArray("protocol=") + context.value(0).toUtf8()
            + QByteArray("\nhost=") + context.value(1).toUtf8();
        if (!context.value(2).isEmpty()) input += QByteArray("\npath=") + context.value(2).toUtf8();
        if (!context.value(3).isEmpty()) input += QByteArray("\nusername=") + context.value(3).toUtf8();
        if (operation == QLatin1String("approve")) input += QByteArray("\npassword=") + secret.toUtf8();
        input += QByteArray("\n\n");
        process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(30000) || process.exitCode() != 0) {
            if (error) *error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            return false;
        }
        return true;
    };
    connect(saveButton, &QPushButton::clicked, &dialog, [&, this] {
        const QStringList context{protocol->currentData().toString(), host->text().trimmed(), path->text().trimmed(), username->text().trimmed()};
        const QString secret = password->text();
        if (context.value(1).isEmpty() || context.value(3).isEmpty() || secret.isEmpty()
            || secret.contains(QLatin1Char('\n')) || secret.contains(QLatin1Char('\r'))) {
            QMessageBox::warning(&dialog, QStringLiteral("Token details required"),
                QStringLiteral("Enter a host, username, and a single-line access token."));
            return;
        }
        QString error;
        if (!runCredential(QStringLiteral("approve"), context, secret, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Token was not stored"), error);
            return;
        }
        QStringList records = QSettings().value(QStringLiteral("gitWorkspace/savedAccessTokenContexts")).toStringList();
        const QString record = context.join(QLatin1Char('\t'));
        records.removeAll(record);
        records.append(record);
        QSettings().setValue(QStringLiteral("gitWorkspace/savedAccessTokenContexts"), records);
        password->clear();
        loadContexts();
        statusBar()->showMessage(QStringLiteral("Access token stored by Git’s configured credential helper."), 4000);
    });
    connect(removeButton, &QPushButton::clicked, &dialog, [&, this] {
        QListWidgetItem *item = savedContexts->currentItem();
        if (!item) return;
        const QStringList context = item->data(Qt::UserRole).toStringList();
        if (QMessageBox::question(&dialog, QStringLiteral("Remove access token"),
                QStringLiteral("Remove the stored token for %1?").arg(contextText(context))) != QMessageBox::Yes) return;
        QString error;
        if (!runCredential(QStringLiteral("reject"), context, QString(), &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Token was not removed"), error);
            return;
        }
        QStringList records = QSettings().value(QStringLiteral("gitWorkspace/savedAccessTokenContexts")).toStringList();
        records.removeAll(context.join(QLatin1Char('\t')));
        QSettings().setValue(QStringLiteral("gitWorkspace/savedAccessTokenContexts"), records);
        loadContexts();
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void GitWorkspaceWindow::manageSshKeys()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Manage SSH Keys"));
    dialog.resize(780, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *keys = new QListWidget(&dialog);
    auto *publicKey = new QPlainTextEdit(&dialog);
    publicKey->setReadOnly(true);
    publicKey->setPlaceholderText(QStringLiteral("Select a key to view its public key."));
    layout->addWidget(new QLabel(QStringLiteral("SSH public keys found in ~/.ssh and keys added here:"), &dialog));
    layout->addWidget(keys, 1);
    layout->addWidget(publicKey, 1);
    auto *actions = new QHBoxLayout;
    auto *generateButton = new QPushButton(QStringLiteral("Generate Key Pair…"), &dialog);
    auto *addExistingButton = new QPushButton(QStringLiteral("Add Existing Public Key…"), &dialog);
    auto *copyButton = new QPushButton(QStringLiteral("Copy Public Key"), &dialog);
    auto *agentButton = new QPushButton(QStringLiteral("Add Private Key to ssh-agent"), &dialog);
    auto *deleteButton = new QPushButton(QStringLiteral("Delete Key Pair…"), &dialog);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
    actions->addWidget(generateButton);
    actions->addWidget(addExistingButton);
    actions->addWidget(copyButton);
    actions->addWidget(agentButton);
    actions->addWidget(deleteButton);
    actions->addStretch(1);
    actions->addWidget(closeButton);
    layout->addLayout(actions);

    const QString homeSsh = QDir::home().filePath(QStringLiteral(".ssh"));
    const auto refresh = [keys, publicKey, homeSsh] {
        keys->clear();
        QSet<QString> paths;
        const QDir sshDirectory(homeSsh);
        for (const QString &name : sshDirectory.entryList({QStringLiteral("*.pub")}, QDir::Files, QDir::Name))
            paths.insert(sshDirectory.absoluteFilePath(name));
        for (const QString &path : QSettings().value(QStringLiteral("gitWorkspace/managedSshPublicKeys")).toStringList())
            if (QFileInfo::exists(path)) paths.insert(QFileInfo(path).absoluteFilePath());
        QStringList sortedPaths = paths.values();
        std::sort(sortedPaths.begin(), sortedPaths.end());
        for (const QString &path : sortedPaths) {
            auto *item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(QFileInfo(path).fileName(), path), keys);
            item->setData(Qt::UserRole, path);
        }
        publicKey->clear();
    };
    refresh();
    connect(keys, &QListWidget::currentItemChanged, &dialog, [keys, publicKey](QListWidgetItem *item) {
        if (!item) { publicKey->clear(); return; }
        QFile file(item->data(Qt::UserRole).toString());
        publicKey->setPlainText(file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed()
            : QStringLiteral("Could not read the selected public key."));
    });
    connect(generateButton, &QPushButton::clicked, &dialog, [this, &dialog, refresh, homeSsh] {
        QDialog generate(&dialog);
        generate.setWindowTitle(QStringLiteral("Generate SSH Key Pair"));
        auto *form = new QFormLayout(&generate);
        auto *type = new QComboBox(&generate);
        type->addItem(QStringLiteral("Ed25519 (recommended)"), QStringLiteral("ed25519"));
        type->addItem(QStringLiteral("RSA 4096-bit"), QStringLiteral("rsa"));
        auto *comment = new QLineEdit(emailEdit->text().trimmed(), &generate);
        auto *pathEdit = new QLineEdit(QDir(homeSsh).filePath(QStringLiteral("id_ed25519")), &generate);
        auto *passphrase = new QLineEdit(&generate);
        passphrase->setEchoMode(QLineEdit::Password);
        passphrase->setPlaceholderText(QStringLiteral("Optional passphrase"));
        form->addRow(QStringLiteral("Key type"), type);
        form->addRow(QStringLiteral("Comment"), comment);
        form->addRow(QStringLiteral("Save private key as"), pathEdit);
        form->addRow(QStringLiteral("Passphrase"), passphrase);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &generate);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Generate"));
        form->addRow(buttons);
        connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &generate, [type, pathEdit, homeSsh](int index) {
            const QString leaf = index == 0 ? QStringLiteral("id_ed25519") : QStringLiteral("id_rsa");
            if (pathEdit->text() == QDir(homeSsh).filePath(QStringLiteral("id_ed25519"))
                || pathEdit->text() == QDir(homeSsh).filePath(QStringLiteral("id_rsa")))
                pathEdit->setText(QDir(homeSsh).filePath(leaf));
        });
        connect(buttons, &QDialogButtonBox::accepted, &generate, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &generate, &QDialog::reject);
        if (generate.exec() != QDialog::Accepted) return;
        QString privatePath = pathEdit->text().trimmed();
        if (privatePath == QStringLiteral("~")) privatePath = QDir::homePath();
        else if (privatePath.startsWith(QStringLiteral("~/"))) privatePath = QDir::home().filePath(privatePath.mid(2));
        privatePath = QFileInfo(privatePath).absoluteFilePath();
        if (privatePath.isEmpty() || comment->text().contains(QLatin1Char('\n'))
            || passphrase->text().contains(QLatin1Char('\n'))) {
            QMessageBox::warning(&generate, QStringLiteral("Invalid key details"), QStringLiteral("Enter a valid key path and single-line comment/passphrase."));
            return;
        }
        const QString publicPath = privatePath + QStringLiteral(".pub");
        if (QFileInfo::exists(privatePath) || QFileInfo::exists(publicPath)) {
            if (QMessageBox::question(&generate, QStringLiteral("Overwrite SSH key?"),
                    QStringLiteral("A key file already exists at this path. Replace the existing key pair?")) != QMessageBox::Yes) return;
            QFile::remove(privatePath);
            QFile::remove(publicPath);
        }
        QDir().mkpath(QFileInfo(privatePath).absolutePath());
        QProcess process;
        process.setProgram(QStringLiteral("ssh-keygen"));
        QStringList arguments{QStringLiteral("-t"), type->currentData().toString(), QStringLiteral("-C"), comment->text().trimmed(),
            QStringLiteral("-f"), privatePath, QStringLiteral("-N"), passphrase->text()};
        if (type->currentData().toString() == QLatin1String("rsa")) arguments << QStringLiteral("-b") << QStringLiteral("4096");
        process.setArguments(arguments);
        process.start();
        if (!process.waitForStarted() || !process.waitForFinished(60000) || process.exitCode() != 0) {
            const QString processError = process.errorString();
            const QString standardError = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            QFile::remove(privatePath);
            QFile::remove(publicPath);
            QMessageBox::warning(&generate, QStringLiteral("SSH key generation failed"),
                standardError.isEmpty() ? processError : standardError);
            return;
        }
        passphrase->clear();
        refresh();
    });
    connect(addExistingButton, &QPushButton::clicked, &dialog, [this, &dialog, refresh] {
        const QString path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Add SSH Public Key"),
            QDir::home().filePath(QStringLiteral(".ssh")), QStringLiteral("SSH public keys (*.pub);;All files (*)"));
        if (path.isEmpty()) return;
        const QString privatePath = path.endsWith(QStringLiteral(".pub")) ? path.left(path.size() - 4) : path;
        if (!QFileInfo::exists(privatePath)) {
            QMessageBox::warning(&dialog, QStringLiteral("Private key missing"), QStringLiteral("The matching private key file was not found."));
            return;
        }
        QStringList paths = QSettings().value(QStringLiteral("gitWorkspace/managedSshPublicKeys")).toStringList();
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        if (!paths.contains(absolutePath)) paths.append(absolutePath);
        QSettings().setValue(QStringLiteral("gitWorkspace/managedSshPublicKeys"), paths);
        refresh();
    });
    connect(copyButton, &QPushButton::clicked, &dialog, [this, keys] {
        if (!keys->currentItem()) return;
        QFile file(keys->currentItem()->data(Qt::UserRole).toString());
        if (file.open(QIODevice::ReadOnly)) {
            QApplication::clipboard()->setText(QString::fromUtf8(file.readAll()).trimmed());
            statusBar()->showMessage(QStringLiteral("Public key copied to clipboard."), 3000);
        }
    });
    connect(agentButton, &QPushButton::clicked, &dialog, [this, &dialog, keys] {
        if (!keys->currentItem()) return;
        QString privatePath = keys->currentItem()->data(Qt::UserRole).toString();
        if (privatePath.endsWith(QStringLiteral(".pub"))) privatePath.chop(4);
        QProcess process;
        process.setProgram(QStringLiteral("ssh-add"));
        process.setArguments({privatePath});
        process.start();
        if (!process.waitForStarted() || !process.waitForFinished(15000) || process.exitCode() != 0)
            QMessageBox::warning(&dialog, QStringLiteral("Could not add key to ssh-agent"),
                QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        else statusBar()->showMessage(QStringLiteral("SSH private key added to ssh-agent."), 4000);
    });
    connect(deleteButton, &QPushButton::clicked, &dialog, [this, &dialog, keys, refresh] {
        if (!keys->currentItem()) return;
        const QString publicPath = keys->currentItem()->data(Qt::UserRole).toString();
        QString privatePath = publicPath;
        if (privatePath.endsWith(QStringLiteral(".pub"))) privatePath.chop(4);
        if (QMessageBox::question(&dialog, QStringLiteral("Delete SSH key pair?"),
                QStringLiteral("Permanently delete both key files?\n%1\n%2").arg(privatePath, publicPath)) != QMessageBox::Yes) return;
        const bool privateRemoved = !QFileInfo::exists(privatePath) || QFile::remove(privatePath);
        const bool publicRemoved = !QFileInfo::exists(publicPath) || QFile::remove(publicPath);
        QStringList paths = QSettings().value(QStringLiteral("gitWorkspace/managedSshPublicKeys")).toStringList();
        paths.removeAll(publicPath);
        QSettings().setValue(QStringLiteral("gitWorkspace/managedSshPublicKeys"), paths);
        if (!privateRemoved || !publicRemoved)
            QMessageBox::warning(&dialog, QStringLiteral("Could not delete key pair"), QStringLiteral("One or both key files could not be removed."));
        refresh();
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
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
    const bool groupedWorkspace = workspace.repositories.size() > 1 && !workspaceAmbiguous;
    if (stageAllShortcutAction) stageAllShortcutAction->setEnabled(hasUnstagedChanges);
    if (commitShortcutAction) commitShortcutAction->setEnabled(hasStagedChanges);
    if (pushShortcutAction) pushShortcutAction->setEnabled(hasRemote && hasUnpushedCommits);
    if (pullShortcutAction) pullShortcutAction->setEnabled(hasRemote);
    if (fetchShortcutAction) fetchShortcutAction->setEnabled(hasRemote);
    if (stashShortcutAction) stashShortcutAction->setEnabled(hasStagedChanges || hasUnstagedChanges);
    if (resolveShortcutAction) resolveShortcutAction->setEnabled(hasConflicts);
    if (commitButton) {
        commitButton->setVisible(hasStagedChanges || groupedWorkspace);
        commitButton->setEnabled(hasStagedChanges || groupedWorkspace);
    }
    if (stageAllButton) {
        stageAllButton->setVisible(hasUnstagedChanges || groupedWorkspace);
        stageAllButton->setEnabled(hasUnstagedChanges || groupedWorkspace);
    }
    if (syncButton) syncButton->setVisible(groupedWorkspace || (hasRemote && (hasUpstream || hasUnpushedCommits)));
    if (pullButton) {
        pullButton->setVisible(groupedWorkspace || hasRemote);
        pullButton->setEnabled(groupedWorkspace || hasRemote);
    }
    if (pushButton) {
        const bool canPush = hasRemote && hasUnpushedCommits && !gitOutput({QStringLiteral("branch"), QStringLiteral("--show-current")}).isEmpty();
        pushButton->setVisible(groupedWorkspace || canPush);
        pushButton->setEnabled(groupedWorkspace || canPush);
        pushButton->setToolTip(QStringLiteral("Push local commits to the configured remote"));
    }
    if (applyStashButton) {
        applyStashButton->setVisible(hasStashes);
        applyStashButton->setEnabled(hasStashes);
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
