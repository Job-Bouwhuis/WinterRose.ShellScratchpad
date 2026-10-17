#include "SyntaxHighlighter.h"

#include "SyntaxTokenizer.h"
#include "SyntaxColorSettings.h"

#include <QColor>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <algorithm>

namespace ShellScratchpad::Syntax
{
namespace
{
QTextCharFormat formatForToken(TokenKind kind, bool inHereDoc = false)
{
    QTextCharFormat format;
    const QColor COLOR = SyntaxColorSettings::instance()->color(kind);
    if (COLOR.isValid())
        format.setForeground(COLOR);
    if (inHereDoc)
        format.setBackground(SyntaxColorSettings::instance()->hereDocBackground());
    switch (kind) {
    case TokenKind::HereDocContent:
        format.setBackground(SyntaxColorSettings::instance()->hereDocBackground());
        break;
    case TokenKind::Command:
    case TokenKind::Builtin:
    case TokenKind::Keyword:
        format.setFontWeight(QFont::DemiBold);
        break;
    case TokenKind::CommandSubstitutionStart:
    case TokenKind::ArithmeticExpansionStart:
    case TokenKind::BacktickCommandSubstitutionDelimiter:
        break;
    case TokenKind::Argument:
    case TokenKind::Word:
        break;
    default:
        break;
    }
    return format;
}
}

SyntaxHighlighter::SyntaxHighlighter(QPlainTextEdit *editor)
    : QSyntaxHighlighter(static_cast<QTextDocument *>(nullptr))
{
    tokens = SyntaxTokenizer::tokenize(editor->toPlainText());
    setParent(editor);
    connect(SyntaxColorSettings::instance(), &SyntaxColorSettings::colorsChanged,
            this, &QSyntaxHighlighter::rehighlight);
    connect(editor, &QPlainTextEdit::textChanged, this, [this, editor]() {
        // Applying formats can emit another textChanged notification; ignore that nested signal.
        if (rehighlighting)
            return;
        tokens = SyntaxTokenizer::tokenize(editor->toPlainText());
        rehighlighting = true;
        rehighlight();
        rehighlighting = false;
    });
    setDocument(editor->document());
}

void SyntaxHighlighter::highlightBlock(const QString &text)
{
    const qsizetype BLOCK_START = currentBlock().position();
    const qsizetype BLOCK_END = BLOCK_START + text.size();
    for (const SyntaxToken &TOKEN : tokens) {
        if (TOKEN.kind != TokenKind::HereDocContent)
            continue;
        const qsizetype TOKEN_END = TOKEN.startOffset + TOKEN.length;
        const qsizetype START = qMax(TOKEN.startOffset, BLOCK_START);
        const qsizetype END = qMin(TOKEN_END, BLOCK_END);
        if (START >= END)
            continue;
        setFormat(static_cast<int>(START - BLOCK_START),
                  static_cast<int>(END - START),
                  formatForToken(TOKEN.kind));
    }
    for (const SyntaxToken &TOKEN : tokens) {
        if (TOKEN.kind == TokenKind::HereDocContent)
            continue;
        const qsizetype TOKEN_END = TOKEN.startOffset + TOKEN.length;
        const qsizetype START = qMax(TOKEN.startOffset, BLOCK_START);
        const qsizetype END = qMin(TOKEN_END, BLOCK_END);
        if (START >= END)
            continue;
        const bool IN_HEREDOC = std::any_of(tokens.cbegin(), tokens.cend(), [&TOKEN, TOKEN_END](const SyntaxToken &span) {
            return span.kind == TokenKind::HereDocContent && TOKEN.startOffset < span.startOffset + span.length &&
                   TOKEN_END > span.startOffset;
        });
        setFormat(static_cast<int>(START - BLOCK_START),
                  static_cast<int>(END - START),
                  formatForToken(TOKEN.kind, IN_HEREDOC));
    }
}
}
