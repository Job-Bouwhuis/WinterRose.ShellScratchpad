#pragma once

#include "SyntaxToken.h"

#include <QSyntaxHighlighter>
#include <QVector>

class QPlainTextEdit;
class QTextDocument;

namespace ShellScratchpad::Syntax
{
class SyntaxHighlighter final : public QSyntaxHighlighter
{
public:
    explicit SyntaxHighlighter(QPlainTextEdit *editor);

protected:
    void highlightBlock(const QString &text) override;

private:
    QVector<SyntaxToken> tokens;
    bool rehighlighting = false;
};
}
