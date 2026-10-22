#include "TerminalOutput.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextFragment>
#include <QTextDocument>
#include <QStringList>

namespace ShellScratchpad::Output
{
namespace
{
struct FormattedRun
{
    QString text;
    QTextCharFormat format;
};

struct ScreenRow
{
    QVector<FormattedRun> runs;
    QTextBlockFormat format;
};
}

TerminalOutput::TerminalOutput(QTextDocument *document)
    : document(document), textCursor(document)
{
    updateFormat();
}

void TerminalOutput::append(const QString &text)
{
    QString printableText;
    const auto FLUSH_PRINTABLE = [this, &printableText]() {
        if (!printableText.isEmpty()) {
            writeText(printableText);
            printableText.clear();
        }
    };
    for (QChar character : text) {
        if (escapeState == EscapeState::Normal && !character.isNull() &&
            character >= QLatin1Char(' ') && character != QChar(0x7f)) {
            printableText.append(character);
            continue;
        }
        FLUSH_PRINTABLE();
        switch (escapeState) {
        case EscapeState::Normal:
            if (character == QChar(0x1b)) {
                escapeState = EscapeState::Escape;
            } else if (character == QLatin1Char('\r')) {
                textCursor.movePosition(QTextCursor::StartOfBlock);
            } else if (character == QLatin1Char('\n')) {
                textCursor.movePosition(QTextCursor::EndOfBlock);
                textCursor.insertBlock(QTextBlockFormat(), currentFormat);
                updateScreenTopForCursor();
            } else if (character == QLatin1Char('\b')) {
                if (textCursor.position() > textCursor.block().position())
                    textCursor.movePosition(QTextCursor::Left);
            } else if (character == QLatin1Char('\t')) {
                const int column = textCursor.position() - textCursor.block().position();
                moveToColumn(((column / 8) + 1) * 8);
            }
            break;
        case EscapeState::Escape:
            if (character == QLatin1Char('[')) {
                csiParameters.clear();
                escapeState = EscapeState::Csi;
            } else if (character == QLatin1Char(']')) {
                escapeState = EscapeState::Osc;
            } else if (character == QLatin1Char('7')) {
                savedCursorPosition = textCursor.position();
                escapeState = EscapeState::Normal;
            } else if (character == QLatin1Char('8')) {
                textCursor.setPosition(qBound(0, savedCursorPosition, document->characterCount() - 1));
                escapeState = EscapeState::Normal;
            } else if (character == QLatin1Char('D')) {
                textCursor.movePosition(QTextCursor::EndOfBlock);
                textCursor.insertBlock(QTextBlockFormat(), currentFormat);
                updateScreenTopForCursor();
                escapeState = EscapeState::Normal;
            } else if (character == QLatin1Char('M')) {
                moveVertical(-1);
                escapeState = EscapeState::Normal;
            } else {
                escapeState = EscapeState::Normal;
            }
            break;
        case EscapeState::Csi:
            if (character >= QLatin1Char('@') && character <= QLatin1Char('~')) {
                handleCsi(character);
                csiParameters.clear();
                escapeState = EscapeState::Normal;
            } else if (csiParameters.size() < 128) {
                csiParameters.append(character);
            } else {
                csiParameters.clear();
                escapeState = EscapeState::Normal;
            }
            break;
        case EscapeState::Osc:
            if (character == QChar(0x07))
                escapeState = EscapeState::Normal;
            else if (character == QChar(0x1b))
                escapeState = EscapeState::OscEscape;
            break;
        case EscapeState::OscEscape:
            escapeState = character == QLatin1Char('\\') ? EscapeState::Normal : EscapeState::Osc;
            break;
        }
    }
    FLUSH_PRINTABLE();
}

void TerminalOutput::setScreenRows(int rows)
{
    screenRows = qBound(0, rows, 1000);
    updateScreenTopForCursor();
}

void TerminalOutput::clear()
{
    document->clear();
    textCursor = QTextCursor(document);
    currentFormat = QTextCharFormat();
    escapeState = EscapeState::Normal;
    csiParameters.clear();
    bold = italic = underline = inverse = false;
    hasForeground = hasBackground = false;
    foreground = QColor();
    background = QColor();
    savedCursorPosition = 0;
    scrollingRegionTop = 0;
    scrollingRegionBottom = -1;
    scrollingRegionSet = false;
    screenTopRow = 0;
    updateFormat();
}

QString TerminalOutput::plainText() const
{
    return document->toPlainText();
}

QTextCursor TerminalOutput::cursor() const
{
    return textCursor;
}

void TerminalOutput::writeText(const QString &text)
{
    qsizetype offset = 0;
    while (offset < text.size()) {
        const QTextBlock block = textCursor.block();
        const int blockEnd = block.position() + block.length() - 1;
        const int replaceLength = textCursor.position() < blockEnd
            ? qMin<qsizetype>(text.size() - offset, blockEnd - textCursor.position()) : 0;
        if (replaceLength > 0) {
            textCursor.setPosition(textCursor.position() + replaceLength, QTextCursor::KeepAnchor);
            textCursor.insertText(text.mid(offset, replaceLength), currentFormat);
            offset += replaceLength;
        } else {
            textCursor.insertText(text.mid(offset), currentFormat);
            break;
        }
    }
}

void TerminalOutput::handleCsi(QChar finalCharacter)
{
    QString parameters = csiParameters;
    const bool privateMode = parameters.startsWith(QLatin1Char('?')) ||
                             parameters.startsWith(QLatin1Char('>')) ||
                             parameters.startsWith(QLatin1Char('!'));
    if (privateMode)
        parameters.remove(0, 1);
    const QStringList values = parameters.split(QLatin1Char(';'), Qt::KeepEmptyParts);
    const int first = parameter(values, 0, 1);
    switch (finalCharacter.toLatin1()) {
    case 'm': applySgr(parameters); break;
    case 'A': moveVertical(-qMax(first, 1)); break;
    case 'B': moveVertical(qMax(first, 1)); break;
    case 'C': moveToColumn(textCursor.position() - textCursor.block().position() + qMax(first, 1)); break;
    case 'D': moveToColumn(textCursor.position() - textCursor.block().position() - qMax(first, 1)); break;
    case 'E': moveVertical(qMax(first, 1)); textCursor.movePosition(QTextCursor::StartOfBlock); break;
    case 'F': moveVertical(-qMax(first, 1)); textCursor.movePosition(QTextCursor::StartOfBlock); break;
    case 'G': moveToColumn(qMax(first, 1) - 1); break;
    case 'H':
    case 'f': moveToPosition(qMax(parameter(values, 0, 1), 1) - 1,
                             qMax(parameter(values, 1, 1), 1) - 1); break;
    case 'J': eraseDisplay(parameter(values, 0)); break;
    case 'K': eraseLine(parameter(values, 0)); break;
    case 'X': {
        const int count = qBound(1, first, 4096);
        const int lineEnd = textCursor.block().position() + textCursor.block().length() - 1;
        for (int index = 0; index < count && textCursor.position() < lineEnd; ++index) {
            textCursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            textCursor.insertText(QStringLiteral(" "), currentFormat);
        }
        break;
    }
    case 'P': deleteCharacters(qMax(first, 1)); break;
    case '@': insertSpaces(qMax(first, 1)); break;
    case 'L': insertLines(qMax(first, 1)); break;
    case 'M': deleteLines(qMax(first, 1)); break;
    case 'S': scrollLines(qMax(first, 1), false); break;
    case 'T': scrollLines(qMax(first, 1), true); break;
    case 'r': setScrollingRegion(qMax(parameter(values, 0, 1), 1) - 1,
                                 qMax(parameter(values, 1, document->blockCount()), 1) - 1); break;
    case 's': savedCursorPosition = textCursor.position(); break;
    case 'u': textCursor.setPosition(qBound(0, savedCursorPosition, document->characterCount() - 1)); break;
    default: break;
    }
}

void TerminalOutput::applySgr(const QString &parameters)
{
    const QStringList pieces = parameters.isEmpty()
        ? QStringList{QStringLiteral("0")}
        : parameters.split(QLatin1Char(';'), Qt::KeepEmptyParts);
    for (int index = 0; index < pieces.size(); ++index) {
        const int code = pieces.at(index).isEmpty() ? 0 : pieces.at(index).toInt();
        if (code == 0) {
            bold = italic = underline = inverse = false;
            hasForeground = hasBackground = false;
        } else if (code == 1) bold = true;
        else if (code == 22) bold = false;
        else if (code == 3) italic = true;
        else if (code == 23) italic = false;
        else if (code == 4 || code == 21) underline = true;
        else if (code == 24) underline = false;
        else if (code == 7) inverse = true;
        else if (code == 27) inverse = false;
        else if (code == 39) hasForeground = false;
        else if (code == 49) hasBackground = false;
        else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
            foreground = ansiColor(code >= 90 ? code - 90 + 8 : code - 30);
            hasForeground = true;
        } else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
            background = ansiColor(code >= 100 ? code - 100 + 8 : code - 40);
            hasBackground = true;
        } else if ((code == 38 || code == 48) && index + 1 < pieces.size()) {
            const bool isForeground = code == 38;
            const int mode = pieces.at(++index).toInt();
            QColor color;
            if (mode == 5 && index + 1 < pieces.size()) {
                color = ansiColor(pieces.at(++index).toInt());
            } else if (mode == 2 && index + 3 < pieces.size()) {
                const int red = qBound(0, pieces.at(++index).toInt(), 255);
                const int green = qBound(0, pieces.at(++index).toInt(), 255);
                const int blue = qBound(0, pieces.at(++index).toInt(), 255);
                color = QColor(red, green, blue);
            }
            if (color.isValid()) {
                if (isForeground) {
                    foreground = color;
                    hasForeground = true;
                } else {
                    background = color;
                    hasBackground = true;
                }
            }
        }
    }
    updateFormat();
}

