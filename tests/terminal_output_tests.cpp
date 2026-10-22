#include "output/TerminalOutput.h"

#include <QApplication>
#include <QTextDocument>

#include <cstdlib>
#include <iostream>

namespace
{
void expectText(const char *name, const QString &actual, const QString &expected)
{
    if (actual == expected)
        return;
    std::cerr << name << " failed\nExpected: " << expected.toStdString()
              << "\nActual:   " << actual.toStdString() << '\n';
    std::exit(EXIT_FAILURE);
}

QString run(const QString &initial, const QString &mutations, int screenRows = 0)
{
    QTextDocument document;
    ShellScratchpad::Output::TerminalOutput output(&document);
    output.setScreenRows(screenRows);
    output.append(initial);
    output.append(mutations);
    return output.plainText();
}

void testInsertLinesAndImmediateWrite()
{
    expectText("CSI L multiple rows plus write",
        run(QStringLiteral("ALPHA\nBRAVO\nCHARLIE\nDELTA"),
            QStringLiteral("\x1b[2;1H\x1b[2LINSERTED-A")),
        QStringLiteral("ALPHA\nINSERTED-A\n\nBRAVO"));
}

void testDeleteLinesAndImmediateWrite()
{
    expectText("CSI M multiple rows plus write",
        run(QStringLiteral("ALPHA\nBRAVO\nCHARLIE\nDELTA"),
            QStringLiteral("\x1b[2;1H\x1b[2MREPLACED")),
        QStringLiteral("ALPHA\nREPLACED\n\n"));
}

void testScrollUpAndImmediateWrite()
{
    expectText("CSI S scrolling region plus write",
        run(QStringLiteral("A\nBRAVO\nCHARLIE\nDELTA\nECHO"),
            QStringLiteral("\x1b[2;5r\x1b[4;1H\x1b[2S=== Test Complete ===")),
        QStringLiteral("A\nDELTA\nECHO\n=== Test Complete ===\n"));
}

void testScrollDownAndImmediateWrite()
{
    expectText("CSI T scrolling region plus write",
        run(QStringLiteral("A\nBRAVO\nCHARLIE\nDELTA\nECHO"),
            QStringLiteral("\x1b[2;5r\x1b[3;1H\x1b[2TINSERTED")),
        QStringLiteral("A\n\nINSERTED\nBRAVO\nCHARLIE"));
}

void testFullScreenScrollUpOnly()
{
    expectText("CSI S full-screen scroll",
        run(QStringLiteral("AAAA\nBBBB\nCCCC\nDDDD\n"), QStringLiteral("\x1b[2S"), 8),
        QStringLiteral("CCCC\nDDDD\n\n\n\n\n\n"));
}

void testFullScreenScrollDownOnly()
{
    expectText("CSI T full-screen scroll",
        run(QStringLiteral("AAAA\nBBBB\nCCCC\nDDDD\n"), QStringLiteral("\x1b[T"), 8),
        QStringLiteral("\nAAAA\nBBBB\nCCCC\nDDDD\n\n\n"));
}

void testFullScreenScrollUpThenDown()
{
    QTextDocument document;
    ShellScratchpad::Output::TerminalOutput output(&document);
    output.setScreenRows(8);
    output.append(QStringLiteral("\x1b[2J\x1b[H"));
    output.append(QStringLiteral("AAAA\nBBBB\nCCCC\nDDDD\n"));
    output.append(QStringLiteral("\x1b[2S\x1b[T"));
    expectText("CSI S then CSI T full-screen scroll", output.plainText(),
        QStringLiteral("\nCCCC\nDDDD\n\n\n\n\n"));
}

void testScrollbackHistorySurvivesVisibleScreenScroll()
{
    const QString initial = QStringLiteral("H0\nH1\nH2\nH3\nH4\nH5\nH6\nH7\nH8\n");
    expectText("CSI S leaves scrollback history unchanged",
        run(initial, QStringLiteral("\x1b[2S"), 4),
        QStringLiteral("H0\nH1\nH2\nH3\nH4\nH5\nH8\n\n\n"));
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testInsertLinesAndImmediateWrite();
    testDeleteLinesAndImmediateWrite();
    testScrollUpAndImmediateWrite();
    testScrollDownAndImmediateWrite();
    testFullScreenScrollUpOnly();
    testFullScreenScrollDownOnly();
    testFullScreenScrollUpThenDown();
    testScrollbackHistorySurvivesVisibleScreenScroll();
    return EXIT_SUCCESS;
}
