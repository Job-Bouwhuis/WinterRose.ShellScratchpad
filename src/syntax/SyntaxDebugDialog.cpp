#include "SyntaxDebugDialog.h"

#include "SyntaxTokenizer.h"

#include <QAbstractItemView>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QVBoxLayout>

namespace ShellScratchpad::Syntax
{
SyntaxDebugDialog::SyntaxDebugDialog(QPlainTextEdit *sourceEditor, QWidget *parent)
    : QDialog(parent), sourceEditor(sourceEditor)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Syntax Debugger"));
    resize(1000, 680);
    setMinimumSize(720, 480);

    auto *LAYOUT = new QVBoxLayout(this);
    auto *DESCRIPTION = new QLabel(
        QStringLiteral("Inspect the tokenizer output. Offsets are zero-based; line and column numbers are one-based."),
        this);
    DESCRIPTION->setWordWrap(true);
    LAYOUT->addWidget(DESCRIPTION);

    auto *BUTTON_ROW = new QHBoxLayout();
    auto *LOAD_BUTTON = new QPushButton(QStringLiteral("Load current script"), this);
    BUTTON_ROW->addWidget(LOAD_BUTTON);
    BUTTON_ROW->addStretch(1);
    LAYOUT->addLayout(BUTTON_ROW);

    auto *SPLITTER = new QSplitter(Qt::Vertical, this);
    SPLITTER->setChildrenCollapsible(false);
    testEditor = new QPlainTextEdit(SPLITTER);
    testEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    testEditor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tokenTable = new QTableWidget(0, 6, SPLITTER);
    tokenTable->setHorizontalHeaderLabels({
        QStringLiteral("Line"), QStringLiteral("Column"), QStringLiteral("Offset"),
        QStringLiteral("Length"), QStringLiteral("Token type"), QStringLiteral("Text")
    });
    tokenTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tokenTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tokenTable->setSelectionMode(QAbstractItemView::SingleSelection);
    tokenTable->verticalHeader()->setVisible(false);
    for (int COLUMN = 0; COLUMN < 5; ++COLUMN)
        tokenTable->horizontalHeader()->setSectionResizeMode(COLUMN, QHeaderView::ResizeToContents);
    tokenTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    SPLITTER->addWidget(testEditor);
    SPLITTER->addWidget(tokenTable);
    SPLITTER->setStretchFactor(0, 3);
    SPLITTER->setStretchFactor(1, 2);
    LAYOUT->addWidget(SPLITTER, 1);

    summaryLabel = new QLabel(this);
    LAYOUT->addWidget(summaryLabel);

    connect(LOAD_BUTTON, &QPushButton::clicked, this, [this]() {
        if (this->sourceEditor)
            testEditor->setPlainText(this->sourceEditor->toPlainText());
    });
    connect(testEditor, &QPlainTextEdit::textChanged, this, [this]() { analyzeCurrentText(); });
    connect(tokenTable, &QTableWidget::cellClicked, this,
            [this](int row, int) { selectTokenAtRow(row); });
    if (sourceEditor)
        testEditor->setPlainText(sourceEditor->toPlainText());
    analyzeCurrentText();
}

void SyntaxDebugDialog::analyzeCurrentText()
{
    const QString SOURCE = testEditor->toPlainText();
    latestTokens = SyntaxTokenizer::tokenize(SOURCE);
    tokenTable->setRowCount(static_cast<int>(latestTokens.size()));
    for (qsizetype INDEX = 0; INDEX < latestTokens.size(); ++INDEX) {
        const SyntaxToken &TOKEN = latestTokens.at(INDEX);
        const QStringList VALUES = {
            QString::number(TOKEN.line), QString::number(TOKEN.column),
            QString::number(TOKEN.startOffset), QString::number(TOKEN.length),
            tokenKindName(TOKEN.kind), printableTokenText(SOURCE, TOKEN)
        };
        for (qsizetype COLUMN = 0; COLUMN < VALUES.size(); ++COLUMN) {
            auto *ITEM = new QTableWidgetItem(VALUES.at(COLUMN));
            ITEM->setToolTip(VALUES.at(COLUMN));
            tokenTable->setItem(static_cast<int>(INDEX), static_cast<int>(COLUMN), ITEM);
        }
    }
    summaryLabel->setText(QStringLiteral("%1 token(s). Analysis is synchronous.")
                              .arg(latestTokens.size()));
}

void SyntaxDebugDialog::selectTokenAtRow(int row)
{
    if (row < 0 || row >= latestTokens.size())
        return;
    const SyntaxToken &TOKEN = latestTokens.at(row);
    QTextCursor CURSOR = testEditor->textCursor();
    CURSOR.setPosition(static_cast<int>(TOKEN.startOffset));
    CURSOR.setPosition(static_cast<int>(TOKEN.startOffset + TOKEN.length), QTextCursor::KeepAnchor);
    testEditor->setTextCursor(CURSOR);
    testEditor->setFocus();
}
}
