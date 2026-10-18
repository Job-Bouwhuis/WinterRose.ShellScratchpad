#include "SyntaxColorSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace ShellScratchpad::Syntax
{
namespace
{
QString colorKey(TokenKind kind)
{
    return QString::number(static_cast<int>(kind));
}
}

QMap<int, QColor> SyntaxColorSettings::defaults()
{
    return {
        {static_cast<int>(TokenKind::Comment), QColor(QStringLiteral("#8bc98b"))},
        {static_cast<int>(TokenKind::StringDelimiter), QColor(QStringLiteral("#e6c07b"))},
        {static_cast<int>(TokenKind::StringContent), QColor(QStringLiteral("#e6c07b"))},
        {static_cast<int>(TokenKind::Variable), QColor(QStringLiteral("#56c8c8"))},
        {static_cast<int>(TokenKind::SpecialParameter), QColor(QStringLiteral("#61afef"))},
        {static_cast<int>(TokenKind::EscapeSequence), QColor(QStringLiteral("#c678dd"))},
        {static_cast<int>(TokenKind::CommandSubstitutionStart), QColor(QStringLiteral("#c678dd"))},
        {static_cast<int>(TokenKind::BacktickCommandSubstitutionDelimiter), QColor(QStringLiteral("#c678dd"))},
        {static_cast<int>(TokenKind::ArithmeticExpansionStart), QColor(QStringLiteral("#c678dd"))},
        {static_cast<int>(TokenKind::Operator), QColor(QStringLiteral("#abb2bf"))},
        {static_cast<int>(TokenKind::Redirection), QColor(QStringLiteral("#e06c75"))},
        {static_cast<int>(TokenKind::Number), QColor(QStringLiteral("#d19a66"))},
        {static_cast<int>(TokenKind::Command), QColor(QStringLiteral("#61afef"))},
        {static_cast<int>(TokenKind::Builtin), QColor(QStringLiteral("#56b6c2"))},
        {static_cast<int>(TokenKind::Keyword), QColor(QStringLiteral("#c678dd"))},
        {static_cast<int>(TokenKind::AssignmentName), QColor(QStringLiteral("#e5c07b"))},
        {static_cast<int>(TokenKind::Option), QColor(QStringLiteral("#d19a66"))},
        {static_cast<int>(TokenKind::Path), QColor(QStringLiteral("#98c379"))},
        {static_cast<int>(TokenKind::Argument), QColor(QStringLiteral("#bf6466"))},
        {static_cast<int>(TokenKind::Word), QColor()}
    };
}

SyntaxColorSettings *SyntaxColorSettings::instance()
{
    static SyntaxColorSettings *settings = new SyntaxColorSettings(QCoreApplication::instance());
    return settings;
}

SyntaxColorSettings::SyntaxColorSettings(QObject *parent)
    : QObject(parent), colors(defaults())
{
    const QString SETTINGS_PATH = settingsFilePath();
    QSettings settings(SETTINGS_PATH, QSettings::IniFormat);
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const QString VALUE = settings.value(colorKey(static_cast<TokenKind>(it.key())), it.value().name()).toString();
        const QColor COLOR(VALUE);
        if (COLOR.isValid())
            it.value() = COLOR;
    }
    if (settings.status() == QSettings::NoError && settings.allKeys().isEmpty())
        save();
}

QColor SyntaxColorSettings::color(TokenKind kind) const
{
    return colors.value(static_cast<int>(kind));
}

void SyntaxColorSettings::setColor(TokenKind kind, const QColor &colorValue)
{
    if (!colorValue.isValid())
        return;
    colors[static_cast<int>(kind)] = colorValue;
    save();
    emit colorsChanged();
}

void SyntaxColorSettings::restoreDefaults()
{
    colors = defaults();
    save();
    emit colorsChanged();
}

void SyntaxColorSettings::save()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    for (auto it = colors.cbegin(); it != colors.cend(); ++it) {
        if (it.value().isValid())
            settings.setValue(colorKey(static_cast<TokenKind>(it.key())), it.value().name());
        else
            settings.remove(colorKey(static_cast<TokenKind>(it.key())));
    }
    settings.sync();
}

QString SyntaxColorSettings::settingsFilePath()
{
    const QString CONFIG_DIR = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(CONFIG_DIR);
    const QString NEW_PATH = QDir(CONFIG_DIR).filePath(QStringLiteral("syntax-colors.ini"));
    const QString OLD_PATH = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("config/syntax-colors.ini"));
    if (!QFileInfo::exists(NEW_PATH) && QFileInfo::exists(OLD_PATH))
        QFile::copy(OLD_PATH, NEW_PATH);
    return NEW_PATH;
}
}
