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
#include <algorithm>

namespace ShellScratchpad
{
namespace
{
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
        if (event->button() == Qt::LeftButton && event->position().x() < 15) {
            codeEditor->handleGutterClick(qRound(event->position().y()));
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CodeEditor *codeEditor;
};
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
    if (event->type() == QEvent::FontChange)
        updateLineNumberAreaWidth();
}

void CodeEditor::mousePressEvent(QMouseEvent *event)
{
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
    setExtraSelections(selections);
    lineNumberArea->update();
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
            painter.setPen(blockNumber + 1 == textCursor().blockNumber() + 1
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
