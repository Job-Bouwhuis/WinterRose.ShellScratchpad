#include "CodeEditor.h"

#include "../syntax/SyntaxTokenizer.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QEvent>
#include <QTextDocument>
#include <QTextBlockUserData>
#include <QCursor>
#include <QMimeData>
#include <QRegularExpression>
#include <algorithm>

namespace ShellScratchpad
{
namespace
{
class BreakpointBlockData final : public QTextBlockUserData
{
public:
    bool breakpoint = true;
};

class LineNumberArea final : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor), codeEditor(editor)
    {
        setObjectName(QStringLiteral("lineNumberArea"));
    }

    QSize sizeHint() const override
    {
        return QSize(codeEditor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        codeEditor->lineNumberAreaPaintEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (event->position().x() < 15)
                codeEditor->handleGutterClick(qRound(event->position().y()));
            else
                codeEditor->toggleBreakpointAtY(qRound(event->position().y()));
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CodeEditor *codeEditor;
};
}

void CodeEditor::toggleBreakpointAtY(int y)
{
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid()) {
        if (block.isVisible() && y >= top && y <= bottom) {
            toggleBreakpoint(block.blockNumber() + 1);
            return;
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
}

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent), lineNumberArea(new LineNumberArea(this))
{
    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);
    connect(this, &QPlainTextEdit::textChanged, this, &CodeEditor::rebuildFoldRegions);
    normalCursorWidth = cursorWidth();
    rebuildFoldRegions();
    updateLineNumberAreaWidth();
    updateTabStopDistance();
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int maximum = qMax(1, blockCount());
    while (maximum >= 10) {
        maximum /= 10;
        ++digits;
    }
    return 19 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        lineNumberArea->scroll(0, dy);
    else
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect CONTENT = contentsRect();
    lineNumberArea->setGeometry(QRect(CONTENT.left(), CONTENT.top(), lineNumberAreaWidth(), CONTENT.height()));
}

void CodeEditor::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateLineNumberAreaWidth();
        updateTabStopDistance();
    }
}

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (const NavigationSymbol *symbol = navigationSymbolAt(event->position().toPoint())) {
            emit symbolNavigationRequested(symbol->name);
            event->accept();
            return;
        }
    }
    if (!snippetPointCursors.isEmpty()) {
        snippetPointCursors.clear();
        snippetAutoChanges.clear();
        activeSnippetPoint = -1;
    }
    if (event->button() == Qt::LeftButton) {
        for (auto it = collapsedBlocks.cbegin(); it != collapsedBlocks.cend(); ++it) {
            const QTextBlock opener = document()->findBlockByNumber(*it);
            if (!opener.isValid())
                continue;
            QTextCursor lineEnd(opener);
            lineEnd.movePosition(QTextCursor::EndOfBlock);
            const QRect CARET_RECT = cursorRect(lineEnd);
            const int HIDDEN_LINE_COUNT = foldRegions.value(*it) - *it;
            const QString LABEL = QStringLiteral("‹ %1 folded lines ›").arg(HIDDEN_LINE_COUNT);
            const QRect ARTIFACT_RECT(CARET_RECT.right() + 4, CARET_RECT.top(),
                                      fontMetrics().horizontalAdvance(LABEL) + 14,
                                      fontMetrics().height());
            if (!ARTIFACT_RECT.contains(event->position().toPoint()))
                continue;

            const QTextBlock endBlock = document()->findBlockByNumber(foldRegions.value(*it));
            const int afterPosition = endBlock.isValid()
                ? endBlock.position() + endBlock.length()
                : document()->characterCount() - 1;
            if (event->position().x() < ARTIFACT_RECT.left() + 14) {
                QTextCursor before(opener);
                before.movePosition(QTextCursor::EndOfBlock);
                virtualAfterFoldBlock = -1;
                setCursorWidth(normalCursorWidth);
                setTextCursor(before);
                selectedFoldBlock = -1;
            } else if (event->position().x() > ARTIFACT_RECT.right() - 14) {
                const QTextBlock followingBlock = endBlock.next();
                if (followingBlock.isValid() && followingBlock.isVisible()) {
                    virtualAfterFoldBlock = -1;
                    setCursorWidth(normalCursorWidth);
                    QTextCursor after(followingBlock);
                    setTextCursor(after);
                } else {
                    virtualAfterFoldBlock = *it;
                    setCursorWidth(0);
                    QTextCursor visualCaret(opener);
                    visualCaret.movePosition(QTextCursor::EndOfBlock);
                    setTextCursor(visualCaret);
                }
                selectedFoldBlock = -1;
            } else {
                const int sectionStart = opener.position() + opener.length();
                QTextCursor selection(document());
                selection.setPosition(qMin(afterPosition, document()->characterCount() - 1));
                selection.setPosition(sectionStart, QTextCursor::KeepAnchor);
                virtualAfterFoldBlock = -1;
                setCursorWidth(normalCursorWidth);
                setTextCursor(selection);
                selectedFoldBlock = *it;
            }
            lineNumberArea->update();
            viewport()->update();
            event->accept();
            return;
        }
    }
    if (virtualAfterFoldBlock >= 0) {
        virtualAfterFoldBlock = -1;
        setCursorWidth(normalCursorWidth);
    }
    QPlainTextEdit::mousePressEvent(event);
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseMoveEvent(event);
    updateNavigationHover(event->position().toPoint(), event->modifiers().testFlag(Qt::ControlModifier));
}

