#pragma once

#include "SyntaxToken.h"

#include <QObject>
#include <QColor>
#include <QMap>

namespace ShellScratchpad::Syntax
{
class SyntaxColorSettings final : public QObject
{
    Q_OBJECT
public:
    static SyntaxColorSettings *instance();
    QColor color(TokenKind kind) const;
    void setColor(TokenKind kind, const QColor &color);
    void restoreDefaults();
    void save();
    static QMap<int, QColor> defaults();
    static QString settingsFilePath();

signals:
    void colorsChanged();

private:
    explicit SyntaxColorSettings(QObject *parent = nullptr);
    QMap<int, QColor> colors;
};
}