void TerminalOutput::eraseLine(int mode)
{
    const QTextBlock block = textCursor.block();
    const int start = block.position();
    const int end = start + block.length() - 1;
    const int originalPosition = textCursor.position();
    if (mode == 0) {
        textCursor.setPosition(originalPosition);
        textCursor.setPosition(end, QTextCursor::KeepAnchor);
        textCursor.removeSelectedText();
    } else if (mode == 1) {
        textCursor.setPosition(start);
        textCursor.setPosition(qMin(originalPosition, end), QTextCursor::KeepAnchor);
        textCursor.insertText(QString(qMax(0, qMin(originalPosition, end) - start), QLatin1Char(' ')), currentFormat);
        textCursor.setPosition(originalPosition);
    } else if (mode == 2) {
        textCursor.setPosition(start);
        textCursor.setPosition(end, QTextCursor::KeepAnchor);
        textCursor.removeSelectedText();
        insertSpaces(originalPosition - start);
    }
}

void TerminalOutput::eraseDisplay(int mode)
{
    const int end = document->characterCount() - 1;
    if (mode == 1) {
        const int position = textCursor.position();
        for (int index = 0; index < position; ++index) {
            if (document->characterAt(index) == QChar::ParagraphSeparator)
                continue;
            QTextCursor eraseCursor(document);
            eraseCursor.setPosition(index);
            eraseCursor.setPosition(index + 1, QTextCursor::KeepAnchor);
            eraseCursor.insertText(QStringLiteral(" "), currentFormat);
        }
        textCursor.setPosition(position);
    } else if (mode == 2 || mode == 3) {
        document->clear();
        textCursor = QTextCursor(document);
        screenTopRow = 0;
    } else {
        textCursor.setPosition(textCursor.position());
        textCursor.setPosition(end, QTextCursor::KeepAnchor);
        textCursor.removeSelectedText();
    }
}

