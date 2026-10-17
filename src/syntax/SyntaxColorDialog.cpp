#include "SyntaxColorDialog.h"

#include "SyntaxColorSettings.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ShellScratchpad::Syntax
{
namespace
{
const QList<QPair<TokenKind, QString>> COLOR_ROLES = {
    {TokenKind::Comment, QStringLiteral("Comments")},
    {TokenKind::StringDelimiter, QStringLiteral("Strings")},
    {TokenKind::Variable, QStringLiteral("Variables")},
    {TokenKind::SpecialParameter, QStringLiteral("Special parameters")},
    {TokenKind::EscapeSequence, QStringLiteral("Escape sequences")},
    {TokenKind::Command, QStringLiteral("Commands")},
    {TokenKind::Builtin, QStringLiteral("Shell builtins")},
    {TokenKind::Keyword, QStringLiteral("Keywords")},
    {TokenKind::AssignmentName, QStringLiteral("Assignment names")},
    {TokenKind::Option, QStringLiteral("Options")},
    {TokenKind::Path, QStringLiteral("Paths")},
    {TokenKind::Redirection, QStringLiteral("Redirections")},
    {TokenKind::Operator, QStringLiteral("Operators")},
    {TokenKind::Number, QStringLiteral("Numbers")},
    {TokenKind::Argument, QStringLiteral("Arguments / plain words")}
};
}

SyntaxColorDialog::SyntaxColorDialog(QWidget *parent)
    : QDialog(parent), settings(SyntaxColorSettings::instance())
{
    setWindowTitle(QStringLiteral("Syntax Colors"));
    resize(430, 520);
    auto *layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    rows = new QWidget(scroll);
    scroll->setWidget(rows);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *resetButton = buttons->addButton(QStringLiteral("Restore Defaults"), QDialogButtonBox::ResetRole);
    connect(resetButton, &QPushButton::clicked, settings, &SyntaxColorSettings::restoreDefaults);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    rebuild();
    connect(settings, &SyntaxColorSettings::colorsChanged, this, &SyntaxColorDialog::rebuild);
}

void SyntaxColorDialog::rebuild()
{
    if (QLayout *oldLayout = rows->layout()) {
        while (QLayoutItem *item = oldLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        delete oldLayout;
    }
    auto *form = new QFormLayout(rows);
    form->setContentsMargins(10, 10, 10, 10);
    for (const auto &role : COLOR_ROLES) {
        auto *button = new QPushButton(settings->color(role.first).name(), rows);
        button->setMinimumWidth(110);
        button->setStyleSheet(QStringLiteral("background-color: %1; color: #202020;").arg(settings->color(role.first).name()));
        connect(button, &QPushButton::clicked, this, [this, button, kind = role.first]() {
            const QColor SELECTED = QColorDialog::getColor(settings->color(kind), this,
                                                            QStringLiteral("Choose syntax color"));
            if (SELECTED.isValid())
                settings->setColor(kind, SELECTED);
        });
        form->addRow(role.second, button);
    }
    auto *hereDocButton = new QPushButton(settings->hereDocBackground().name(), rows);
    hereDocButton->setMinimumWidth(110);
    hereDocButton->setStyleSheet(QStringLiteral("background-color: %1; color: #202020;")
                                     .arg(settings->hereDocBackground().name()));
    connect(hereDocButton, &QPushButton::clicked, this, [this, hereDocButton]() {
        const QColor SELECTED = QColorDialog::getColor(settings->hereDocBackground(), this,
                                                        QStringLiteral("Choose here-document background"));
        if (SELECTED.isValid())
            settings->setHereDocBackground(SELECTED);
    });
    form->addRow(QStringLiteral("Here-document background"), hereDocButton);
}
}
