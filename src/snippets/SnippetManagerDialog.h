#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTabWidget;
class QComboBox;

namespace ShellScratchpad::Snippets
{
struct Snippet;
class SnippetManagerDialog final : public QDialog
{
public:
    explicit SnippetManagerDialog(QWidget *parent = nullptr);

private:
    QListWidget *snippetList = nullptr;
    QListWidget *builtInList = nullptr;
    QTabWidget *snippetTabs = nullptr;
    QLineEdit *nameEdit = nullptr;
    QLineEdit *descriptionEdit = nullptr;
    QPlainTextEdit *codePreview = nullptr;
    QListWidget *pointsList = nullptr;
    QListWidget *autoChangesList = nullptr;
    QComboBox *autoChangeSource = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *reloadButton = nullptr;
    QPushButton *deleteButton = nullptr;
    QPushButton *addPointButton = nullptr;
    QPushButton *removePointButton = nullptr;
    QPushButton *addAutoChangeButton = nullptr;
    QPushButton *removeAutoChangeButton = nullptr;
    QTimer *saveTimer = nullptr;
    QString selectedId;
    bool loading = false;

    void reloadFromDisk();
    void showCurrentSnippet();
    void saveCurrentMetadata();
    void refreshPointsList(const Snippet &snippet);
    void addHighlightPoint();
    void removeHighlightPoint();
    void refreshAutoChangesList(const Snippet &snippet);
    void addAutoChangeSection();
    void removeAutoChangeSection();
    void deleteCurrentSnippet();
    QListWidget *currentSnippetList() const;
};
}
