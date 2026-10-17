#pragma once

#include "SyntaxToken.h"

#include <QDialog>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QTableWidget;

namespace ShellScratchpad::Syntax
{
class SyntaxDebugDialog final : public QDialog
{
public:
    explicit SyntaxDebugDialog(QPlainTextEdit *sourceEditor, QWidget *parent = nullptr);

private:
    QPlainTextEdit *sourceEditor = nullptr;
    QPlainTextEdit *testEditor = nullptr;
    QTableWidget *tokenTable = nullptr;
    QLabel *summaryLabel = nullptr;
    QVector<SyntaxToken> latestTokens;

    void analyzeCurrentText();
    void selectTokenAtRow(int row);
};
}
