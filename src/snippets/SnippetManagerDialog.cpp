#include "SnippetManagerDialog.h"

#include "SnippetStore.h"
#include "editor/CodeEditor.h"
#include "syntax/SyntaxHighlighter.h"

#include <QFormLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QDesktopServices>
#include <QDir>
#include <QTimer>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace ShellScratchpad::Snippets
{
SnippetManagerDialog::SnippetManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Snippet Management"));
    resize(900, 600);
    auto *rootLayout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(this);
    snippetList = new QListWidget(splitter);
    auto *details = new QWidget(splitter);
    auto *detailsLayout = new QVBoxLayout(details);
    auto *form = new QFormLayout();
    nameEdit = new QLineEdit(details);
    descriptionEdit = new QLineEdit(details);
    form->addRow(QStringLiteral("Name"), nameEdit);
    form->addRow(QStringLiteral("Description"), descriptionEdit);
    detailsLayout->addLayout(form);
    codePreview = new ShellScratchpad::CodeEditor(details);
    codePreview->setReadOnly(true);
    codePreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    codePreview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    codePreview->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #25282d; color: #e5e7eb; }"));
    new Syntax::SyntaxHighlighter(codePreview);
    detailsLayout->addWidget(codePreview, 1);
    auto *pointsHeading = new QLabel(QStringLiteral("Highlight points"), details);
    detailsLayout->addWidget(pointsHeading);
    pointsList = new QListWidget(details);
    pointsList->setMaximumHeight(100);
    detailsLayout->addWidget(pointsList);
    auto *pointButtons = new QHBoxLayout();
    addPointButton = new QPushButton(QStringLiteral("Mark Selected Text"), details);
    removePointButton = new QPushButton(QStringLiteral("Remove Point"), details);
    pointButtons->addWidget(addPointButton);
    pointButtons->addWidget(removePointButton);
    pointButtons->addStretch(1);
    detailsLayout->addLayout(pointButtons);
    splitter->addWidget(snippetList);
    splitter->addWidget(details);
    splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(splitter, 1);

    auto *footer = new QHBoxLayout();
    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    reloadButton = new QPushButton(QStringLiteral("Reload from Disk"), this);
    deleteButton = new QPushButton(QStringLiteral("Delete Snippet"), this);
    auto *openFolderButton = new QPushButton(QStringLiteral("Open Snippets Folder"), this);
    auto *closeButton = new QPushButton(QStringLiteral("Close"), this);
    footer->addStretch(1);
    footer->addWidget(deleteButton);
    footer->addWidget(reloadButton);
    footer->addWidget(openFolderButton);
    footer->addWidget(closeButton);
    rootLayout->addLayout(footer);
    rootLayout->addWidget(statusLabel);

    saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);
    saveTimer->setInterval(350);
    connect(saveTimer, &QTimer::timeout, this, [this]() { saveCurrentMetadata(); });
    connect(snippetList, &QListWidget::currentRowChanged, this, [this]() { showCurrentSnippet(); });
    connect(nameEdit, &QLineEdit::textEdited, this, [this]() { saveTimer->start(); });
    connect(descriptionEdit, &QLineEdit::textEdited, this, [this]() { saveTimer->start(); });
    connect(reloadButton, &QPushButton::clicked, this, [this]() { reloadFromDisk(); });
    connect(deleteButton, &QPushButton::clicked, this, [this]() { deleteCurrentSnippet(); });
    connect(openFolderButton, &QPushButton::clicked, this, []() {
        const QString DIRECTORY = SnippetStore::directoryPath();
        if (QDir().mkpath(DIRECTORY))
            QDesktopServices::openUrl(QUrl::fromLocalFile(DIRECTORY));
    });
    connect(addPointButton, &QPushButton::clicked, this, [this]() { addHighlightPoint(); });
    connect(removePointButton, &QPushButton::clicked, this, [this]() { removeHighlightPoint(); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    reloadFromDisk();
}

void SnippetManagerDialog::reloadFromDisk()
{
    saveTimer->stop();
    loading = true;
    selectedId.clear();
    snippetList->clear();
    const QVector<Snippet> snippets = SnippetStore::loadAll();
    for (const Snippet &snippet : snippets) {
        auto *item = new QListWidgetItem(snippet.name, snippetList);
        item->setData(Qt::UserRole, snippet.id);
    }
    if (snippetList->count() > 0)
        snippetList->setCurrentRow(0);
    else {
        nameEdit->clear();
        descriptionEdit->clear();
        codePreview->clear();
        pointsList->clear();
        nameEdit->setEnabled(false);
        descriptionEdit->setEnabled(false);
        addPointButton->setEnabled(false);
        removePointButton->setEnabled(false);
        deleteButton->setEnabled(false);
        statusLabel->setText(QStringLiteral("No snippets saved yet."));
    }
    loading = false;
    if (snippetList->count() > 0)
        showCurrentSnippet();
}

void SnippetManagerDialog::showCurrentSnippet()
{
    if (loading)
        return;
    saveTimer->stop();
    const QString PREVIOUS_ID = selectedId;
    if (!PREVIOUS_ID.isEmpty())
        saveCurrentMetadata();
    const QListWidgetItem *item = snippetList->currentItem();
    if (!item)
        return;
    selectedId = item->data(Qt::UserRole).toString();
    const QVector<Snippet> snippets = SnippetStore::loadAll();
    for (const Snippet &snippet : snippets) {
        if (snippet.id != selectedId)
            continue;
        loading = true;
        nameEdit->setEnabled(true);
        descriptionEdit->setEnabled(true);
        nameEdit->setText(snippet.name);
        descriptionEdit->setText(snippet.description);
        codePreview->setPlainText(snippet.code);
        refreshPointsList(snippet);
        addPointButton->setEnabled(true);
        removePointButton->setEnabled(true);
        deleteButton->setEnabled(true);
        loading = false;
        statusLabel->setText(snippet.filePath);
        return;
    }
}

void SnippetManagerDialog::saveCurrentMetadata()
{
    if (loading || selectedId.isEmpty())
        return;
    QVector<Snippet> snippets = SnippetStore::loadAll();
    for (Snippet &snippet : snippets) {
        if (snippet.id != selectedId)
            continue;
        snippet.name = nameEdit->text();
        snippet.description = descriptionEdit->text();
        QString error;
        if (!SnippetStore::save(&snippet, &error)) {
            statusLabel->setText(error);
            return;
        }
        loading = true;
        for (int row = 0; row < snippetList->count(); ++row) {
            QListWidgetItem *item = snippetList->item(row);
            if (item->data(Qt::UserRole).toString() == selectedId) {
                item->setText(snippet.name);
                break;
            }
        }
        loading = false;
        statusLabel->setText(QStringLiteral("Saved: %1").arg(snippet.filePath));
        return;
    }
}

void SnippetManagerDialog::refreshPointsList(const Snippet &snippet)
{
    pointsList->clear();
    for (int index = 0; index < snippet.highlightPoints.size(); ++index) {
        const HighlightPoint &point = snippet.highlightPoints.at(index);
        const int LINE = snippet.code.left(point.offset).count(QLatin1Char('\n')) + 1;
        const QString TEXT = snippet.code.mid(point.offset, point.length).simplified();
        auto *item = new QListWidgetItem(QStringLiteral("%1. Line %2 — %3")
            .arg(index + 1).arg(LINE).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT), pointsList);
        item->setData(Qt::UserRole, static_cast<int>(point.offset));
        item->setData(Qt::UserRole + 1, static_cast<int>(point.length));
    }
}

