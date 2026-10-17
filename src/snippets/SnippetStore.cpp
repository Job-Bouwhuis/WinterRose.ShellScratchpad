#include "SnippetStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace ShellScratchpad::Snippets
{
namespace
{
QString snippetFileStem(const QString &name)
{
    QString stem = name.trimmed();
    stem.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("_"));
    while (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' ')))
        stem.chop(1);
    return stem.isEmpty() ? QStringLiteral("snippet") : stem;
}

QJsonObject snippetObject(const Snippet &snippet)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), snippet.id);
    object.insert(QStringLiteral("name"), snippet.name.trimmed());
    object.insert(QStringLiteral("description"), snippet.description.trimmed());
    object.insert(QStringLiteral("code"), snippet.code);
    QJsonArray points;
    for (const HighlightPoint &point : snippet.highlightPoints) {
        if (point.offset < 0 || point.length < 0 || point.offset + point.length > snippet.code.size())
            continue;
        QJsonObject pointObject;
        pointObject.insert(QStringLiteral("offset"), static_cast<int>(point.offset));
        pointObject.insert(QStringLiteral("length"), static_cast<int>(point.length));
        points.append(pointObject);
    }
    object.insert(QStringLiteral("highlightPoints"), points);
    return object;
}

bool writeSnippetFile(const Snippet &snippet, const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(snippetObject(snippet)).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

Snippet readSnippetFile(const QString &path)
{
    Snippet snippet;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return snippet;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return snippet;
    const QJsonObject object = document.object();
    snippet.id = object.value(QStringLiteral("id")).toString();
    snippet.name = object.value(QStringLiteral("name")).toString().trimmed();
    snippet.description = object.value(QStringLiteral("description")).toString().trimmed();
    snippet.code = object.value(QStringLiteral("code")).toString();
    snippet.filePath = path;
    for (const QJsonValue &pointValue : object.value(QStringLiteral("highlightPoints")).toArray()) {
        const QJsonObject pointObject = pointValue.toObject();
        const qsizetype offset = pointObject.value(QStringLiteral("offset")).toInt(-1);
        const qsizetype length = pointObject.value(QStringLiteral("length")).toInt(-1);
        if (offset >= 0 && length >= 0 && offset + length <= snippet.code.size())
            snippet.highlightPoints.append(HighlightPoint{offset, length});
    }
    return snippet;
}

QString availableSnippetPath(const Snippet &snippet)
{
    const QDir root(SnippetStore::directoryPath());
    QString stem = snippetFileStem(snippet.name);
    QString path = root.filePath(stem + QStringLiteral(".json"));
    if (!QFileInfo::exists(path) || readSnippetFile(path).id == snippet.id)
        return path;
    stem += QStringLiteral("_") + snippet.id.left(8);
    path = root.filePath(stem + QStringLiteral(".json"));
    int suffix = 2;
    while (QFileInfo::exists(path) && readSnippetFile(path).id != snippet.id)
        path = root.filePath(QStringLiteral("%1_%2.json").arg(stem).arg(suffix++));
    return path;
}
}

QString SnippetStore::directoryPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("snippets"));
}

