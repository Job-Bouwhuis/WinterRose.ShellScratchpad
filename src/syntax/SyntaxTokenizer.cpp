#include "SyntaxTokenizer.h"

#include <QStringList>
#include <QChar>
#include <QSet>
#include <algorithm>

namespace ShellScratchpad::Syntax
{
namespace
{
struct Position
{
    qsizetype offset = 0;
    int line = 1;
    int column = 1;
};

bool isIdentifierStart(QChar character)
{
    const ushort CHARACTER_CODE = character.unicode();
    return CHARACTER_CODE == static_cast<ushort>('_') ||
           (CHARACTER_CODE >= static_cast<ushort>('A') && CHARACTER_CODE <= static_cast<ushort>('Z')) ||
           (CHARACTER_CODE >= static_cast<ushort>('a') && CHARACTER_CODE <= static_cast<ushort>('z'));
}

bool isIdentifierPart(QChar character)
{
    const ushort CHARACTER_CODE = character.unicode();
    return isIdentifierStart(character) ||
           (CHARACTER_CODE >= static_cast<ushort>('0') && CHARACTER_CODE <= static_cast<ushort>('9'));
}

bool isSpecialParameter(QChar character)
{
    static const QString SPECIAL_PARAMETERS = QStringLiteral("?#$!@*-_");
    return SPECIAL_PARAMETERS.contains(character) || character.isDigit();
}

bool isNumber(const QString &text)
{
    if (text.isEmpty())
        return false;

    bool HAS_DIGIT = false;
    bool HAS_DOT = false;
    for (const QChar CHARACTER : text) {
        if (CHARACTER.unicode() >= static_cast<ushort>('0') &&
            CHARACTER.unicode() <= static_cast<ushort>('9')) {
            HAS_DIGIT = true;
            continue;
        }
        if (CHARACTER == QLatin1Char('.') && !HAS_DOT) {
            HAS_DOT = true;
            continue;
        }
        return false;
    }
    return HAS_DIGIT;
}

const QStringList &operatorSpellings()
{
    // Keep longer spellings first so that, for example, && is not split into two ampersands.
    static const QStringList OPERATOR_SPELLINGS = {
        QStringLiteral("2>&1"), QStringLiteral("&>>"), QStringLiteral("<<<"),
        QStringLiteral("<<-"), QStringLiteral(";;&"), QStringLiteral("2>>"),
        QStringLiteral("2>"), QStringLiteral("&>"),
        QStringLiteral(">>"), QStringLiteral("<<"), QStringLiteral(">&"),
        QStringLiteral("<&"), QStringLiteral("<>"), QStringLiteral(">|"),
        QStringLiteral("&&"), QStringLiteral("||"), QStringLiteral("|&"),
        QStringLiteral(";;"), QStringLiteral(";&"), QStringLiteral(";"),
        QStringLiteral("|"), QStringLiteral("&"), QStringLiteral("("),
        QStringLiteral(")"), QStringLiteral("{"), QStringLiteral("}"),
        QStringLiteral("<"), QStringLiteral(">")
    };
    return OPERATOR_SPELLINGS;
}

bool startsOperator(const QString &source, qsizetype offset, QString *matched = nullptr,
                    bool allowFileDescriptorPrefix = true)
{
    for (const QString &SPELLING : operatorSpellings()) {
        if (!allowFileDescriptorPrefix && SPELLING.startsWith(QStringLiteral("2>")))
            continue;
        if (source.mid(offset, SPELLING.size()) == SPELLING) {
            if (matched)
                *matched = SPELLING;
            return true;
        }
    }
    return false;
}

bool parseHereDocDelimiter(const QString &source, qsizetype start, QString *delimiter)
{
    while (start < source.size() && (source.at(start) == QLatin1Char(' ') ||
                                     source.at(start) == QLatin1Char('\t')))
        ++start;
    if (start >= source.size() || source.at(start) == QLatin1Char('\n'))
        return false;

    if (source.at(start) == QLatin1Char('\'') || source.at(start) == QLatin1Char('"')) {
        const QChar QUOTE = source.at(start++);
        while (start < source.size() && source.at(start) != QLatin1Char('\n')) {
            if (source.at(start) == QUOTE)
                return !delimiter->isEmpty();
            if (source.at(start) == QLatin1Char('\\') && start + 1 < source.size() &&
                source.at(start + 1) != QLatin1Char('\n'))
                ++start;
            delimiter->append(source.at(start++));
        }
        return false;
    }

    while (start < source.size()) {
        const QChar CHARACTER = source.at(start);
        if (CHARACTER.isSpace() || CHARACTER == QLatin1Char(';') || CHARACTER == QLatin1Char('|') ||
            CHARACTER == QLatin1Char('&') || CHARACTER == QLatin1Char('<') || CHARACTER == QLatin1Char('>'))
            break;
        if (CHARACTER == QLatin1Char('\\') && start + 1 < source.size() &&
            source.at(start + 1) != QLatin1Char('\n'))
            ++start;
        delimiter->append(source.at(start++));
    }
    return !delimiter->isEmpty();
}

QVector<SyntaxToken> findHereDocSpans(const QString &source,
                                      const QVector<SyntaxToken> &tokens)
{
    QVector<SyntaxToken> spans;
    qsizetype nextBodyOffset = 0;
    for (const SyntaxToken &token : tokens) {
        const bool IS_IN_HEREDOC_BODY = std::any_of(spans.cbegin(), spans.cend(), [&token](const SyntaxToken &span) {
            return token.startOffset >= span.startOffset &&
                   token.startOffset < span.startOffset + span.length;
        });
        if (token.kind != TokenKind::Redirection || IS_IN_HEREDOC_BODY)
            continue;
        const QString OPERATOR_TEXT = source.mid(token.startOffset, token.length);
        if (OPERATOR_TEXT != QStringLiteral("<<") && OPERATOR_TEXT != QStringLiteral("<<-"))
            continue;

        QString delimiter;
        bool stripTabs = OPERATOR_TEXT == QStringLiteral("<<-");
        if (!parseHereDocDelimiter(source, token.startOffset + token.length, &delimiter))
            continue;

        const qsizetype lineEnd = source.indexOf(QLatin1Char('\n'), token.startOffset);
        qsizetype bodyStart = lineEnd < 0 ? source.size() : lineEnd + 1;
        bodyStart = qMax(bodyStart, nextBodyOffset);
        qsizetype lineStart = bodyStart;
        qsizetype bodyEnd = source.size();
        bool foundTerminator = false;
        while (lineStart < source.size()) {
            qsizetype currentLineEnd = source.indexOf(QLatin1Char('\n'), lineStart);
            if (currentLineEnd < 0)
                currentLineEnd = source.size();
            QString currentLine = source.mid(lineStart, currentLineEnd - lineStart);
            if (currentLine.endsWith(QLatin1Char('\r')))
                currentLine.chop(1);
            if (stripTabs) {
                while (currentLine.startsWith(QLatin1Char('\t')))
                    currentLine.remove(0, 1);
            }
            if (currentLine == delimiter) {
                bodyEnd = currentLineEnd < source.size() ? currentLineEnd + 1 : currentLineEnd;
                foundTerminator = true;
                break;
            }
            lineStart = currentLineEnd < source.size() ? currentLineEnd + 1 : source.size();
        }

        if (bodyEnd > bodyStart) {
            int line = 1;
            int column = 1;
            for (qsizetype index = 0; index < bodyStart; ++index) {
                if (source.at(index) == QLatin1Char('\n')) {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            spans.append(SyntaxToken{bodyStart, bodyEnd - bodyStart, line, column,
                                     TokenKind::HereDocContent});
        }
        nextBodyOffset = foundTerminator ? bodyEnd : source.size();
    }
    return spans;
}

class Lexer final
{
public:
    explicit Lexer(const QString &text)
        : source(text)
    {
    }

    QVector<SyntaxToken> run()
    {
        while (!atEnd())
            scanNext();
        return tokens;
    }

private:
    const QString &source;
    Position position;
    QVector<SyntaxToken> tokens;

    bool atEnd() const
    {
        return position.offset >= source.size();
    }

    QChar peek(qsizetype lookAhead = 0) const
    {
        const qsizetype INDEX = position.offset + lookAhead;
        return INDEX >= 0 && INDEX < source.size() ? source.at(INDEX) : QChar();
    }

    void advance()
    {
        if (atEnd())
            return;

        const QChar CHARACTER = source.at(position.offset++);
        if (CHARACTER == QLatin1Char('\n')) {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }

    void advance(qsizetype count)
    {
        for (qsizetype INDEX = 0; INDEX < count; ++INDEX)
            advance();
    }

    Position currentPosition() const
    {
        return position;
    }

    void addToken(TokenKind kind, const Position &start)
    {
        const qsizetype LENGTH = position.offset - start.offset;
        if (LENGTH <= 0)
            return;
        tokens.append(SyntaxToken{start.offset, LENGTH, start.line, start.column, kind});
    }

    void scanNext()
    {
        const QChar CHARACTER = peek();
        if (CHARACTER.isSpace()) {
            advance();
            return;
        }

        if (CHARACTER == QLatin1Char('#')) {
            scanComment();
            return;
        }

        if (CHARACTER == QLatin1Char('\'')) {
            scanSingleQuotedString();
            return;
        }

        if (CHARACTER == QLatin1Char('"')) {
            scanDoubleQuotedString();
            return;
        }

        if (CHARACTER == QLatin1Char('$')) {
            scanDollarExpansion();
            return;
        }

        if (CHARACTER == QLatin1Char('\\')) {
            scanEscapeSequence();
            return;
        }

        if (CHARACTER == QLatin1Char('`')) {
            const Position START = currentPosition();
            advance();
            addToken(TokenKind::BacktickCommandSubstitutionDelimiter, START);
            return;
        }

        QString MATCHED_OPERATOR;
        if (startsOperator(source, position.offset, &MATCHED_OPERATOR)) {
            scanOperator(MATCHED_OPERATOR);
            return;
        }

        scanWordOrNumber();
    }

    void scanComment()
    {
        const Position START = currentPosition();
        while (!atEnd() && peek() != QLatin1Char('\n'))
            advance();
        addToken(TokenKind::Comment, START);
    }

    void scanSingleQuotedString()
    {
        const Position OPENING_QUOTE = currentPosition();
        advance();
        addToken(TokenKind::StringDelimiter, OPENING_QUOTE);

        const Position CONTENT_START = currentPosition();
        while (!atEnd() && peek() != QLatin1Char('\''))
            advance();
        addToken(TokenKind::StringContent, CONTENT_START);

        if (!atEnd()) {
            const Position CLOSING_QUOTE = currentPosition();
            advance();
            addToken(TokenKind::StringDelimiter, CLOSING_QUOTE);
        }
    }

    void scanDoubleQuotedString()
    {
        const Position OPENING_QUOTE = currentPosition();
        advance();
        addToken(TokenKind::StringDelimiter, OPENING_QUOTE);

        Position CONTENT_START = currentPosition();
        while (!atEnd()) {
            if (peek() == QLatin1Char('"')) {
                addToken(TokenKind::StringContent, CONTENT_START);
                const Position CLOSING_QUOTE = currentPosition();
                advance();
                addToken(TokenKind::StringDelimiter, CLOSING_QUOTE);
                return;
            }

            if (peek() == QLatin1Char('$')) {
                addToken(TokenKind::StringContent, CONTENT_START);
                scanDollarExpansion();
                CONTENT_START = currentPosition();
                continue;
            }

            if (peek() == QLatin1Char('\\')) {
                const QChar NEXT_CHARACTER = peek(1);
                const bool IS_SPECIAL_DOUBLE_QUOTE_ESCAPE =
                    NEXT_CHARACTER == QLatin1Char('$') || NEXT_CHARACTER == QLatin1Char('`') ||
                    NEXT_CHARACTER == QLatin1Char('"') || NEXT_CHARACTER == QLatin1Char('\\') ||
                    NEXT_CHARACTER == QLatin1Char('\n');
                if (IS_SPECIAL_DOUBLE_QUOTE_ESCAPE) {
                    addToken(TokenKind::StringContent, CONTENT_START);
                    scanEscapeSequence();
                    CONTENT_START = currentPosition();
                    continue;
                }
            }

            advance();
        }

        // An unfinished string is still a useful tokenization result, not a parser failure.
        addToken(TokenKind::StringContent, CONTENT_START);
    }

    void scanDollarExpansion()
    {
        const Position START = currentPosition();
        advance();
        if (atEnd()) {
            addToken(TokenKind::Word, START);
            return;
        }

        if (peek() == QLatin1Char('(')) {
            advance();
            if (peek() == QLatin1Char('(')) {
                advance();
                addToken(TokenKind::ArithmeticExpansionStart, START);
            } else {
                addToken(TokenKind::CommandSubstitutionStart, START);
            }
            return;
        }

        if (peek() == QLatin1Char('{')) {
            advance();
            int BRACE_DEPTH = 1;
            while (!atEnd() && BRACE_DEPTH > 0) {
                if (peek() == QLatin1Char('\\')) {
                    advance();
                    if (!atEnd())
                        advance();
                    continue;
                }
                if (peek() == QLatin1Char('{'))
                    ++BRACE_DEPTH;
                else if (peek() == QLatin1Char('}'))
                    --BRACE_DEPTH;
                advance();
            }
            addToken(TokenKind::Variable, START);
            return;
        }

        if (isIdentifierStart(peek())) {
            while (isIdentifierPart(peek()))
                advance();
            addToken(TokenKind::Variable, START);
            return;
        }

        if (isSpecialParameter(peek())) {
            advance();
            addToken(TokenKind::SpecialParameter, START);
            return;
        }

        // A dollar sign not followed by a valid expansion introducer is literal text.
        addToken(TokenKind::Word, START);
    }

    void scanEscapeSequence()
    {
        const Position START = currentPosition();
        advance();
        if (!atEnd())
            advance();
        addToken(TokenKind::EscapeSequence, START);
    }

    void scanOperator(const QString &spelling)
    {
        const Position START = currentPosition();
        advance(spelling.size());

        const bool IS_REDIRECTION = spelling.startsWith(QLatin1Char('<')) ||
                                    spelling.startsWith(QLatin1Char('>')) ||
                                    spelling.startsWith(QStringLiteral("2>")) ||
                                    spelling.startsWith(QStringLiteral("&>"));
        addToken(IS_REDIRECTION ? TokenKind::Redirection : TokenKind::Operator, START);
    }

    void scanWordOrNumber()
    {
        const Position START = currentPosition();
        while (!atEnd()) {
            const QChar CHARACTER = peek();
            if (CHARACTER.isSpace() || CHARACTER == QLatin1Char('\'') ||
                CHARACTER == QLatin1Char('"') || CHARACTER == QLatin1Char('$') ||
                CHARACTER == QLatin1Char('\\') || CHARACTER == QLatin1Char('`'))
                break;

            QString MATCHED_OPERATOR;
            if (startsOperator(source, position.offset, &MATCHED_OPERATOR, false))
                break;

            advance();
        }

        // Guard against unsupported single-character input getting stuck in the scanner.
        if (position.offset == START.offset) {
            advance();
            addToken(TokenKind::Word, START);
            return;
        }

        const QString TEXT = source.mid(START.offset, position.offset - START.offset);
        addToken(isNumber(TEXT) ? TokenKind::Number : TokenKind::Word, START);
    }
};
}

QVector<SyntaxToken> SyntaxTokenizer::tokenize(const QString &source)
{
    Lexer LEXER(source);
    const QVector<SyntaxToken> LEXICAL_TOKENS = LEXER.run();
    QVector<SyntaxToken> result;
    result.reserve(LEXICAL_TOKENS.size());
    static const QSet<QString> KEYWORDS = {
        QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("elif"),
        QStringLiteral("else"), QStringLiteral("fi"), QStringLiteral("for"),
        QStringLiteral("while"), QStringLiteral("until"), QStringLiteral("do"),
        QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
        QStringLiteral("in"), QStringLiteral("function"), QStringLiteral("select"),
        QStringLiteral("time"), QStringLiteral("coproc")
    };
    static const QSet<QString> BUILTINS = {
        QStringLiteral(":"), QStringLiteral("."), QStringLiteral("["), QStringLiteral("]]"),
        QStringLiteral("alias"), QStringLiteral("bg"), QStringLiteral("bind"),
        QStringLiteral("break"), QStringLiteral("builtin"), QStringLiteral("caller"),
        QStringLiteral("cd"), QStringLiteral("command"), QStringLiteral("compgen"),
        QStringLiteral("complete"), QStringLiteral("continue"), QStringLiteral("declare"),
        QStringLiteral("dirs"), QStringLiteral("disown"), QStringLiteral("echo"),
        QStringLiteral("enable"), QStringLiteral("eval"), QStringLiteral("exec"),
        QStringLiteral("exit"), QStringLiteral("export"), QStringLiteral("false"),
        QStringLiteral("fc"), QStringLiteral("fg"), QStringLiteral("getopts"),
        QStringLiteral("hash"), QStringLiteral("help"), QStringLiteral("history"),
        QStringLiteral("jobs"), QStringLiteral("kill"), QStringLiteral("let"),
        QStringLiteral("local"), QStringLiteral("logout"), QStringLiteral("mapfile"),
        QStringLiteral("popd"), QStringLiteral("printf"), QStringLiteral("pushd"),
        QStringLiteral("pwd"), QStringLiteral("read"), QStringLiteral("readarray"),
        QStringLiteral("readonly"), QStringLiteral("return"), QStringLiteral("set"),
        QStringLiteral("shift"), QStringLiteral("shopt"), QStringLiteral("source"),
        QStringLiteral("suspend"), QStringLiteral("test"), QStringLiteral("times"),
        QStringLiteral("trap"), QStringLiteral("true"), QStringLiteral("type"),
        QStringLiteral("typeset"), QStringLiteral("ulimit"), QStringLiteral("umask"),
        QStringLiteral("unalias"), QStringLiteral("unset"), QStringLiteral("wait")
    };
    bool expectingCommand = true;
    bool expectingRedirectionTarget = false;
    int previousLine = 1;
    for (const SyntaxToken &TOKEN : LEXICAL_TOKENS) {
        const QString TEXT = source.mid(TOKEN.startOffset, TOKEN.length);
        if (TOKEN.line > previousLine && !source.mid(0, TOKEN.startOffset).trimmed().endsWith(QLatin1Char('\\')))
            expectingCommand = true;
        previousLine = TOKEN.line;
        if (TOKEN.kind == TokenKind::Operator) {
            result.append(TOKEN);
            if (TEXT == QStringLiteral(";") || TEXT == QStringLiteral("&&") || TEXT == QStringLiteral("||") ||
                TEXT == QStringLiteral("|") || TEXT == QStringLiteral("|&") || TEXT == QStringLiteral("{") ||
                TEXT == QStringLiteral("("))
                expectingCommand = true;
            continue;
        }
        if (TOKEN.kind == TokenKind::Redirection) {
            result.append(TOKEN);
            expectingRedirectionTarget = TEXT != QStringLiteral("2>&1") && TEXT != QStringLiteral(">&") &&
                TEXT != QStringLiteral("<&") && !TEXT.startsWith(QStringLiteral("<<"));
            continue;
        }
        if (TOKEN.kind == TokenKind::Comment ||
            TOKEN.kind == TokenKind::StringContent || TOKEN.kind == TokenKind::StringDelimiter ||
            TOKEN.kind == TokenKind::Variable || TOKEN.kind == TokenKind::SpecialParameter ||
            TOKEN.kind == TokenKind::EscapeSequence || TOKEN.kind == TokenKind::CommandSubstitutionStart ||
            TOKEN.kind == TokenKind::ArithmeticExpansionStart || TOKEN.kind == TokenKind::BacktickCommandSubstitutionDelimiter) {
            result.append(TOKEN);
            continue;
        }
        if (TOKEN.kind != TokenKind::Word && TOKEN.kind != TokenKind::Number) {
            if (expectingRedirectionTarget)
                expectingRedirectionTarget = false;
            result.append(TOKEN);
            continue;
        }
        if (expectingRedirectionTarget) {
            SyntaxToken target = TOKEN;
            target.kind = (TEXT.startsWith(QLatin1Char('/')) || TEXT.startsWith(QStringLiteral("./")) ||
                           TEXT.startsWith(QStringLiteral("../")) || TEXT.contains(QLatin1Char('/')))
                              ? TokenKind::Path : TokenKind::Argument;
            result.append(target);
            expectingRedirectionTarget = false;
            continue;
        }
        const qsizetype EQUALS = TEXT.indexOf(QLatin1Char('='));
        const QString NAME = EQUALS > 0 ? TEXT.left(EQUALS) : QString();
        const bool IS_ASSIGNMENT = !NAME.isEmpty() && isIdentifierStart(NAME.at(0)) &&
            std::all_of(NAME.cbegin(), NAME.cend(), [](QChar c) { return isIdentifierPart(c); });
        if (IS_ASSIGNMENT) {
            SyntaxToken nameToken = TOKEN;
            nameToken.length = NAME.size();
            nameToken.kind = TokenKind::AssignmentName;
            result.append(nameToken);
            SyntaxToken equalsToken = TOKEN;
            equalsToken.startOffset += EQUALS;
            equalsToken.column += static_cast<int>(EQUALS);
            equalsToken.length = 1;
            equalsToken.kind = TokenKind::Operator;
            result.append(equalsToken);
            if (EQUALS + 1 < TEXT.size()) {
                SyntaxToken valueToken = TOKEN;
                valueToken.startOffset += EQUALS + 1;
                valueToken.column += static_cast<int>(EQUALS + 1);
                valueToken.length = TEXT.size() - EQUALS - 1;
                valueToken.kind = TokenKind::Argument;
                result.append(valueToken);
            }
            continue;
        }
        SyntaxToken classified = TOKEN;
        if (TEXT == QStringLiteral("]") || TEXT == QStringLiteral("[[") || TEXT == QStringLiteral("]]")) {
            classified.kind = TokenKind::Builtin;
        } else if (KEYWORDS.contains(TEXT)) {
            classified.kind = TokenKind::Keyword;
            if (TEXT == QStringLiteral("if") || TEXT == QStringLiteral("then") || TEXT == QStringLiteral("else") ||
                TEXT == QStringLiteral("elif") || TEXT == QStringLiteral("do"))
                expectingCommand = true;
            else if (TEXT == QStringLiteral("for") || TEXT == QStringLiteral("select") ||
                     TEXT == QStringLiteral("function") || TEXT == QStringLiteral("case"))
                expectingCommand = false;
        } else if (expectingCommand) {
            classified.kind = BUILTINS.contains(TEXT) ? TokenKind::Builtin : TokenKind::Command;
            expectingCommand = false;
        } else if (TEXT.startsWith(QLatin1Char('-')) && TEXT.size() > 1) {
            classified.kind = TokenKind::Option;
        } else if (TEXT.startsWith(QLatin1Char('/')) || TEXT.startsWith(QStringLiteral("./")) ||
                   TEXT.startsWith(QStringLiteral("../")) || TEXT.contains(QLatin1Char('/')) || TEXT == QStringLiteral("~")) {
            classified.kind = TokenKind::Path;
        } else {
            classified.kind = TokenKind::Argument;
        }
        result.append(classified);
    }
    result += findHereDocSpans(source, LEXICAL_TOKENS);
    std::stable_sort(result.begin(), result.end(), [](const SyntaxToken &left, const SyntaxToken &right) {
        return left.startOffset < right.startOffset;
    });
    return result;
}

QString tokenKindName(TokenKind kind)
{
    switch (kind) {
    case TokenKind::Comment: return QStringLiteral("Comment");
    case TokenKind::StringDelimiter: return QStringLiteral("String delimiter");
    case TokenKind::StringContent: return QStringLiteral("String content");
    case TokenKind::Variable: return QStringLiteral("Variable expansion");
    case TokenKind::SpecialParameter: return QStringLiteral("Special parameter");
    case TokenKind::EscapeSequence: return QStringLiteral("Escape sequence");
    case TokenKind::CommandSubstitutionStart: return QStringLiteral("Command substitution start");
    case TokenKind::BacktickCommandSubstitutionDelimiter: return QStringLiteral("Backtick substitution delimiter");
    case TokenKind::ArithmeticExpansionStart: return QStringLiteral("Arithmetic expansion start");
    case TokenKind::Operator: return QStringLiteral("Operator");
    case TokenKind::Redirection: return QStringLiteral("Redirection");
    case TokenKind::Number: return QStringLiteral("Number");
    case TokenKind::Word: return QStringLiteral("Word");
    case TokenKind::Command: return QStringLiteral("Command");
    case TokenKind::Builtin: return QStringLiteral("Shell builtin");
    case TokenKind::Keyword: return QStringLiteral("Shell keyword");
    case TokenKind::AssignmentName: return QStringLiteral("Assignment name");
    case TokenKind::Option: return QStringLiteral("Option");
    case TokenKind::Path: return QStringLiteral("Path");
    case TokenKind::Argument: return QStringLiteral("Argument / value");
    case TokenKind::HereDocContent: return QStringLiteral("Here-document content");
    }
    return QStringLiteral("Unknown");
}

QString printableTokenText(const QString &source, const SyntaxToken &token)
{
    QString TEXT = source.mid(token.startOffset, token.length);
    TEXT.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    TEXT.replace(QStringLiteral("\t"), QStringLiteral("\\t"));
    TEXT.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
    TEXT.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return TEXT;
}
}
