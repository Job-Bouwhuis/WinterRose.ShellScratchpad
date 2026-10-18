#include "syntax/SyntaxTokenizer.h"

#include <QString>
#include <QVector>

#include <iostream>

using ShellScratchpad::Syntax::SyntaxToken;
using ShellScratchpad::Syntax::SyntaxTokenizer;
using ShellScratchpad::Syntax::TokenKind;

namespace
{
int FAILURE_COUNT = 0;

void expectToken(const QString &source, TokenKind expectedKind, const QString &expectedText,
                 const QString &caseName)
{
    const QVector<SyntaxToken> TOKENS = SyntaxTokenizer::tokenize(source);
    for (const SyntaxToken &TOKEN : TOKENS) {
        if (TOKEN.kind == expectedKind && source.mid(TOKEN.startOffset, TOKEN.length) == expectedText)
            return;
    }

    std::cerr << "FAIL: " << caseName.toStdString() << " (missing token '"
              << expectedText.toStdString() << "')\n";
    ++FAILURE_COUNT;
}

void expectPosition(const QString &source, TokenKind expectedKind, const QString &expectedText,
                    int expectedLine, int expectedColumn, qsizetype expectedOffset,
                    const QString &caseName)
{
    const QVector<SyntaxToken> TOKENS = SyntaxTokenizer::tokenize(source);
    for (const SyntaxToken &TOKEN : TOKENS) {
        if (TOKEN.kind == expectedKind && source.mid(TOKEN.startOffset, TOKEN.length) == expectedText) {
            if (TOKEN.line != expectedLine || TOKEN.column != expectedColumn || TOKEN.startOffset != expectedOffset) {
                std::cerr << "FAIL: " << caseName.toStdString() << " (unexpected source position)\n";
                ++FAILURE_COUNT;
            }
            return;
        }
    }

    std::cerr << "FAIL: " << caseName.toStdString() << " (token not found for position check)\n";
    ++FAILURE_COUNT;
}

void expectNotToken(const QString &source, TokenKind unexpectedKind, const QString &unexpectedText,
                    const QString &caseName)
{
    const QVector<SyntaxToken> TOKENS = SyntaxTokenizer::tokenize(source);
    for (const SyntaxToken &TOKEN : TOKENS) {
        if (TOKEN.kind == unexpectedKind && source.mid(TOKEN.startOffset, TOKEN.length) == unexpectedText) {
            std::cerr << "FAIL: " << caseName.toStdString() << " (unexpected token '"
                      << unexpectedText.toStdString() << "')\n";
            ++FAILURE_COUNT;
            return;
        }
    }
}
}

