#pragma once

#include <QObject>
#include <QHash>
#include <QPoint>
#include <QStringList>
#include <QTimer>
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
    struct ScriptSymbol
    {
        enum class Kind { Variable, Function };
        QString name;
        Kind kind = Kind::Variable;
        qsizetype declarationOffset = 0;
        int declarationLine = 1;
        int declarationColumn = 1;
        bool readOnly = false;
        QString value;
    };

    explicit CompletionController(QPlainTextEdit *targetEditor, QObject *parent = nullptr);
    void refreshCommandCache();
    void refreshSnippets();
    void setProfileEnvironmentVariables(const QStringList &names);
    void setRuntimeVariables(const QHash<QString, QString> &variables);
    const QVector<ScriptSymbol> &scriptSymbols() const { return knownScriptSymbols; }

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
        QVector<Snippets::AutoChangeSection> autoChangeSections;
    };

    QPlainTextEdit *editor = nullptr;
    QListWidget *popup = nullptr;
    QProcess *descriptionLookupProcess = nullptr;
    QStringList systemCommands;
    QStringList profileEnvironmentVariables;
    QHash<QString, QString> runtimeVariables;
    QVector<Snippets::Snippet> snippets;
    QStringList descriptionLookupNames;
    QHash<QString, QString> commandDescriptions;
    QString descriptionLookupBuffer;
    QString descriptionLookupProgram;
    QStringList descriptionLookupArguments;
    qsizetype descriptionLookupOffset = 0;
    QVector<Suggestion> currentSuggestions;
    QVector<ScriptSymbol> knownScriptSymbols;
    qsizetype replacementStart = 0;
    qsizetype replacementEnd = 0;
    QString currentCommand;
    bool commandPosition = true;
    quint64 descriptionLookupGeneration = 0;
    quint64 activeDescriptionLookupGeneration = 0;
    QTimer hoverDescriptionTimer;
    QTimer selectedDescriptionTimer;
    QPoint hoverPosition;
    qsizetype hoverTokenStart = -1;
    QString hoverDescription;

    void updateSuggestions();
    void positionPopup();
    void acceptCurrentSuggestion();
    void showSelectedDescription();
    void startNextDescriptionLookupBatch();
    void consumeDescriptionLookupOutput();
    void parseDescriptionLookupLine(const QString &line);
    QString commandDescription(const QString &command) const;
    void refreshScriptSymbols(const QString &source);
    void updateHoverTarget(const QPoint &position);
    void showHoverDescription();
    QString descriptionForTokenAt(const QPoint &position, qsizetype *tokenStart) const;
};
}
