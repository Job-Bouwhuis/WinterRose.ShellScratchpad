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
#include <QTabWidget>
#include <QComboBox>
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
    snippetTabs = new QTabWidget(splitter);
    snippetList = new QListWidget(snippetTabs);
    builtInList = new QListWidget(snippetTabs);
    snippetTabs->addTab(snippetList, QStringLiteral("My Snippets"));
    snippetTabs->addTab(builtInList, QStringLiteral("Built-in"));
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
    auto *autoChangeButtons = new QHBoxLayout();
    auto *pointButtons = new QHBoxLayout();
    addPointButton = new QPushButton(QStringLiteral("Mark Selected Text"), details);
    removePointButton = new QPushButton(QStringLiteral("Remove Point"), details);
    pointButtons->addWidget(addPointButton);
    pointButtons->addWidget(removePointButton);
    pointButtons->addStretch(1);
    detailsLayout->addLayout(pointButtons);
    detailsLayout->addWidget(new QLabel(QStringLiteral("Auto-change sections"), details));
    autoChangeSource = new QComboBox(details);
    detailsLayout->addWidget(autoChangeSource);
    autoChangesList = new QListWidget(details);
    autoChangesList->setMaximumHeight(80);
    detailsLayout->addWidget(autoChangesList);
    addAutoChangeButton = new QPushButton(QStringLiteral("Mark Selection as Auto-change"), details);
    removeAutoChangeButton = new QPushButton(QStringLiteral("Remove Auto-change"), details);
    autoChangeButtons->addWidget(addAutoChangeButton);
    autoChangeButtons->addWidget(removeAutoChangeButton);
    autoChangeButtons->addStretch(1);
    detailsLayout->addLayout(autoChangeButtons);
    splitter->addWidget(snippetTabs);
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
    connect(builtInList, &QListWidget::currentRowChanged, this, [this]() { showCurrentSnippet(); });
    connect(snippetTabs, &QTabWidget::currentChanged, this, [this]() { showCurrentSnippet(); });
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
    connect(addAutoChangeButton, &QPushButton::clicked, this, [this]() { addAutoChangeSection(); });
    connect(removeAutoChangeButton, &QPushButton::clicked, this, [this]() { removeAutoChangeSection(); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    reloadFromDisk();
}

void SnippetManagerDialog::reloadFromDisk()
{
    saveTimer->stop();
    loading = true;
    selectedId.clear();
    snippetList->clear();
    builtInList->clear();
    const QVector<Snippet> snippets = SnippetStore::loadAll();
    for (const Snippet &snippet : snippets) {
        auto *item = new QListWidgetItem(snippet.name, snippetList);
        item->setData(Qt::UserRole, snippet.id);
    }
    for (const Snippet &snippet : SnippetStore::builtInSnippets()) {
        auto *item = new QListWidgetItem(snippet.name, builtInList);
        item->setData(Qt::UserRole, snippet.id);
    }
    if (builtInList->count() > 0)
        builtInList->setCurrentRow(0);
    snippetTabs->setCurrentIndex(0);
    if (snippetList->count() > 0)
        snippetList->setCurrentRow(0);
    else {
        nameEdit->clear();
        descriptionEdit->clear();
        codePreview->clear();
        pointsList->clear();
        autoChangesList->clear();
        autoChangeSource->clear();
        nameEdit->setEnabled(false);
        descriptionEdit->setEnabled(false);
        addPointButton->setEnabled(false);
        removePointButton->setEnabled(false);
        autoChangeSource->setEnabled(false);
        addAutoChangeButton->setEnabled(false);
        removeAutoChangeButton->setEnabled(false);
        deleteButton->setEnabled(false);
        statusLabel->setText(QStringLiteral("No snippets saved yet."));
    }
    loading = false;
    if (currentSnippetList()->count() > 0)
        showCurrentSnippet();
}

QListWidget *SnippetManagerDialog::currentSnippetList() const
{
    return snippetTabs->currentIndex() == 1 ? builtInList : snippetList;
}

void SnippetManagerDialog::showCurrentSnippet()
{
    if (loading)
        return;
    saveTimer->stop();
    const QString PREVIOUS_ID = selectedId;
    if (!PREVIOUS_ID.isEmpty())
        saveCurrentMetadata();
    const QListWidgetItem *item = currentSnippetList()->currentItem();
    if (!item)
        return;
    selectedId = item->data(Qt::UserRole).toString();
    QVector<Snippet> snippets = SnippetStore::loadAll();
    const bool BUILT_IN = snippetTabs->currentIndex() == 1;
    if (BUILT_IN)
        snippets = SnippetStore::builtInSnippets();
    for (const Snippet &snippet : snippets) {
        if (snippet.id != selectedId)
            continue;
        loading = true;
        nameEdit->setEnabled(!BUILT_IN);
        descriptionEdit->setEnabled(!BUILT_IN);
        nameEdit->setText(snippet.name);
        descriptionEdit->setText(snippet.description);
        codePreview->setPlainText(snippet.code);
        refreshPointsList(snippet);
        refreshAutoChangesList(snippet);
        refreshAutoChangesList(snippet);
        addPointButton->setEnabled(!BUILT_IN);
        removePointButton->setEnabled(!BUILT_IN);
        autoChangeSource->setEnabled(!BUILT_IN && !snippet.highlightPoints.isEmpty());
        addAutoChangeButton->setEnabled(!BUILT_IN && !snippet.highlightPoints.isEmpty());
        removeAutoChangeButton->setEnabled(!BUILT_IN && !snippet.autoChangeSections.isEmpty());
        deleteButton->setEnabled(!BUILT_IN);
        loading = false;
        statusLabel->setText(BUILT_IN ? QStringLiteral("Built-in snippet · read-only") : snippet.filePath);
        return;
    }
}

void SnippetManagerDialog::saveCurrentMetadata()
{
    if (loading || selectedId.isEmpty() || snippetTabs->currentIndex() == 1)
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
    const QVariant PREVIOUS_SOURCE = autoChangeSource->currentData();
    autoChangeSource->clear();
    for (int index = 0; index < snippet.highlightPoints.size(); ++index) {
        const HighlightPoint &point = snippet.highlightPoints.at(index);
        const int LINE = snippet.code.left(point.offset).count(QLatin1Char('\n')) + 1;
        const QString TEXT = snippet.code.mid(point.offset, point.length).simplified();
        auto *item = new QListWidgetItem(QStringLiteral("%1. Line %2 — %3")
            .arg(index + 1).arg(LINE).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT), pointsList);
        item->setData(Qt::UserRole, static_cast<int>(point.offset));
        item->setData(Qt::UserRole + 1, static_cast<int>(point.length));
        autoChangeSource->addItem(QStringLiteral("After highlight point %1: %2")
            .arg(index + 1).arg(TEXT.isEmpty() ? QStringLiteral("(caret point)") : TEXT),
            static_cast<int>(point.offset));
    }
    const int SOURCE_INDEX = autoChangeSource->findData(PREVIOUS_SOURCE);
    if (SOURCE_INDEX >= 0)
        autoChangeSource->setCurrentIndex(SOURCE_INDEX);
}