void CodeEditor::leaveEvent(QEvent *event)
{
    updateNavigationHover(QPoint(-1, -1), false);
    QPlainTextEdit::leaveEvent(event);
}

void CodeEditor::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    QPainter painter(viewport());
    painter.setFont(font());
    for (int blockNumber : collapsedBlocks) {
        const QTextBlock opener = document()->findBlockByNumber(blockNumber);
        if (!opener.isValid() || !opener.isVisible())
            continue;
        QTextCursor lineEnd(opener);
        lineEnd.movePosition(QTextCursor::EndOfBlock);
        const QRect CARET_RECT = cursorRect(lineEnd);
        const int HIDDEN_LINE_COUNT = foldRegions.value(blockNumber) - blockNumber;
        const QString LABEL = QStringLiteral("‹ %1 folded lines ›").arg(HIDDEN_LINE_COUNT);
        const QRect ARTIFACT_RECT(CARET_RECT.right() + 4, CARET_RECT.top(),
                                  fontMetrics().horizontalAdvance(LABEL) + 14,
                                  fontMetrics().height());
        if (!viewport()->rect().intersects(ARTIFACT_RECT))
            continue;
        const bool IS_SELECTED = selectedFoldBlock == blockNumber;
        painter.setPen(IS_SELECTED ? palette().color(QPalette::HighlightedText)
                                   : QColor(QStringLiteral("#d6dce5")));
        painter.setBrush(IS_SELECTED ? palette().color(QPalette::Highlight)
                                     : QColor(QStringLiteral("#3b4654")));
        painter.drawRoundedRect(ARTIFACT_RECT.adjusted(0, 1, -1, -1), 4, 4);
        painter.drawText(ARTIFACT_RECT.adjusted(7, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft, LABEL);
        if (virtualAfterFoldBlock == blockNumber) {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawLine(ARTIFACT_RECT.right() + 2, ARTIFACT_RECT.top() + 3,
                             ARTIFACT_RECT.right() + 2, ARTIFACT_RECT.bottom() - 3);
        }
    }
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Control)
        updateNavigationHover(viewport()->mapFromGlobal(QCursor::pos()), true);
    if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) && textCursor().hasSelection()) {
        const bool UNINDENT = event->key() == Qt::Key_Backtab || event->modifiers().testFlag(Qt::ShiftModifier);
        if (indentSelectedLines(UNINDENT)) {
            event->accept();
            return;
        }
    }
    if (activeSnippetPoint >= 0 && !snippetPointCursors.isEmpty()) {
        if (event->key() == Qt::Key_Escape) {
            snippetPointCursors.clear();
            snippetAutoChanges.clear();
            activeSnippetPoint = -1;
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            const int CONFIRMED_POINT = activeSnippetPoint;
            const int VALUE_START = activePointStartCursor.position();
            const int VALUE_END = activePointEndCursor.position();
            const QString CONFIRMED_VALUE = document()->toPlainText().mid(VALUE_START, qMax(0, VALUE_END - VALUE_START));
            for (ActiveAutoChange &section : snippetAutoChanges) {
                if (section.sourcePointIndex != CONFIRMED_POINT || section.applied)
                    continue;
                QTextCursor target = section.range;
                target.insertText(CONFIRMED_VALUE);
                section.range = target;
                section.applied = true;
            }
            ++activeSnippetPoint;
            if (activeSnippetPoint < snippetPointCursors.size()) {
                setTextCursor(snippetPointCursors.at(activeSnippetPoint));
                activePointStartCursor = QTextCursor(document());
                activePointStartCursor.setPosition(snippetPointCursors.at(activeSnippetPoint).selectionStart());
                activePointStartCursor.setKeepPositionOnInsert(true);
                activePointEndCursor = QTextCursor(document());
                activePointEndCursor.setPosition(snippetPointCursors.at(activeSnippetPoint).selectionEnd());
            } else {
                snippetPointCursors.clear();
                snippetAutoChanges.clear();
                activeSnippetPoint = -1;
                setTextCursor(snippetEndCursor);
            }
            event->accept();
            return;
        }
    }

    if (virtualAfterFoldBlock < 0 || !foldRegions.contains(virtualAfterFoldBlock)) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    const int foldedBlock = virtualAfterFoldBlock;
    const QTextBlock endBlock = document()->findBlockByNumber(foldRegions.value(foldedBlock));
    const int insertionPosition = endBlock.isValid()
        ? qMin(endBlock.position() + endBlock.length(), document()->characterCount() - 1)
        : document()->characterCount() - 1;
    QString insertionText;
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        insertionText = QStringLiteral("\n");
    } else if (event->matches(QKeySequence::Paste)) {
        insertionText = QApplication::clipboard()->text();
    } else if (event->text().size() > 0 &&
               !event->modifiers().testFlag(Qt::ControlModifier) &&
               !event->modifiers().testFlag(Qt::AltModifier) &&
               !event->modifiers().testFlag(Qt::MetaModifier)) {
        insertionText = event->text();
    } else {
        virtualAfterFoldBlock = -1;
        setCursorWidth(normalCursorWidth);
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    QTextCursor insertion(document());
    insertion.setPosition(insertionPosition);
    insertion.insertBlock();
    if (insertionText != QStringLiteral("\n"))
        insertion.insertText(insertionText);
    virtualAfterFoldBlock = -1;
    selectedFoldBlock = -1;
    setCursorWidth(normalCursorWidth);
    setTextCursor(insertion);
    event->accept();
}

