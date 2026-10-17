#pragma once

#include <QDialog>

namespace ShellScratchpad::Syntax
{
class SyntaxColorSettings;

class SyntaxColorDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit SyntaxColorDialog(QWidget *parent = nullptr);

private:
    void rebuild();
    SyntaxColorSettings *settings;
    QWidget *rows = nullptr;
};
}
