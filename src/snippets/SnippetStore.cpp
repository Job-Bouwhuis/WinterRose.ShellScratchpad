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
    QJsonArray autoChanges;
    for (const AutoChangeSection &section : snippet.autoChangeSections) {
        if (section.offset < 0 || section.length < 0 || section.offset + section.length > snippet.code.size())
            continue;
        QJsonObject sectionObject;
        sectionObject.insert(QStringLiteral("offset"), static_cast<int>(section.offset));
        sectionObject.insert(QStringLiteral("length"), static_cast<int>(section.length));
        sectionObject.insert(QStringLiteral("sourcePointOffset"), static_cast<int>(section.sourcePointOffset));
        autoChanges.append(sectionObject);
    }
    object.insert(QStringLiteral("autoChangeSections"), autoChanges);
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
    for (const QJsonValue &sectionValue : object.value(QStringLiteral("autoChangeSections")).toArray()) {
        const QJsonObject sectionObject = sectionValue.toObject();
        const qsizetype offset = sectionObject.value(QStringLiteral("offset")).toInt(-1);
        const qsizetype length = sectionObject.value(QStringLiteral("length")).toInt(-1);
        const qsizetype sourceOffset = sectionObject.value(QStringLiteral("sourcePointOffset")).toInt(-1);
        const bool HAS_SOURCE = std::any_of(snippet.highlightPoints.cbegin(), snippet.highlightPoints.cend(),
            [sourceOffset](const HighlightPoint &point) { return point.offset == sourceOffset; });
        if (offset >= 0 && length >= 0 && offset + length <= snippet.code.size() && HAS_SOURCE)
            snippet.autoChangeSections.append(AutoChangeSection{offset, length, sourceOffset});
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

QVector<Snippet> SnippetStore::builtInSnippets()
{
    const auto makeSnippet = [](const QString &id, const QString &name, const QString &description,
                                const QString &code, const QStringList &placeholders = QStringList()) {
        Snippet snippet;
        snippet.id = QStringLiteral("builtin-") + id;
        snippet.name = name;
        snippet.description = description;
        snippet.code = code;
        for (const QString &placeholder : placeholders) {
            const qsizetype offset = snippet.code.indexOf(placeholder);
            if (offset >= 0)
                snippet.highlightPoints.append(HighlightPoint{offset, placeholder.size()});
        }
        return snippet;
    };

    QVector<Snippet> snippets = {
        makeSnippet(QStringLiteral("function"), QStringLiteral("function"),
            QStringLiteral("A reusable Bash function with a return status."),
            QStringLiteral("function my_function() {\n    echo \"Hello World\"\n}\n"),
            {QStringLiteral("my_function"), QStringLiteral("echo \"Hello World\"")}),
        makeSnippet(QStringLiteral("for-list"), QStringLiteral("forlist"),
            QStringLiteral("Run a block once for each item in a list."),
            QStringLiteral("for item in \"${items[@]}\"; do\n    printf '%s\\n' \"$item\"\ndone\n"),
            {QStringLiteral("items"), QStringLiteral("item")}),
        makeSnippet(QStringLiteral("while-read"), QStringLiteral("readlines"),
            QStringLiteral("Safely read a file line by line, preserving whitespace."),
            QStringLiteral("while IFS= read -r line; do\n    printf '%s\\n' \"$line\"\ndone < \"${input_file}\"\n"),
            {QStringLiteral("input_file"), QStringLiteral("line")}),
        makeSnippet(QStringLiteral("if-file"), QStringLiteral("fileexists"),
            QStringLiteral("Branch based on whether a file exists."),
            QStringLiteral("if [[ -f \"${file_path}\" ]]; then\n    printf 'Found: %s\\n' \"$file_path\"\nelse\n    printf 'Missing: %s\\n' \"$file_path\" >&2\nfi\n"),
            {QStringLiteral("file_path")}),
        makeSnippet(QStringLiteral("variable"), QStringLiteral("localvariable"),
            QStringLiteral("Declare a function-local variable with a default value."),
            QStringLiteral("local variable_name=\"${1:-default_value}\"\n"),
            {QStringLiteral("variable_name"), QStringLiteral("default_value")}),
        makeSnippet(QStringLiteral("case"), QStringLiteral("casestatement"),
            QStringLiteral("Handle a value with several matching patterns."),
            QStringLiteral("case \"${choice}\" in\n    start)\n        printf 'Starting…\\n'\n        ;;\n    stop)\n        printf 'Stopping…\\n'\n        ;;\n    *)\n        printf 'Unknown choice: %s\\n' \"$choice\" >&2\n        return 2\n        ;;\nesac\n"),
            {QStringLiteral("choice"), QStringLiteral("start"), QStringLiteral("stop")}),
        makeSnippet(QStringLiteral("command-check"), QStringLiteral("checkcommand"),
            QStringLiteral("Check that a required executable is available."),
            QStringLiteral("if ! command -v \"${command_name}\" >/dev/null 2>&1; then\n    printf 'Required command not found: %s\\n' \"$command_name\" >&2\n    return 1\nfi\n"),
            {QStringLiteral("command_name")}),
        makeSnippet(QStringLiteral("temp-cleanup"), QStringLiteral("tempfile"),
            QStringLiteral("Create a temporary file and remove it on exit."),
            QStringLiteral("temp_file=$(mktemp)\ntrap 'rm -f -- \"$temp_file\"' EXIT\n\n"),
            {QStringLiteral("temp_file")}),
        makeSnippet(QStringLiteral("argument-default"), QStringLiteral("argumentwithdefault"),
            QStringLiteral("Read a positional argument and provide a fallback."),
            QStringLiteral("value=\"${1:-default_value}\"\n"),
            {QStringLiteral("default_value")})
    };

    const auto linkRepeatedValue = [&snippets](const QString &id, const QString &value, int targetOccurrence = 1) {
        auto snippet = std::find_if(snippets.begin(), snippets.end(), [&id](const Snippet &item) {
            return item.id == QStringLiteral("builtin-") + id;
        });
        if (snippet == snippets.end())
            return;
        const qsizetype sourceOffset = snippet->code.indexOf(value);
        if (sourceOffset < 0)
            return;
        const bool HAS_POINT = std::any_of(snippet->highlightPoints.cbegin(), snippet->highlightPoints.cend(),
            [sourceOffset, &value](const HighlightPoint &point) {
                return point.offset == sourceOffset && point.length == value.size();
            });
        if (!HAS_POINT)
            snippet->highlightPoints.append(HighlightPoint{sourceOffset, value.size()});
        qsizetype targetOffset = sourceOffset + value.size();
        for (int occurrence = 1; occurrence <= targetOccurrence; ++occurrence) {
            targetOffset = snippet->code.indexOf(value, targetOffset);
            if (targetOffset < 0)
                return;
            targetOffset += value.size();
        }
        targetOffset -= value.size();
        snippet->autoChangeSections.append(AutoChangeSection{targetOffset, value.size(), sourceOffset});
    };
    linkRepeatedValue(QStringLiteral("for-list"), QStringLiteral("item"));
    linkRepeatedValue(QStringLiteral("while-read"), QStringLiteral("line"));
    linkRepeatedValue(QStringLiteral("if-file"), QStringLiteral("file_path"), 2);
    linkRepeatedValue(QStringLiteral("case"), QStringLiteral("choice"));
    linkRepeatedValue(QStringLiteral("command-check"), QStringLiteral("command_name"));
    linkRepeatedValue(QStringLiteral("temp-cleanup"), QStringLiteral("temp_file"));
    return snippets;
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
    for (const Snippet &snippet : builtInSnippets()) {
        if (snippet.name.compare(NORMALIZED, Qt::CaseInsensitive) == 0)
            return false;
    }
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