void TerminalOutput::moveToColumn(int column)
{
    column = qBound(0, column, 10000);
    textCursor.movePosition(QTextCursor::StartOfBlock);
    const int start = textCursor.block().position();
    const int currentLength = textCursor.block().length() - 1;
    if (column > currentLength) {
        textCursor.setPosition(start + currentLength);
        insertSpaces(column - currentLength);
    } else {
        textCursor.setPosition(start + column);
    }
}

void TerminalOutput::moveToPosition(int row, int column)
{
    row = qBound(0, row, screenRows > 0 ? screenRows - 1 : 1000) + screenTopRow;
    while (!document->findBlockByNumber(row).isValid()) {
        textCursor.movePosition(QTextCursor::End);
        textCursor.insertBlock(QTextBlockFormat(), currentFormat);
    }
    textCursor = QTextCursor(document->findBlockByNumber(row));
    moveToColumn(column);
}

void TerminalOutput::moveVertical(int lines)
{
    if (lines == 0)
        return;
    const int targetRow = qBound(screenTopRow,
        textCursor.blockNumber() + lines,
        screenRows > 0 ? screenTopRow + screenRows - 1 : qMax(0, document->blockCount() - 1));
    const int column = textCursor.position() - textCursor.block().position();
    ensureLineCount(targetRow + 1);
    textCursor = QTextCursor(document->findBlockByNumber(targetRow));
    moveToColumn(column);
}

