#pragma once

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVector>
#include "snippets/SnippetStore.h"

class QListWidget;
class QPlainTextEdit;
class QProcess;

namespace ShellScratchpad::Completion
{
class CompletionController final : public QObject
{
public:
    explicit CompletionController(QPlainTextEdit *targetEditor, QObject *parent = nullptr);
    void refreshCommandCache();
    void refreshSnippets();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Suggestion
    {
        QString text;
        QString description;
        QString category;
        QString insertText;
        QVector<Snippets::HighlightPoint> highlightPoints;
    };

    QPlainTextEdit *editor = nullptr;
    QListWidget *popup = nullptr;
    QProcess *descriptionLookupProcess = nullptr;
    QStringList systemCommands;
    QVector<Snippets::Snippet> snippets;
    QStringList descriptionLookupNames;
    QHash<QString, QString> commandDescriptions;
    QString descriptionLookupBuffer;
    QString descriptionLookupProgram;
    QStringList descriptionLookupArguments;
    qsizetype descriptionLookupOffset = 0;
    QVector<Suggestion> currentSuggestions;
    qsizetype replacementStart = 0;
    qsizetype replacementEnd = 0;
    QString currentCommand;
    bool commandPosition = true;
    quint64 descriptionLookupGeneration = 0;
    quint64 activeDescriptionLookupGeneration = 0;

    void updateSuggestions();
    void positionPopup();
    void acceptCurrentSuggestion();
    void showSelectedDescription();
    void startNextDescriptionLookupBatch();
    void consumeDescriptionLookupOutput();
    void parseDescriptionLookupLine(const QString &line);
    QString commandDescription(const QString &command) const;
};
}
