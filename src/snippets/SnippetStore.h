#pragma once

#include <QString>
#include <QVector>

namespace ShellScratchpad::Snippets
{
struct HighlightPoint
{
    qsizetype offset = 0;
    qsizetype length = 0;
};

struct Snippet
{
    QString id;
    QString name;
    QString description;
    QString code;
    QString filePath;
    QVector<HighlightPoint> highlightPoints;
};

class SnippetStore final
{
public:
    static QString directoryPath();
    static QVector<Snippet> loadAll();
    static bool save(Snippet *snippet, QString *error = nullptr);
    static bool remove(const QString &id, QString *error = nullptr);
    static bool create(const QString &name, const QString &description,
                       const QString &code, Snippet *created = nullptr,
                       QString *error = nullptr);
    static bool validateSyntax(const QString &code, QString *error = nullptr);
    static bool nameIsAvailable(const QString &name, const QString &exceptId = QString());
};
}