void TerminalOutput::insertSpaces(int count)
{
    count = qBound(0, count, 4096);
    if (count > 0)
        textCursor.insertText(QString(count, QLatin1Char(' ')), currentFormat);
}

void TerminalOutput::deleteCharacters(int count)
{
    const QTextBlock block = textCursor.block();
    const int end = block.position() + block.length() - 1;
    const int selectionEnd = qMin(end, textCursor.position() + qMax(count, 0));
    if (selectionEnd <= textCursor.position())
        return;
    textCursor.setPosition(selectionEnd, QTextCursor::KeepAnchor);
    textCursor.removeSelectedText();
}

void TerminalOutput::insertLines(int count)
{
    const int top = scrollingRegionSet ? screenTopRow + scrollingRegionTop : screenTopRow;
    const int bottom = scrollingRegionSet ? screenTopRow + scrollingRegionBottom
        : qMax(document->blockCount() - 1, screenTopRow + screenRows - 1);
    if (textCursor.blockNumber() < top || textCursor.blockNumber() > bottom)
        return;
    mutateLineRange(textCursor.blockNumber(), bottom, count, true);
}

void TerminalOutput::deleteLines(int count)
{
    const int top = scrollingRegionSet ? screenTopRow + scrollingRegionTop : screenTopRow;
    const int bottom = scrollingRegionSet ? screenTopRow + scrollingRegionBottom
        : qMax(document->blockCount() - 1, screenTopRow + screenRows - 1);
    if (textCursor.blockNumber() < top || textCursor.blockNumber() > bottom)
        return;
    mutateLineRange(textCursor.blockNumber(), bottom, count, false);
}

void TerminalOutput::scrollLines(int count, bool down)
{
    const int top = screenTopRow + (scrollingRegionSet ? scrollingRegionTop : 0);
    const int bottom = scrollingRegionSet ? screenTopRow + scrollingRegionBottom
        : qMax(document->blockCount() - 1, screenTopRow + screenRows - 1);
    mutateLineRange(top, bottom, count, down);
}

void TerminalOutput::mutateLineRange(int startRow, int bottomRow, int count, bool down)
{
    if (bottomRow < startRow || startRow < 0)
        return;
    ensureLineCount(bottomRow + 1);
    const int rowCount = bottomRow - startRow + 1;
    count = qBound(1, count, rowCount);
    const int cursorRow = textCursor.blockNumber();
    const int cursorColumn = textCursor.position() - textCursor.block().position();
    QVector<ScreenRow> originalRows;
    originalRows.reserve(rowCount);
    for (int row = startRow; row <= bottomRow; ++row) {
        ScreenRow screenRow;
        const QTextBlock block = document->findBlockByNumber(row);
        screenRow.format = block.blockFormat();
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const QTextFragment textFragment = fragment.fragment();
            if (textFragment.isValid())
                screenRow.runs.append({textFragment.text(), textFragment.charFormat()});
        }
        originalRows.append(screenRow);
    }

    QVector<ScreenRow> shiftedRows(rowCount);
    for (int destination = 0; destination < rowCount; ++destination) {
        const int source = down ? destination - count : destination + count;
        if (source >= 0 && source < rowCount)
            shiftedRows[destination] = originalRows.at(source);
        else
            shiftedRows[destination].format = originalRows.at(destination).format;
    }

    for (int offset = 0; offset < rowCount; ++offset) {
        QTextBlock block = document->findBlockByNumber(startRow + offset);
        QTextCursor rowCursor(block);
        rowCursor.movePosition(QTextCursor::StartOfBlock);
        rowCursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        rowCursor.removeSelectedText();
        rowCursor.setBlockFormat(shiftedRows.at(offset).format);
        for (const FormattedRun &run : shiftedRows.at(offset).runs)
            rowCursor.insertText(run.text, run.format);
    }

    const QTextBlock restoredBlock = document->findBlockByNumber(qBound(0, cursorRow, document->blockCount() - 1));
    textCursor = QTextCursor(restoredBlock);
    textCursor.setPosition(restoredBlock.position());
    moveToColumn(cursorColumn);
}