void SnippetManagerDialog::refreshAutoChangesList(const Snippet &snippet)
{
    autoChangesList->clear();
    for (const AutoChangeSection &section : snippet.autoChangeSections) {
        const QString TEXT = snippet.code.mid(section.offset, section.length).simplified();
        const auto source = std::find_if(snippet.highlightPoints.cbegin(), snippet.highlightPoints.cend(),
            [&section](const HighlightPoint &point) { return point.offset == section.sourcePointOffset; });
        const QString SOURCE_NUMBER = source == snippet.highlightPoints.cend()
            ? QStringLiteral("?") : QString::number(static_cast<int>(source - snippet.highlightPoints.cbegin()) + 1);
        auto *item = new QListWidgetItem(QStringLiteral("%1 ← highlight point %2")
            .arg(TEXT, SOURCE_NUMBER), autoChangesList);
        item->setData(Qt::UserRole, static_cast<int>(section.offset));
        item->setData(Qt::UserRole + 1, static_cast<int>(section.length));
    }
}

void SnippetManagerDialog::addAutoChangeSection()
{
    if (selectedId.isEmpty() || autoChangeSource->currentIndex() < 0)
        return;
    const QTextCursor selection = codePreview->textCursor();
    if (!selection.hasSelection()) {
        statusLabel->setText(QStringLiteral("Select text in the preview first."));
        return;
    }
    const qsizetype offset = selection.selectionStart();
    const qsizetype length = selection.selectionEnd() - selection.selectionStart();
    const qsizetype sourceOffset = autoChangeSource->currentData().toInt();
    for (const Snippet &snippet : SnippetStore::loadAll()) {
        if (snippet.id != selectedId)
            continue;
        const auto overlaps = [offset, length](qsizetype otherOffset, qsizetype otherLength) {
            return offset < otherOffset + otherLength && otherOffset < offset + length;
        };
        for (const HighlightPoint &point : snippet.highlightPoints) {
            if (overlaps(point.offset, point.length)) {
                statusLabel->setText(QStringLiteral("Auto-change sections must be separate from highlight points."));
                return;
            }
        }
        for (const AutoChangeSection &existing : snippet.autoChangeSections) {
            if (overlaps(existing.offset, existing.length)) {
                statusLabel->setText(QStringLiteral("Auto-change sections cannot overlap."));
                return;
            }
        }
        Snippet updated = snippet;
        updated.autoChangeSections.append(AutoChangeSection{offset, length, sourceOffset});
        std::stable_sort(updated.autoChangeSections.begin(), updated.autoChangeSections.end(),
            [](const AutoChangeSection &left, const AutoChangeSection &right) { return left.offset < right.offset; });
        QString error;
        if (!SnippetStore::save(&updated, &error)) {
            statusLabel->setText(error);
            return;
        }
        refreshAutoChangesList(updated);
        removeAutoChangeButton->setEnabled(true);
        statusLabel->setText(QStringLiteral("Auto-change section saved."));
        return;
    }
}

