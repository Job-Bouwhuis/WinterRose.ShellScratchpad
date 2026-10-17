#pragma once

#include <QString>
#include <QVector>

namespace ShellScratchpad::Syntax
{
enum class TokenKind
{
    Comment,
    StringDelimiter,
    StringContent,
    Variable,
    SpecialParameter,
    EscapeSequence,
    CommandSubstitutionStart,
    BacktickCommandSubstitutionDelimiter,
    ArithmeticExpansionStart,
    Operator,
    Redirection,
    Number,
    Word,
    Command,
    Builtin,
    Keyword,
    AssignmentName,
    Option,
    Path,
    Argument,
    HereDocContent
};

struct SyntaxToken
{
    qsizetype startOffset = 0;
    qsizetype length = 0;
    int line = 1;
    int column = 1;
    TokenKind kind = TokenKind::Word;
};

QString tokenKindName(TokenKind kind);
QString printableTokenText(const QString &source, const SyntaxToken &token);
}