int main()
{

    const QString COMMENT_CASE = QStringLiteral("# comment\necho ready");
    expectToken(COMMENT_CASE, TokenKind::Comment, QStringLiteral("# comment"), QStringLiteral("line comment"));
    expectToken(COMMENT_CASE, TokenKind::Builtin, QStringLiteral("echo"), QStringLiteral("builtin command"));
    expectPosition(COMMENT_CASE, TokenKind::Builtin, QStringLiteral("echo"), 2, 1, 10,
                   QStringLiteral("line, column and offset tracking"));

    const QString QUOTE_CASE = QStringLiteral("echo \"hello $HOME\" '$USER'");
    expectToken(QUOTE_CASE, TokenKind::StringDelimiter, QStringLiteral("\""), QStringLiteral("double quote delimiter"));
    expectToken(QUOTE_CASE, TokenKind::StringContent, QStringLiteral("hello "), QStringLiteral("double quote content"));
    expectToken(QUOTE_CASE, TokenKind::Variable, QStringLiteral("$HOME"), QStringLiteral("variable in double quotes"));
    expectToken(QUOTE_CASE, TokenKind::StringContent, QStringLiteral("$USER"), QStringLiteral("single quoted literal"));
    expectNotToken(QUOTE_CASE, TokenKind::Variable, QStringLiteral("$USER"), QStringLiteral("single quotes suppress expansions"));

    const QString HASH_CASE = QStringLiteral("echo foo#bar # actual comment");
    expectToken(HASH_CASE, TokenKind::Argument, QStringLiteral("foo#bar"), QStringLiteral("hash inside word"));
    expectToken(HASH_CASE, TokenKind::Comment, QStringLiteral("# actual comment"), QStringLiteral("comment after whitespace"));

    const QString OPERATOR_CASE = QStringLiteral("echo hello && printf '%s\\n' \"$USER\" 2>&1");
    expectToken(OPERATOR_CASE, TokenKind::Operator, QStringLiteral("&&"), QStringLiteral("logical operator"));
    expectToken(OPERATOR_CASE, TokenKind::Redirection, QStringLiteral("2>&1"), QStringLiteral("stderr redirection"));
    expectToken(OPERATOR_CASE, TokenKind::StringContent, QStringLiteral("%s\\n"), QStringLiteral("single quoted backslash remains literal"));
    const QString FD_ADJACENCY_CASE = QStringLiteral("echo foo2>&1");
    expectToken(FD_ADJACENCY_CASE, TokenKind::Argument, QStringLiteral("foo2"), QStringLiteral("fd digits embedded in a word"));
    expectToken(FD_ADJACENCY_CASE, TokenKind::Redirection, QStringLiteral(">&"), QStringLiteral("redirection following a word"));

    const QString EXPANSION_CASE = QStringLiteral("echo $(printf '%s' \"$HOME\") $((2 + 3))");
    expectToken(EXPANSION_CASE, TokenKind::CommandSubstitutionStart, QStringLiteral("$("), QStringLiteral("command substitution"));
    expectToken(EXPANSION_CASE, TokenKind::ArithmeticExpansionStart, QStringLiteral("$(("), QStringLiteral("arithmetic expansion"));

    const QString ESCAPE_CASE = QStringLiteral("echo \\#literal");
    expectToken(ESCAPE_CASE, TokenKind::EscapeSequence, QStringLiteral("\\#"), QStringLiteral("escaped hash"));
    expectNotToken(ESCAPE_CASE, TokenKind::Comment, QStringLiteral("#literal"), QStringLiteral("escaped hash is not comment"));

    const QString INCOMPLETE_CASE = QStringLiteral("echo \"unfinished $HOME");
    expectToken(INCOMPLETE_CASE, TokenKind::Variable, QStringLiteral("$HOME"), QStringLiteral("unfinished quote remains tokenizable"));

    const QString CONTEXT_CASE = QStringLiteral(
        "export BUILD_MODE=Release\n"
        "cmake -S . -B build\n"
        "echo ./setup.sh\n"
        "cmake --build build\n"
        "sudo dnf install qt6-qtbase-devel && echo \"Done\"");
    expectToken(CONTEXT_CASE, TokenKind::Builtin, QStringLiteral("export"), QStringLiteral("export builtin"));
    expectToken(CONTEXT_CASE, TokenKind::AssignmentName, QStringLiteral("BUILD_MODE"), QStringLiteral("assignment name"));
    expectToken(CONTEXT_CASE, TokenKind::Operator, QStringLiteral("="), QStringLiteral("assignment operator"));
    expectToken(CONTEXT_CASE, TokenKind::Argument, QStringLiteral("Release"), QStringLiteral("assignment value"));
    expectToken(CONTEXT_CASE, TokenKind::Command, QStringLiteral("cmake"), QStringLiteral("external command"));
    expectToken(CONTEXT_CASE, TokenKind::Option, QStringLiteral("-S"), QStringLiteral("command option"));
    expectToken(CONTEXT_CASE, TokenKind::Path, QStringLiteral("./setup.sh"), QStringLiteral("path argument"));
    expectToken(CONTEXT_CASE, TokenKind::Command, QStringLiteral("sudo"), QStringLiteral("first command in chain"));
    expectToken(CONTEXT_CASE, TokenKind::Command, QStringLiteral("dnf"), QStringLiteral("command invoked through sudo"));
    expectToken(CONTEXT_CASE, TokenKind::Builtin, QStringLiteral("echo"), QStringLiteral("command after logical operator"));

    const QString SUDO_OPTION_CASE = QStringLiteral("sudo -u root apt install package");
    expectToken(SUDO_OPTION_CASE, TokenKind::Option, QStringLiteral("-u"), QStringLiteral("sudo option"));
    expectToken(SUDO_OPTION_CASE, TokenKind::Argument, QStringLiteral("root"), QStringLiteral("sudo option value"));
    expectToken(SUDO_OPTION_CASE, TokenKind::Command, QStringLiteral("apt"), QStringLiteral("command after sudo option value"));

    const QString CONTROL_FLOW_CASE = QStringLiteral(
        "if [ -d \"$HOME/.cache\" ]; then\n"
        "  echo \"Cache directory exists\"\n"
        "fi\n"
        "for item in one two; do\n"
        "  printf '%s\\n' \"$item\"\n"
        "done");
    expectToken(CONTROL_FLOW_CASE, TokenKind::Keyword, QStringLiteral("if"), QStringLiteral("if keyword"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Builtin, QStringLiteral("["), QStringLiteral("test builtin"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Builtin, QStringLiteral("]"), QStringLiteral("test closing bracket"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Option, QStringLiteral("-d"), QStringLiteral("test option"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Keyword, QStringLiteral("then"), QStringLiteral("then keyword"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Builtin, QStringLiteral("echo"), QStringLiteral("command in conditional body"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Keyword, QStringLiteral("fi"), QStringLiteral("fi keyword"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Keyword, QStringLiteral("for"), QStringLiteral("for keyword"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Argument, QStringLiteral("item"), QStringLiteral("loop variable is not a command"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Keyword, QStringLiteral("do"), QStringLiteral("do keyword"));
    expectToken(CONTROL_FLOW_CASE, TokenKind::Builtin, QStringLiteral("printf"), QStringLiteral("command in loop body"));

    const QString REDIRECTION_CASE = QStringLiteral("echo ready > /tmp/output.txt");
    expectToken(REDIRECTION_CASE, TokenKind::Redirection, QStringLiteral(">"), QStringLiteral("output redirection"));
    expectToken(REDIRECTION_CASE, TokenKind::Path, QStringLiteral("/tmp/output.txt"), QStringLiteral("redirection target"));

    if (FAILURE_COUNT > 0) {
        std::cerr << FAILURE_COUNT << " tokenizer test(s) failed.\n";
        return 1;
    }

    std::cout << "All syntax tokenizer tests passed.\n";
    return 0;
}