void CodeEditor::insertFromMimeData(const QMimeData *source)
{
    if (!source || !source->hasText()) {
        QPlainTextEdit::insertFromMimeData(source);
        return;
    }

    const QString pastedText = source->text();
    static const QRegularExpression codeFencePattern(
        QStringLiteral("\\A[\\t ]*(?<fence>`{3,}|~{3,})[^\\r\\n]*(?:\\r\\n|\\n|\\r)"
                       "(?<body>[\\s\\S]*?)(?:\\r\\n|\\n|\\r)[\\t ]*\\k<fence>[\\t ]*(?:\\r\\n|\\n|\\r)?\\z"));
    const QRegularExpressionMatch match = codeFencePattern.match(pastedText);
    if (!match.hasMatch()) {
        QPlainTextEdit::insertFromMimeData(source);
        return;
    }

    const QString body = match.captured(QStringLiteral("body"));
    const qsizetype bodyStart = match.capturedStart(QStringLiteral("body"));
    const qsizetype bodyEnd = match.capturedEnd(QStringLiteral("body"));
    QTextCursor cursor = textCursor();
    const qsizetype insertionStart = cursor.selectionStart();
    cursor.beginEditBlock();
    cursor.insertText(pastedText);
    cursor.endEditBlock();

    // Keep paste and fence removal as separate undo steps. The first Undo
    // restores the Markdown fences while leaving the code in the editor.
    cursor.beginEditBlock();
    cursor.setPosition(insertionStart + bodyEnd);
    cursor.setPosition(insertionStart + pastedText.size(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.setPosition(insertionStart);
    cursor.setPosition(insertionStart + bodyStart, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.endEditBlock();
    cursor.setPosition(insertionStart + body.size());
    setTextCursor(cursor);
    emit markdownCodeBlockUnwrapped();
}

bool CodeEditor::indentSelectedLines(bool unindent)
{
    const QTextCursor originalCursor = textCursor();
    const int selectionStart = static_cast<int>(originalCursor.selectionStart());
    const int selectionEnd = static_cast<int>(originalCursor.selectionEnd());
    const QTextBlock firstBlock = document()->findBlock(selectionStart);
    int adjustedEnd = selectionEnd;
    if (adjustedEnd > selectionStart) {
        const QTextBlock blockAtEnd = document()->findBlock(adjustedEnd);
        if (blockAtEnd.isValid() && blockAtEnd.position() == adjustedEnd)
            --adjustedEnd;
    }
    const QTextBlock lastBlock = document()->findBlock(adjustedEnd);
    if (!firstBlock.isValid() || !lastBlock.isValid() || firstBlock == lastBlock)
        return false;

    struct Edit { int position; int removed; int inserted; };
    QVector<Edit> edits;
    for (QTextBlock block = firstBlock; block.isValid() && block.blockNumber() <= lastBlock.blockNumber();
         block = block.next()) {
        const QString line = block.text();
        if (!unindent) {
            edits.append(Edit{block.position(), 0, 4});
            continue;
        }
        int removeCount = 0;
        if (line.startsWith(QLatin1Char('\t'))) {
            removeCount = 1;
        } else {
            while (removeCount < 4 && removeCount < line.size() && line.at(removeCount) == QLatin1Char(' '))
                ++removeCount;
        }
        if (removeCount > 0)
            edits.append(Edit{block.position(), removeCount, 0});
    }
    if (edits.isEmpty())
        return true;

    QTextCursor editCursor(document());
    editCursor.beginEditBlock();
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        editCursor.setPosition(it->position);
        if (it->removed > 0) {
            editCursor.setPosition(it->position + it->removed, QTextCursor::KeepAnchor);
            editCursor.removeSelectedText();
        }
        if (it->inserted > 0)
            editCursor.insertText(QString(it->inserted, QLatin1Char(' ')));
    }
    editCursor.endEditBlock();

    const auto adjustedPosition = [&edits](int position) {
        int result = position;
        for (const Edit &edit : edits) {
            if (edit.inserted > 0 && edit.position <= position)
                result += edit.inserted;
            else if (edit.removed > 0 && edit.position + edit.removed <= position)
                result -= edit.removed;
            else if (edit.removed > 0 && edit.position < position)
                result = edit.position;
        }
        return result;
    };
    QTextCursor restored(document());
    restored.setPosition(qBound(0, adjustedPosition(selectionStart), document()->characterCount() - 1));
    restored.setPosition(qBound(0, adjustedPosition(selectionEnd), document()->characterCount() - 1),
                         QTextCursor::KeepAnchor);
    setTextCursor(restored);
    return true;
}

void CodeEditor::updateTabStopDistance()
{
    setTabStopDistance(4.0 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
}

void CodeEditor::keyReleaseEvent(QKeyEvent *event)
{
    QPlainTextEdit::keyReleaseEvent(event);
    if (event->key() == Qt::Key_Control)
        updateNavigationHover(viewport()->mapFromGlobal(QCursor::pos()), false);
}

void CodeEditor::insertSnippet(const QString &source, const QVector<QPair<int, int>> &highlightPoints,
                               const QVector<ShellScratchpad::Snippets::AutoChangeSection> &autoChangeSections)
{
    snippetPointCursors.clear();
    snippetAutoChanges.clear();
    activeSnippetPoint = -1;
    virtualAfterFoldBlock = -1;
    setCursorWidth(normalCursorWidth);

    QTextCursor insertion = textCursor();
    const int START = insertion.selectionStart();
    insertion.insertText(source);
    const int END = START + source.size();
    snippetEndCursor = QTextCursor(document());
    snippetEndCursor.setPosition(END);

    QVector<QPair<int, int>> orderedPoints = highlightPoints;
    std::stable_sort(orderedPoints.begin(), orderedPoints.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    for (const auto &point : orderedPoints) {
        if (point.first < 0 || point.second < 0 || point.first + point.second > source.size())
            continue;
        QTextCursor selection(document());
        selection.setPosition(START + point.first);
        selection.setPosition(START + point.first + point.second, QTextCursor::KeepAnchor);
        snippetPointCursors.append(selection);
    }
    for (const ShellScratchpad::Snippets::AutoChangeSection &section : autoChangeSections) {
        if (section.offset < 0 || section.length < 0 || section.offset + section.length > source.size())
            continue;
        int sourcePointIndex = -1;
        for (int index = 0; index < orderedPoints.size(); ++index) {
            if (orderedPoints.at(index).first == section.sourcePointOffset) {
                sourcePointIndex = index;
                break;
            }
        }
        if (sourcePointIndex < 0)
            continue;
        QTextCursor target(document());
        target.setPosition(START + section.offset);
        target.setPosition(START + section.offset + section.length, QTextCursor::KeepAnchor);
        snippetAutoChanges.append(ActiveAutoChange{target, sourcePointIndex, false});
    }
    if (snippetPointCursors.isEmpty()) {
        setTextCursor(snippetEndCursor);
        return;
    }
    activeSnippetPoint = 0;
    setTextCursor(snippetPointCursors.first());
    activePointStartCursor = QTextCursor(document());
    activePointStartCursor.setPosition(snippetPointCursors.first().selectionStart());
    activePointStartCursor.setKeepPositionOnInsert(true);
    activePointEndCursor = QTextCursor(document());
    activePointEndCursor.setPosition(snippetPointCursors.first().selectionEnd());
    setFocus();
}

void CodeEditor::rebuildFoldRegions()
{
    using ShellScratchpad::Syntax::SyntaxToken;
    using ShellScratchpad::Syntax::TokenKind;

    selectedFoldBlock = -1;
    if (virtualAfterFoldBlock >= 0) {
        virtualAfterFoldBlock = -1;
        setCursorWidth(normalCursorWidth);
    }
    const QString SOURCE = toPlainText();
    const QVector<SyntaxToken> TOKENS = ShellScratchpad::Syntax::SyntaxTokenizer::tokenize(SOURCE);
    foldRegions.clear();
    QVector<SyntaxToken> heredocSpans;
    for (const SyntaxToken &token : TOKENS) {
        if (token.kind == TokenKind::HereDocContent)
            heredocSpans.append(token);
    }

    for (const SyntaxToken &span : heredocSpans) {
        const int openerBlock = span.line - 2;
        const QString HEREDOC_TEXT = SOURCE.mid(span.startOffset, span.length);
        const int NEWLINE_COUNT = HEREDOC_TEXT.count(QLatin1Char('\n'));
        const bool ENDS_WITH_NEWLINE = HEREDOC_TEXT.endsWith(QLatin1Char('\n'));
        const int endBlock = span.line - 1 + NEWLINE_COUNT - (ENDS_WITH_NEWLINE ? 1 : 0);
        if (openerBlock >= 0 && endBlock > openerBlock)
            foldRegions[openerBlock] = qMax(foldRegions.value(openerBlock, openerBlock), endBlock);
    }

    struct OpenFold { QString closer; int line; };
    QVector<OpenFold> stack;
    auto hasStatementPrefix = [&SOURCE](const SyntaxToken &token, const QStringList &allowedEnds) {
        const qsizetype LINE_START_OFFSET = SOURCE.lastIndexOf(QLatin1Char('\n'), token.startOffset - 1) + 1;
        const QString PREFIX = SOURCE.mid(LINE_START_OFFSET, token.startOffset - LINE_START_OFFSET).trimmed();
        if (PREFIX.isEmpty())
            return true;
        for (const QString &ENDING : allowedEnds) {
            if (PREFIX.endsWith(ENDING))
                return true;
        }
        return false;
    };

    for (const SyntaxToken &token : TOKENS) {
        if (token.kind == TokenKind::HereDocContent)
            continue;
        const bool IN_HEREDOC = std::any_of(heredocSpans.cbegin(), heredocSpans.cend(), [&token](const SyntaxToken &span) {
            return token.startOffset >= span.startOffset && token.startOffset < span.startOffset + span.length;
        });
        if (IN_HEREDOC)
            continue;
        const QString TEXT = SOURCE.mid(token.startOffset, token.length);
        if (token.kind == TokenKind::Keyword &&
            (TEXT == QStringLiteral("if") || TEXT == QStringLiteral("for") ||
             TEXT == QStringLiteral("while") || TEXT == QStringLiteral("until") ||
             TEXT == QStringLiteral("select") || TEXT == QStringLiteral("case"))) {
            if (!hasStatementPrefix(token, {QStringLiteral(";"), QStringLiteral("then"),
                                            QStringLiteral("else"), QStringLiteral("do")}))
                continue;
            const QString CLOSER = TEXT == QStringLiteral("if") ? QStringLiteral("fi")
                : TEXT == QStringLiteral("case") ? QStringLiteral("esac") : QStringLiteral("done");
            stack.append(OpenFold{CLOSER, token.line - 1});
            continue;
        }
        if (token.kind == TokenKind::Operator && TEXT == QStringLiteral("{") &&
            hasStatementPrefix(token, {QStringLiteral(")"), QStringLiteral("then"),
                                       QStringLiteral("else"), QStringLiteral("do"), QStringLiteral(";")})) {
            stack.append(OpenFold{QStringLiteral("}"), token.line - 1});
            continue;
        }

        const bool IS_CLOSER = (token.kind == TokenKind::Keyword &&
                                (TEXT == QStringLiteral("fi") || TEXT == QStringLiteral("done") ||
                                 TEXT == QStringLiteral("esac"))) ||
                               (token.kind == TokenKind::Operator && TEXT == QStringLiteral("}"));
        if (!IS_CLOSER || stack.isEmpty() || stack.last().closer != TEXT ||
            !hasStatementPrefix(token, {QStringLiteral(";"), QStringLiteral(";;"),
                                        QStringLiteral(";&"), QStringLiteral(";;&")}))
            continue;
        const OpenFold OPEN = stack.takeLast();
        if (token.line - 1 > OPEN.line)
            foldRegions.insert(OPEN.line, token.line - 1);
    }

    for (auto it = collapsedBlocks.begin(); it != collapsedBlocks.end();) {
        if (!foldRegions.contains(*it))
            it = collapsedBlocks.erase(it);
        else
            ++it;
    }
    applyFoldVisibility();
}

void CodeEditor::toggleFoldAt(int blockNumber)
{
    if (!foldRegions.contains(blockNumber))
        return;
    if (collapsedBlocks.contains(blockNumber)) {
        collapsedBlocks.remove(blockNumber);
    } else {
        collapsedBlocks.insert(blockNumber);
        const int END_BLOCK = foldRegions.value(blockNumber);
        if (textCursor().blockNumber() > blockNumber && textCursor().blockNumber() <= END_BLOCK) {
            QTextCursor cursor(document()->findBlockByNumber(blockNumber));
            setTextCursor(cursor);
        }
    }
    applyFoldVisibility();
}

void CodeEditor::collapseAllSections()
{
    collapsedBlocks.clear();
    for (auto it = foldRegions.cbegin(); it != foldRegions.cend(); ++it)
        collapsedBlocks.insert(it.key());
    applyFoldVisibility();
}

void CodeEditor::expandAllSections()
{
    collapsedBlocks.clear();
    applyFoldVisibility();
}

void CodeEditor::revealPosition(qsizetype position)
{
    const QTextBlock target = document()->findBlock(position);
    if (!target.isValid())
        return;
    const int TARGET_BLOCK = target.blockNumber();
    bool changed = false;
    for (auto it = collapsedBlocks.begin(); it != collapsedBlocks.end();) {
        if (TARGET_BLOCK > *it && TARGET_BLOCK <= foldRegions.value(*it, *it)) {
            it = collapsedBlocks.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        applyFoldVisibility();
    virtualAfterFoldBlock = -1;
    selectedFoldBlock = -1;
    setCursorWidth(normalCursorWidth);
}

void CodeEditor::handleGutterClick(int y)
{
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid()) {
        const int bottom = top + qRound(blockBoundingRect(block).height());
        if (block.isVisible() && y >= top && y < bottom) {
            toggleFoldAt(block.blockNumber());
            return;
        }
        block = block.next();
        top = bottom;
    }
}

void CodeEditor::applyFoldVisibility()
{
    QTextBlock block = document()->begin();
    while (block.isValid()) {
        block.setVisible(true);
        block.setLineCount(1);
        block = block.next();
    }
    for (int startBlock : collapsedBlocks) {
        const int endBlock = foldRegions.value(startBlock, startBlock);
        for (int hiddenBlock = startBlock + 1; hiddenBlock <= endBlock; ++hiddenBlock) {
            QTextBlock hidden = document()->findBlockByNumber(hiddenBlock);
            if (!hidden.isValid())
                break;
            hidden.setVisible(false);
            hidden.setLineCount(0);
        }
    }
    document()->markContentsDirty(0, document()->characterCount());
    viewport()->update();
    lineNumberArea->update();
}

void CodeEditor::highlightCurrentLine()
{
    if (selectedFoldBlock >= 0) {
        const QTextBlock opener = document()->findBlockByNumber(selectedFoldBlock);
        const QTextBlock endBlock = document()->findBlockByNumber(foldRegions.value(selectedFoldBlock, -1));
        const int expectedStart = opener.isValid() ? opener.position() + opener.length() : -1;
        const int expectedEnd = endBlock.isValid()
            ? qMin(endBlock.position() + endBlock.length(), document()->characterCount() - 1) : -1;
        const QTextCursor cursor = textCursor();
        if (!cursor.hasSelection() || cursor.selectionStart() != expectedStart ||
            cursor.selectionEnd() != expectedEnd) {
            selectedFoldBlock = -1;
            viewport()->update();
        }
    }
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(QColor(QStringLiteral("#353535")));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        selections.append(selection);
    }
    if (debugLine > 0) {
        const QTextBlock block = document()->findBlockByNumber(debugLine - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.cursor.clearSelection();
            selection.format.setBackground(QColor(QStringLiteral("#51492a")));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selections.append(selection);
        }
    }
    for (const TextDecoration &decoration : textDecorations) {
        if (decoration.length <= 0 || decoration.startOffset < 0 ||
            decoration.startOffset >= document()->characterCount() - 1)
            continue;
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(document());
        selection.cursor.setPosition(static_cast<int>(decoration.startOffset));
        const qsizetype end = qMin<qsizetype>(decoration.startOffset + decoration.length,
                                              document()->characterCount() - 1);
        selection.cursor.setPosition(static_cast<int>(end), QTextCursor::KeepAnchor);
        selection.format = decoration.format;
        selections.append(selection);
    }
    if (navigationHoverStart >= 0 && navigationHoverLength > 0 &&
        navigationHoverStart < document()->characterCount() - 1) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(document());
        selection.cursor.setPosition(static_cast<int>(navigationHoverStart));
        selection.cursor.setPosition(static_cast<int>(qMin<qsizetype>(navigationHoverStart + navigationHoverLength,
            document()->characterCount() - 1)), QTextCursor::KeepAnchor);
        selection.format.setForeground(Qt::white);
        selection.format.setUnderlineColor(Qt::white);
        selection.format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        selections.append(selection);
    }
    setExtraSelections(selections);
    lineNumberArea->update();
}

void CodeEditor::setTextDecorations(const QVector<TextDecoration> &decorations)
{
    textDecorations = decorations;
    highlightCurrentLine();
}

void CodeEditor::toggleBreakpoint(int lineNumber)
{
    QTextBlock block = document()->findBlockByNumber(lineNumber - 1);
    if (!block.isValid())
        return;
    auto *data = static_cast<BreakpointBlockData *>(block.userData());
    if (data)
        block.setUserData(nullptr);
    else
        block.setUserData(new BreakpointBlockData());
    lineNumberArea->update();
    emit breakpointsChanged();
}

QSet<int> CodeEditor::breakpoints() const
{
    QSet<int> result;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const auto *data = dynamic_cast<const BreakpointBlockData *>(block.userData());
        if (data && data->breakpoint)
            result.insert(block.blockNumber() + 1);
    }
    return result;
}

void CodeEditor::setDebugLine(int lineNumber)
{
    debugLine = lineNumber;
    highlightCurrentLine();
}

void CodeEditor::setNavigationSymbols(const QVector<NavigationSymbol> &symbols)
{
    navigationSymbols = symbols;
    if (navigationHoverStart >= 0) {
        bool stillNavigable = false;
        const QString NAME = document()->toPlainText().mid(navigationHoverStart, navigationHoverLength);
        for (const NavigationSymbol &symbol : navigationSymbols) {
            if (symbol.name == NAME) {
                stillNavigable = true;
                break;
            }
        }
        if (!stillNavigable)
            updateNavigationHover(QPoint(-1, -1), false);
    }
}

const CodeEditor::NavigationSymbol *CodeEditor::navigationSymbolAt(const QPoint &viewportPosition,
                                                                    qsizetype *start, qsizetype *length) const
{
    if (viewportPosition.x() < 0 || viewportPosition.y() < 0)
        return nullptr;
    const QTextCursor cursor = cursorForPosition(viewportPosition);
    const QString LINE = cursor.block().text();
    int column = cursor.positionInBlock();
    if (column >= LINE.size())
        column = LINE.size() - 1;
    if (column < 0)
        return nullptr;
    const auto isIdentifierCharacter = [](QChar character) {
        return character.isLetterOrNumber() || character == QLatin1Char('_');
    };
    if (!isIdentifierCharacter(LINE.at(column))) {
        if (LINE.at(column) == QLatin1Char('$')) {
            ++column;
            if (column < LINE.size() && LINE.at(column) == QLatin1Char('{'))
                ++column;
        } else if (column + 1 < LINE.size() && isIdentifierCharacter(LINE.at(column + 1))) {
            ++column;
        } else if (column > 0 && isIdentifierCharacter(LINE.at(column - 1))) {
            --column;
        }
    }
    if (column < 0 || column >= LINE.size() || !isIdentifierCharacter(LINE.at(column)))
        return nullptr;
    int left = column;
    int right = column + 1;
    while (left > 0 && isIdentifierCharacter(LINE.at(left - 1)))
        --left;
    while (right < LINE.size() && isIdentifierCharacter(LINE.at(right)))
        ++right;
    const QString NAME = LINE.mid(left, right - left);
    for (const NavigationSymbol &symbol : navigationSymbols) {
        if (symbol.name != NAME)
            continue;
        if (start)
            *start = cursor.block().position() + left;
        if (length)
            *length = right - left;
        return &symbol;
    }
    return nullptr;
}

void CodeEditor::updateNavigationHover(const QPoint &viewportPosition, bool controlHeld)
{
    qsizetype start = -1;
    qsizetype length = 0;
    const NavigationSymbol *symbol = controlHeld
        ? navigationSymbolAt(viewportPosition, &start, &length) : nullptr;
    const qsizetype newStart = symbol ? start : -1;
    const qsizetype newLength = symbol ? length : 0;
    if (navigationHoverStart == newStart && navigationHoverLength == newLength)
        return;
    navigationHoverStart = newStart;
    navigationHoverLength = newLength;
    viewport()->setCursor(symbol ? Qt::PointingHandCursor : Qt::IBeamCursor);
    highlightCurrentLine();
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), QColor(QStringLiteral("#252525")));
    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            if (foldRegions.contains(blockNumber)) {
                QPainterPath marker;
                if (collapsedBlocks.contains(blockNumber)) {
                    marker.moveTo(4, top + fontMetrics().height() / 2 - 4);
                    marker.lineTo(4, top + fontMetrics().height() / 2 + 4);
                    marker.lineTo(10, top + fontMetrics().height() / 2);
                } else {
                    marker.moveTo(2, top + fontMetrics().height() / 2 - 2);
                    marker.lineTo(11, top + fontMetrics().height() / 2 - 2);
                    marker.lineTo(6, top + fontMetrics().height() / 2 + 4);
                }
                marker.closeSubpath();
                painter.setBrush(QColor(QStringLiteral("#aab4c0")));
                painter.setPen(Qt::NoPen);
                painter.drawPath(marker);
            }
            const auto *data = dynamic_cast<const BreakpointBlockData *>(block.userData());
            if (data && data->breakpoint)
                painter.setPen(QColor(QStringLiteral("#ff6666")));
            else if (blockNumber + 1 == debugLine)
                painter.setPen(QColor(QStringLiteral("#ffe082")));
            else
                painter.setPen(blockNumber == textCursor().blockNumber()
                                   ? QColor(QStringLiteral("#e5e7eb"))
                                   : QColor(QStringLiteral("#858585")));
            painter.drawText(16, top, lineNumberArea->width() - 20, fontMetrics().height(),
                             Qt::AlignRight, QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
}
