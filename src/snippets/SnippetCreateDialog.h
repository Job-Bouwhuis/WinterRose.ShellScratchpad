#pragma once

#include <QDialog>

#include "SnippetStore.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
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
    QLabel *statusLabel = nullptr;
    QPushButton *removePointButton = nullptr;
    QVector<HighlightPoint> highlightPoints;

    void refreshPointsList();
    void addHighlightPoint();
    void removeHighlightPoint();
    void saveSnippet();
};
}
