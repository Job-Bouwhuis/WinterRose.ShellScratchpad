#pragma once

#include <QDialog>

#include "SnippetStore.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QComboBox;
namespace ShellScratchpad { class CodeEditor; }

namespace ShellScratchpad::Snippets
{
class SnippetCreateDialog final : public QDialog
{
public:
    explicit SnippetCreateDialog(const QString &initialCode, QWidget *parent = nullptr);
    Snippet snippet() const;

private:
    QLineEdit *nameEdit = nullptr;
    QLineEdit *descriptionEdit = nullptr;
    ShellScratchpad::CodeEditor *codePreview = nullptr;
    QListWidget *pointsList = nullptr;
    QListWidget *autoChangesList = nullptr;
    QComboBox *autoChangeSource = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *removePointButton = nullptr;
    QPushButton *removeAutoChangeButton = nullptr;
    QVector<HighlightPoint> highlightPoints;
    QVector<AutoChangeSection> autoChangeSections;

    void refreshPointsList();
    void refreshAutoChangesList();
    void addHighlightPoint();
    void removeHighlightPoint();
    void addAutoChangeSection();
    void removeAutoChangeSection();
    void saveSnippet();
};
}
