#pragma once

#include "SyntaxToken.h"

namespace ShellScratchpad::Syntax
{
class SyntaxTokenizer final
{
public:
    static QVector<SyntaxToken> tokenize(const QString &source);
};
}