void TerminalOutput::setScrollingRegion(int top, int bottom)
{
    if (top >= bottom || (screenRows > 0 && screenRows < 2)) {
        scrollingRegionSet = false;
        scrollingRegionTop = 0;
        scrollingRegionBottom = -1;
        return;
    }
    const int maximumRow = screenRows > 0 ? screenRows - 1 : 1000;
    scrollingRegionSet = true;
    scrollingRegionTop = qBound(0, top, qMax(0, maximumRow - 1));
    scrollingRegionBottom = qBound(scrollingRegionTop + 1, bottom, maximumRow);
}

void TerminalOutput::ensureLineCount(int count)
{
    if (count <= document->blockCount())
        return;
    const int cursorPosition = textCursor.position();
    QTextCursor endCursor(document);
    endCursor.movePosition(QTextCursor::End);
    while (document->blockCount() < count)
        endCursor.insertBlock(QTextBlockFormat(), currentFormat);
    textCursor.setPosition(qBound(0, cursorPosition, document->characterCount() - 1));
}

void TerminalOutput::updateScreenTopForCursor()
{
    if (screenRows <= 0)
        return;
    screenTopRow = qMax(0, textCursor.blockNumber() - screenRows + 1);
}

void TerminalOutput::updateFormat()
{
    currentFormat = QTextCharFormat();
    currentFormat.setFontWeight(bold ? QFont::Bold : QFont::Normal);
    currentFormat.setFontItalic(italic);
    currentFormat.setFontUnderline(underline);
    const QPalette palette = QApplication::palette();
    const QColor defaultForeground = palette.color(QPalette::Text);
    const QColor defaultBackground = palette.color(QPalette::Base);
    currentFormat.setForeground(defaultForeground);
    if (inverse) {
        currentFormat.setForeground(hasBackground ? background : defaultBackground);
        currentFormat.setBackground(hasForeground ? foreground : defaultForeground);
    } else {
        if (hasForeground)
            currentFormat.setForeground(foreground);
        if (hasBackground)
            currentFormat.setBackground(background);
    }
}

QColor TerminalOutput::ansiColor(int index)
{
    static const QColor COLORS[16] = {
        QColor(0, 0, 0), QColor(205, 49, 49), QColor(13, 188, 121), QColor(229, 229, 16),
        QColor(36, 114, 200), QColor(188, 63, 188), QColor(17, 168, 205), QColor(229, 229, 229),
        QColor(102, 102, 102), QColor(241, 76, 76), QColor(35, 209, 139), QColor(245, 245, 67),
        QColor(59, 142, 234), QColor(214, 112, 214), QColor(41, 184, 219), QColor(255, 255, 255)
    };
    if (index >= 0 && index < 16)
        return COLORS[index];
    if (index >= 232 && index <= 255) {
        const int component = 8 + (index - 232) * 10;
        return QColor(component, component, component);
    }
    if (index >= 16 && index <= 231) {
        const int value = index - 16;
        const int red = value / 36;
        const int green = (value / 6) % 6;
        const int blue = value % 6;
        const auto component = [](int channel) { return channel == 0 ? 0 : 55 + channel * 40; };
        return QColor(component(red), component(green), component(blue));
    }
    return {};
}

int TerminalOutput::parameter(const QStringList &parameters, int index, int defaultValue)
{
    if (index < 0 || index >= parameters.size() || parameters.at(index).isEmpty())
        return defaultValue;
    bool valid = false;
    const int value = parameters.at(index).toInt(&valid);
    return valid ? value : defaultValue;
}
}