void SnippetManagerDialog::addHighlightPoint()
{
    if (selectedId.isEmpty())
        return;
    const QTextCursor SELECTION = codePreview->textCursor();
    if (!SELECTION.hasSelection()) {
        statusLabel->setText(QStringLiteral("Select text in the preview first."));
        return;
    }
    saveTimer->stop();
    saveCurrentMetadata();
    const int OFFSET = static_cast<int>(SELECTION.selectionStart());
    const int LENGTH = static_cast<int>(SELECTION.selectionEnd() - SELECTION.selectionStart());
    QVector<Snippet> snippets = SnippetStore::loadAll();
    for (Snippet &snippet : snippets) {
        if (snippet.id != selectedId)
            continue;
        const bool ALREADY_MARKED = std::any_of(snippet.highlightPoints.cbegin(), snippet.highlightPoints.cend(),
            [OFFSET, LENGTH](const HighlightPoint &point) {
                return point.offset == OFFSET && point.length == LENGTH;
            });
        if (!ALREADY_MARKED)
            snippet.highlightPoints.append(HighlightPoint{OFFSET, LENGTH});
        std::stable_sort(snippet.highlightPoints.begin(), snippet.highlightPoints.end(),
            [](const HighlightPoint &left, const HighlightPoint &right) { return left.offset < right.offset; });
        QString error;
        if (!SnippetStore::save(&snippet, &error)) {
            statusLabel->setText(error);
            return;
        }
        refreshPointsList(snippet);
        statusLabel->setText(QStringLiteral("Highlight point saved."));
        return;
    }
}