QVector<Snippet> SnippetStore::loadAll()
{
    QVector<Snippet> snippets;
    const QDir root(directoryPath());
    QSet<QString> seenIds;
    const QFileInfoList files = root.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
        Snippet snippet = readSnippetFile(file.absoluteFilePath());
        if (snippet.name.isEmpty())
            continue;
        if (snippet.id.isEmpty())
            snippet.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (seenIds.contains(snippet.id))
            continue;
        seenIds.insert(snippet.id);
        snippets.append(snippet);
    }

    // Import the previous one-folder-per-snippet layout into the flat JSON folder.
    const QFileInfoList folders = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &folder : folders) {
        const QString path = QDir(folder.absoluteFilePath()).filePath(QStringLiteral("snippet.json"));
        Snippet snippet = readSnippetFile(path);
        if (snippet.name.isEmpty() || seenIds.contains(snippet.id))
            continue;
        if (snippet.id.isEmpty())
            snippet.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString targetPath = availableSnippetPath(snippet);
        if (writeSnippetFile(snippet, targetPath, nullptr)) {
            snippet.filePath = targetPath;
            snippets.append(snippet);
            seenIds.insert(snippet.id);
            QFile::remove(path);
            QDir().rmdir(folder.absoluteFilePath());
        }
    }
    std::sort(snippets.begin(), snippets.end(), [](const Snippet &left, const Snippet &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return snippets;
}

bool SnippetStore::nameIsAvailable(const QString &name, const QString &exceptId)
{
    const QString NORMALIZED = name.trimmed();
    if (NORMALIZED.isEmpty())
        return false;
    for (const Snippet &snippet : loadAll()) {
        if (snippet.id != exceptId && snippet.name.compare(NORMALIZED, Qt::CaseInsensitive) == 0)
            return false;
    }
    return true;
}

bool SnippetStore::save(Snippet *snippet, QString *error)
{
    if (!snippet || snippet->name.trimmed().isEmpty() || snippet->description.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("A snippet name and description are required.");
        return false;
    }
    const QVector<Snippet> existingSnippets = loadAll();
    if (!nameIsAvailable(snippet->name, snippet->id)) {
        if (error)
            *error = QStringLiteral("A snippet with that name already exists.");
        return false;
    }
    if (snippet->id.isEmpty())
        snippet->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = availableSnippetPath(*snippet);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the snippet folder.");
        return false;
    }
    const QString OLD_PATH = [&existingSnippets, snippet]() {
        for (const Snippet &existing : existingSnippets) {
            if (existing.id == snippet->id)
                return existing.filePath;
        }
        return QString();
    }();
    if (!writeSnippetFile(*snippet, path, error))
        return false;
    if (!OLD_PATH.isEmpty() && OLD_PATH != path)
        QFile::remove(OLD_PATH);
    snippet->name = snippet->name.trimmed();
    snippet->description = snippet->description.trimmed();
    snippet->filePath = path;
    return true;
}

bool SnippetStore::remove(const QString &id, QString *error)
{
    for (const Snippet &snippet : loadAll()) {
        if (snippet.id != id)
            continue;
        if (QFile::remove(snippet.filePath))
            return true;
        if (error)
            *error = QStringLiteral("Could not delete snippet file: %1").arg(snippet.filePath);
        return false;
    }
    if (error)
        *error = QStringLiteral("The selected snippet could not be found on disk.");
    return false;
}

bool SnippetStore::create(const QString &name, const QString &description,
                          const QString &code, Snippet *created, QString *error)
{
    if (!validateSyntax(code, error))
        return false;
    Snippet snippet;
    snippet.name = name;
    snippet.description = description;
    snippet.code = code;
    if (!save(&snippet, error))
        return false;
    if (created)
        *created = snippet;
    return true;
}

bool SnippetStore::validateSyntax(const QString &code, QString *error)
{
    const QString EXECUTABLE = QStandardPaths::findExecutable(QStringLiteral("shellcheck"));
    if (EXECUTABLE.isEmpty()) {
        if (error)
            *error = QStringLiteral("ShellCheck is required to validate and create snippets.");
        return false;
    }
    QProcess process;
    process.start(EXECUTABLE, {QStringLiteral("-f"), QStringLiteral("json"),
                               QStringLiteral("-s"), QStringLiteral("bash"), QStringLiteral("-")});
    if (!process.waitForStarted(1500)) {
        if (error)
            *error = QStringLiteral("Could not start ShellCheck: %1").arg(process.errorString());
        return false;
    }
    process.write(code.toUtf8());
    process.closeWriteChannel();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("ShellCheck timed out while validating the snippet.");
        return false;
    }
    const QByteArray OUTPUT = process.readAllStandardOutput();
    const QJsonDocument DOCUMENT = QJsonDocument::fromJson(OUTPUT);
    if (process.exitStatus() != QProcess::NormalExit || !DOCUMENT.isArray()) {
        if (error)
            *error = QStringLiteral("ShellCheck could not validate this snippet.");
        return false;
    }
    QStringList syntaxErrors;
    for (const QJsonValue &value : DOCUMENT.array()) {
        const QJsonObject finding = value.toObject();
        if (finding.value(QStringLiteral("level")).toString() != QLatin1String("error"))
            continue;
        syntaxErrors.append(QStringLiteral("Line %1, SC%2: %3")
            .arg(finding.value(QStringLiteral("line")).toInt())
            .arg(finding.value(QStringLiteral("code")).toInt(), 4, 10, QLatin1Char('0'))
            .arg(finding.value(QStringLiteral("message")).toString()));
    }
    if (!syntaxErrors.isEmpty()) {
        if (error)
            *error = QStringLiteral("Snippet has Bash syntax errors:\n%1")
                .arg(syntaxErrors.join(QLatin1Char('\n')));
        return false;
    }
    return true;
}
}
