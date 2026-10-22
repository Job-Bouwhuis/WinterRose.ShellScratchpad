#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>

class QTextDocument;

namespace ShellScratchpad::Output
{
class TerminalOutput final
{
public:
    explicit TerminalOutput(QTextDocument *document);

    void append(const QString &text);
    void clear();
    QString plainText() const;
    QTextCursor cursor() const;

private:
    enum class EscapeState { Normal, Escape, Csi, Osc, OscEscape };

    void writeText(const QString &text);
    void handleCsi(QChar finalCharacter);
    void applySgr(const QString &parameters);
    void eraseLine(int mode);
    void eraseDisplay(int mode);
    void moveToColumn(int column);
    void moveToPosition(int row, int column);
    void moveVertical(int lines);
    void insertSpaces(int count);
    void deleteCharacters(int count);
    void updateFormat();
    static QColor ansiColor(int index);
    static int parameter(const QStringList &parameters, int index, int defaultValue = 0);

    QTextDocument *document = nullptr;
    QTextCursor textCursor;
    QTextCharFormat currentFormat;
    EscapeState escapeState = EscapeState::Normal;
    QString csiParameters;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool hasForeground = false;
    bool hasBackground = false;
    QColor foreground;
    QColor background;
    int savedCursorPosition = 0;
};
}
