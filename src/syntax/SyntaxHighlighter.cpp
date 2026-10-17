#include "SyntaxHighlighter.h"

#include "SyntaxTokenizer.h"

#include <QColor>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>

namespace ShellScratchpad::Syntax
{
namespace
{
QTextCharFormat formatForToken(TokenKind kind)
{
    QTextCharFormat format;
    switch (kind) {
    case TokenKind::Comment: format.setForeground(QColor(QStringLiteral("#8bc98b"))); break;
    case TokenKind::StringDelimiter:
    case TokenKind::StringContent: format.setForeground(QColor(QStringLiteral("#e6c07b"))); break;
    case TokenKind::Variable: format.setForeground(QColor(QStringLiteral("#56c8c8"))); break;
    case TokenKind::SpecialParameter: format.setForeground(QColor(QStringLiteral("#61afef"))); break;
    case TokenKind::EscapeSequence: format.setForeground(QColor(QStringLiteral("#c678dd"))); break;
    case TokenKind::Command: format.setForeground(QColor(QStringLiteral("#61afef"))); format.setFontWeight(QFont::DemiBold); break;
    case TokenKind::Builtin: format.setForeground(QColor(QStringLiteral("#56b6c2"))); format.setFontWeight(QFont::DemiBold); break;
    case TokenKind::Keyword: format.setForeground(QColor(QStringLiteral("#c678dd"))); format.setFontWeight(QFont::DemiBold); break;
    case TokenKind::AssignmentName: format.setForeground(QColor(QStringLiteral("#e5c07b"))); break;
    case TokenKind::Option: format.setForeground(QColor(QStringLiteral("#d19a66"))); break;
    case TokenKind::Path: format.setForeground(QColor(QStringLiteral("#98c379"))); break;
    case TokenKind::Redirection: format.setForeground(QColor(QStringLiteral("#e06c75"))); break;
    case TokenKind::Operator: format.setForeground(QColor(QStringLiteral("#abb2bf"))); break;
    case TokenKind::Number: format.setForeground(QColor(QStringLiteral("#d19a66"))); break;
    case TokenKind::CommandSubstitutionStart:
    case TokenKind::ArithmeticExpansionStart:
    case TokenKind::BacktickCommandSubstitutionDelimiter:
        format.setForeground(QColor(QStringLiteral("#c678dd")));
        break;
    case TokenKind::Argument:
    case TokenKind::Word:
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
        const qsizetype TOKEN_END = TOKEN.startOffset + TOKEN.length;
        const qsizetype START = qMax(TOKEN.startOffset, BLOCK_START);
        const qsizetype END = qMin(TOKEN_END, BLOCK_END);
        if (START >= END)
            continue;
        setFormat(static_cast<int>(START - BLOCK_START),
                  static_cast<int>(END - START),
                  formatForToken(TOKEN.kind));
    }
}
}
