#include "SnippetCreateDialog.h"

#include "SnippetStore.h"
#include "editor/CodeEditor.h"
#include "syntax/SyntaxHighlighter.h"

#include <QDialogButtonBox>
#include <QComboBox>
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

    auto *autoChangeLabel = new QLabel(QStringLiteral("Auto-change sections — select text that should copy a confirmed highlight point"), this);
    rootLayout->addWidget(autoChangeLabel);
    autoChangeSource = new QComboBox(this);
    rootLayout->addWidget(autoChangeSource);
    autoChangesList = new QListWidget(this);
    autoChangesList->setMaximumHeight(90);
    rootLayout->addWidget(autoChangesList);
    auto *autoChangeActions = new QHBoxLayout();
    auto *addAutoChangeButton = new QPushButton(QStringLiteral("Mark Selected Text as Auto-change"), this);
    removeAutoChangeButton = new QPushButton(QStringLiteral("Remove Auto-change"), this);
    autoChangeActions->addWidget(addAutoChangeButton);
    autoChangeActions->addWidget(removeAutoChangeButton);
    autoChangeActions->addStretch(1);
    rootLayout->addLayout(autoChangeActions);

    auto *footer = new QHBoxLayout();
    statusLabel = new QLabel(this);
    footer->addWidget(statusLabel, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    footer->addWidget(buttons);
    rootLayout->addLayout(footer);

    connect(addPoint, &QPushButton::clicked, this, [this]() { addHighlightPoint(); });
    connect(removePointButton, &QPushButton::clicked, this, [this]() { removeHighlightPoint(); });
    connect(addAutoChangeButton, &QPushButton::clicked, this, [this]() { addAutoChangeSection(); });
    connect(removeAutoChangeButton, &QPushButton::clicked, this, [this]() { removeAutoChangeSection(); });
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
    result.autoChangeSections = autoChangeSections;
    return result;
}

void SnippetCreateDialog::refreshPointsList()
{
    pointsList->clear();
    const int PREVIOUS_SOURCE_OFFSET = autoChangeSource->currentData().toInt();
    autoChangeSource->clear();
    const QString CODE = codePreview->toPlainText();
    for (int index = 0; index < highlightPoints.size(); ++index) {
        const HighlightPoint &point = highlightPoints.at(index);
        const int LINE = CODE.left(point.offset).count(QLatin1Char('\n')) + 1;
        const QString TEXT = CODE.mid(point.offset, point.length).simplified();
        new QListWidgetItem(QStringLiteral("%1. Line %2 — %3")
            .arg(index + 1).arg(LINE).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT), pointsList);
        autoChangeSource->addItem(QStringLiteral("After confirming %1: %2")
            .arg(index + 1).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT),
            static_cast<int>(point.offset));
    }
    const int SOURCE_INDEX = autoChangeSource->findData(PREVIOUS_SOURCE_OFFSET);
    if (SOURCE_INDEX >= 0)
        autoChangeSource->setCurrentIndex(SOURCE_INDEX);
    removePointButton->setEnabled(!highlightPoints.isEmpty());
    autoChangeSource->setEnabled(!highlightPoints.isEmpty());
    refreshAutoChangesList();
}

void SnippetCreateDialog::refreshAutoChangesList()
{
    autoChangesList->clear();
    const QString CODE = codePreview->toPlainText();
    for (int index = 0; index < autoChangeSections.size(); ++index) {
        const AutoChangeSection &section = autoChangeSections.at(index);
        const QString TEXT = CODE.mid(section.offset, section.length).simplified();
        const auto source = std::find_if(highlightPoints.cbegin(), highlightPoints.cend(), [&section](const HighlightPoint &point) {
            return point.offset == section.sourcePointOffset;
        });
        const int SOURCE_INDEX = source == highlightPoints.cend() ? -1 : static_cast<int>(source - highlightPoints.cbegin()) + 1;
        new QListWidgetItem(QStringLiteral("%1. %2 ← highlight point %3")
            .arg(index + 1).arg(TEXT).arg(SOURCE_INDEX), autoChangesList);
    }
    removeAutoChangeButton->setEnabled(!autoChangeSections.isEmpty());
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
    const qsizetype REMOVED_OFFSET = highlightPoints.at(INDEX).offset;
    highlightPoints.removeAt(INDEX);
    autoChangeSections.erase(std::remove_if(autoChangeSections.begin(), autoChangeSections.end(),
        [REMOVED_OFFSET](const AutoChangeSection &section) { return section.sourcePointOffset == REMOVED_OFFSET; }),
        autoChangeSections.end());
    refreshPointsList();
}

void SnippetCreateDialog::addAutoChangeSection()
{
    const QTextCursor SELECTION = codePreview->textCursor();
    if (!SELECTION.hasSelection() || autoChangeSource->currentIndex() < 0) {
        statusLabel->setText(QStringLiteral("Select text and choose a highlight point first."));
        return;
    }
    const AutoChangeSection section{SELECTION.selectionStart(),
        SELECTION.selectionEnd() - SELECTION.selectionStart(), autoChangeSource->currentData().toInt()};
    const auto OVERLAPS = [&section](qsizetype offset, qsizetype length) {
        return section.offset < offset + length && offset < section.offset + section.length;
    };
    for (const HighlightPoint &point : highlightPoints) {
        if (OVERLAPS(point.offset, point.length)) {
            statusLabel->setText(QStringLiteral("Auto-change sections must be separate from highlight points."));
            return;
        }
    }
    for (const AutoChangeSection &existing : autoChangeSections) {
        if (OVERLAPS(existing.offset, existing.length)) {
            statusLabel->setText(QStringLiteral("Auto-change sections cannot overlap."));
            return;
        }
    }
    const bool EXISTS = std::any_of(autoChangeSections.cbegin(), autoChangeSections.cend(), [&section](const AutoChangeSection &item) {
        return item.offset == section.offset && item.length == section.length;
    });
    if (!EXISTS) {
        autoChangeSections.append(section);
        std::stable_sort(autoChangeSections.begin(), autoChangeSections.end(),
            [](const AutoChangeSection &left, const AutoChangeSection &right) { return left.offset < right.offset; });
    }
    refreshAutoChangesList();
    statusLabel->setText(QStringLiteral("Auto-change section added."));
}

void SnippetCreateDialog::removeAutoChangeSection()
{
    const int INDEX = autoChangesList->currentRow();
    if (INDEX < 0 || INDEX >= autoChangeSections.size())
        return;
    autoChangeSections.removeAt(INDEX);
    refreshAutoChangesList();
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
