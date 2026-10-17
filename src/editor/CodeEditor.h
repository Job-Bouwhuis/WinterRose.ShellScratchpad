#pragma once

#include <QPlainTextEdit>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QTextCursor>
#include <QVector>

class QWidget;
class QMouseEvent;
class QKeyEvent;

namespace ShellScratchpad
{
class CodeEditor final : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);
    void toggleFoldAt(int blockNumber);
    void handleGutterClick(int y);
    void collapseAllSections();
    void expandAllSections();
    void insertSnippet(const QString &source, const QVector<QPair<int, int>> &highlightPoints);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();
    void rebuildFoldRegions();
    void applyFoldVisibility();

private:
    QWidget *lineNumberArea = nullptr;
    QMap<int, int> foldRegions;
    QSet<int> collapsedBlocks;
    int selectedFoldBlock = -1;
    int virtualAfterFoldBlock = -1;
    int normalCursorWidth = 1;
    QVector<QTextCursor> snippetPointCursors;
    QTextCursor snippetEndCursor;
    int activeSnippetPoint = -1;
};
}