void SnippetManagerDialog::removeHighlightPoint()
{
    if (selectedId.isEmpty() || !pointsList->currentItem())
        return;
    saveTimer->stop();
    saveCurrentMetadata();
    const int OFFSET = pointsList->currentItem()->data(Qt::UserRole).toInt();
    const int LENGTH = pointsList->currentItem()->data(Qt::UserRole + 1).toInt();
    QVector<Snippet> snippets = SnippetStore::loadAll();
    for (Snippet &snippet : snippets) {
        if (snippet.id != selectedId)
            continue;
        for (qsizetype index = 0; index < snippet.highlightPoints.size(); ++index) {
            const HighlightPoint &point = snippet.highlightPoints.at(index);
            if (point.offset == OFFSET && point.length == LENGTH) {
                snippet.highlightPoints.removeAt(index);
                break;
            }
        }
        QString error;
        if (!SnippetStore::save(&snippet, &error)) {
            statusLabel->setText(error);
            return;
        }
        refreshPointsList(snippet);
        statusLabel->setText(QStringLiteral("Highlight point removed."));
        return;
    }
}

void SnippetManagerDialog::deleteCurrentSnippet()
{
    if (selectedId.isEmpty())
        return;
    Snippet selected;
    bool found = false;
    for (const Snippet &snippet : SnippetStore::loadAll()) {
        if (snippet.id == selectedId) {
            selected = snippet;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    const QMessageBox::StandardButton ANSWER = QMessageBox::question(
        this, QStringLiteral("Delete Snippet"),
        QStringLiteral("Delete the snippet \"%1\"? This cannot be undone.").arg(selected.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ANSWER != QMessageBox::Yes)
        return;

    saveTimer->stop();
    QString error;
    if (!SnippetStore::remove(selectedId, &error)) {
        statusLabel->setText(error);
        return;
    }
    const int PREVIOUS_ROW = snippetList->currentRow();
    loading = true;
    selectedId.clear();
    delete snippetList->takeItem(PREVIOUS_ROW);
    if (snippetList->count() > 0)
        snippetList->setCurrentRow(qMin(PREVIOUS_ROW, snippetList->count() - 1));
    loading = false;
    if (snippetList->count() > 0) {
        showCurrentSnippet();
    } else {
        nameEdit->clear();
        descriptionEdit->clear();
        codePreview->clear();
        pointsList->clear();
        nameEdit->setEnabled(false);
        descriptionEdit->setEnabled(false);
        addPointButton->setEnabled(false);
        removePointButton->setEnabled(false);
        deleteButton->setEnabled(false);
    }
    statusLabel->setText(QStringLiteral("Deleted snippet: %1").arg(selected.name));
}
}