void SnippetManagerDialog::removeAutoChangeSection()
{
    if (selectedId.isEmpty() || !autoChangesList->currentItem())
        return;
    const qsizetype offset = autoChangesList->currentItem()->data(Qt::UserRole).toInt();
    const qsizetype length = autoChangesList->currentItem()->data(Qt::UserRole + 1).toInt();
    for (const Snippet &snippet : SnippetStore::loadAll()) {
        if (snippet.id != selectedId)
            continue;
        Snippet updated = snippet;
        const auto section = std::find_if(updated.autoChangeSections.begin(), updated.autoChangeSections.end(),
            [offset, length](const AutoChangeSection &item) { return item.offset == offset && item.length == length; });
        if (section == updated.autoChangeSections.end())
            return;
        updated.autoChangeSections.erase(section);
        QString error;
        if (!SnippetStore::save(&updated, &error)) {
            statusLabel->setText(error);
            return;
        }
        refreshAutoChangesList(updated);
        removeAutoChangeButton->setEnabled(!updated.autoChangeSections.isEmpty());
        statusLabel->setText(QStringLiteral("Auto-change section removed."));
        return;
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
        refreshAutoChangesList(snippet);
        autoChangeSource->setEnabled(!snippet.highlightPoints.isEmpty());
        addAutoChangeButton->setEnabled(!snippet.highlightPoints.isEmpty());
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
        snippet.autoChangeSections.erase(std::remove_if(snippet.autoChangeSections.begin(), snippet.autoChangeSections.end(),
            [OFFSET](const AutoChangeSection &section) { return section.sourcePointOffset == OFFSET; }),
            snippet.autoChangeSections.end());
        QString error;
        if (!SnippetStore::save(&snippet, &error)) {
            statusLabel->setText(error);
            return;
        }
        refreshPointsList(snippet);
        refreshAutoChangesList(snippet);
        autoChangeSource->setEnabled(!snippet.highlightPoints.isEmpty());
        addAutoChangeButton->setEnabled(!snippet.highlightPoints.isEmpty());
        removeAutoChangeButton->setEnabled(!snippet.autoChangeSections.isEmpty());
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
