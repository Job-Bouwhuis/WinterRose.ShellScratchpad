#pragma once

#include <QPlainTextEdit>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QVector>
#include "snippets/SnippetStore.h"

class QWidget;
class QMouseEvent;
class QKeyEvent;

namespace ShellScratchpad
{
class CodeEditor final : public QPlainTextEdit
{
    Q_OBJECT
public:
    struct NavigationSymbol
    {
        QString name;
        qsizetype declarationOffset = -1;
        bool profileEnvironment = false;
    };
    struct TextDecoration
    {
        qsizetype startOffset = 0;
        qsizetype length = 0;
        QTextCharFormat format;
    };

    explicit CodeEditor(QWidget *parent = nullptr);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);
    void toggleFoldAt(int blockNumber);
    void handleGutterClick(int y);
    void collapseAllSections();
    void expandAllSections();
    void revealPosition(qsizetype position);
    void insertSnippet(const QString &source, const QVector<QPair<int, int>> &highlightPoints,
                       const QVector<ShellScratchpad::Snippets::AutoChangeSection> &autoChangeSections = {});
    void setTextDecorations(const QVector<TextDecoration> &decorations);
    void setNavigationSymbols(const QVector<NavigationSymbol> &symbols);
    void toggleBreakpoint(int lineNumber);
    void toggleBreakpointAtY(int y);
    QSet<int> breakpoints() const;
    void setDebugLine(int lineNumber);

signals:
    void symbolNavigationRequested(const QString &name);
    void breakpointsChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

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
    int debugLine = 0;
    int selectedFoldBlock = -1;
    int virtualAfterFoldBlock = -1;
    int normalCursorWidth = 1;
    QVector<QTextCursor> snippetPointCursors;
    struct ActiveAutoChange
    {
        QTextCursor range;
        int sourcePointIndex = -1;
        bool applied = false;
    };
    QVector<ActiveAutoChange> snippetAutoChanges;
    QTextCursor activePointStartCursor;
    QTextCursor activePointEndCursor;
    QTextCursor snippetEndCursor;
    int activeSnippetPoint = -1;
    QVector<TextDecoration> textDecorations;
    QVector<NavigationSymbol> navigationSymbols;
    qsizetype navigationHoverStart = -1;
    qsizetype navigationHoverLength = 0;

    const NavigationSymbol *navigationSymbolAt(const QPoint &viewportPosition, qsizetype *start = nullptr,
                                               qsizetype *length = nullptr) const;
    void updateNavigationHover(const QPoint &viewportPosition, bool controlHeld);
    bool indentSelectedLines(bool unindent);
    void updateTabStopDistance();
};
}
