#include "SnippetCreateDialog.h"

#include "SnippetStore.h"
#include "editor/CodeEditor.h"
#include "syntax/SyntaxHighlighter.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ShellScratchpad::Snippets
{
SnippetCreateDialog::SnippetCreateDialog(const QString &initialCode, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Create Snippet"));
    resize(850, 620);
    setMinimumSize(680, 500);

    auto *rootLayout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    nameEdit = new QLineEdit(this);
    descriptionEdit = new QLineEdit(this);
    form->addRow(QStringLiteral("Name"), nameEdit);
    form->addRow(QStringLiteral("Description"), descriptionEdit);
    rootLayout->addLayout(form);

    auto *codeLabel = new QLabel(QStringLiteral("Snippet Bash source"), this);
    rootLayout->addWidget(codeLabel);
    codePreview = new ShellScratchpad::CodeEditor(this);
    codePreview->setReadOnly(true);
    codePreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    codePreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    codePreview->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #25282d; color: #e5e7eb; }"));
    codePreview->setPlainText(initialCode);
    new Syntax::SyntaxHighlighter(codePreview);
    rootLayout->addWidget(codePreview, 1);

    auto *pointsLabel = new QLabel(QStringLiteral("Highlight points — select text in the preview and mark it"), this);
    rootLayout->addWidget(pointsLabel);
    pointsList = new QListWidget(this);
    pointsList->setMaximumHeight(100);
    rootLayout->addWidget(pointsList);
    auto *pointActions = new QHBoxLayout();
    auto *addPoint = new QPushButton(QStringLiteral("Mark Selected Text"), this);
    removePointButton = new QPushButton(QStringLiteral("Remove Point"), this);
    pointActions->addWidget(addPoint);
    pointActions->addWidget(removePointButton);
    pointActions->addStretch(1);
    rootLayout->addLayout(pointActions);

    auto *footer = new QHBoxLayout();
    statusLabel = new QLabel(this);
    footer->addWidget(statusLabel, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    footer->addWidget(buttons);
    rootLayout->addLayout(footer);

    connect(addPoint, &QPushButton::clicked, this, [this]() { addHighlightPoint(); });
    connect(removePointButton, &QPushButton::clicked, this, [this]() { removeHighlightPoint(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() { saveSnippet(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refreshPointsList();
}

Snippet SnippetCreateDialog::snippet() const
{
    Snippet result;
    result.name = nameEdit->text().trimmed();
    result.description = descriptionEdit->text().trimmed();
    result.code = codePreview->toPlainText();
    result.highlightPoints = highlightPoints;
    return result;
}

void SnippetCreateDialog::refreshPointsList()
{
    pointsList->clear();
    const QString CODE = codePreview->toPlainText();
    for (int index = 0; index < highlightPoints.size(); ++index) {
        const HighlightPoint &point = highlightPoints.at(index);
        const int LINE = CODE.left(point.offset).count(QLatin1Char('\n')) + 1;
        const QString TEXT = CODE.mid(point.offset, point.length).simplified();
        new QListWidgetItem(QStringLiteral("%1. Line %2 — %3")
            .arg(index + 1).arg(LINE).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT), pointsList);
    }
    removePointButton->setEnabled(!highlightPoints.isEmpty());
}

void SnippetCreateDialog::addHighlightPoint()
{
    const QTextCursor SELECTION = codePreview->textCursor();
    if (!SELECTION.hasSelection()) {
        statusLabel->setText(QStringLiteral("Select text in the preview first."));
        return;
    }
    const HighlightPoint POINT{SELECTION.selectionStart(),
                               SELECTION.selectionEnd() - SELECTION.selectionStart()};
    const bool EXISTS = std::any_of(highlightPoints.cbegin(), highlightPoints.cend(), [&POINT](const HighlightPoint &point) {
        return point.offset == POINT.offset && point.length == POINT.length;
    });
    if (!EXISTS) {
        highlightPoints.append(POINT);
        std::stable_sort(highlightPoints.begin(), highlightPoints.end(),
            [](const HighlightPoint &left, const HighlightPoint &right) { return left.offset < right.offset; });
    }
    refreshPointsList();
    statusLabel->setText(QStringLiteral("Highlight point added."));
}

void SnippetCreateDialog::removeHighlightPoint()
{
    const int INDEX = pointsList->currentRow();
    if (INDEX < 0 || INDEX >= highlightPoints.size())
        return;
    highlightPoints.removeAt(INDEX);
    refreshPointsList();
}

void SnippetCreateDialog::saveSnippet()
{
    Snippet value = snippet();
    QString error;
    if (!SnippetStore::validateSyntax(value.code, &error)) {
        statusLabel->setText(error);
        return;
    }
    if (!SnippetStore::save(&value, &error)) {
        statusLabel->setText(error);
        return;
    }
    accept();
}
}
