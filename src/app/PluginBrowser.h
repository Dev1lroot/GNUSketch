// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Help > Plugins: the plugins found, with their vendor, license,
// version and formats, each turned on or off (from the next start).
#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace gs {

class PluginBrowser : public QDialog {
    Q_OBJECT
public:
    explicit PluginBrowser(QWidget* parent = nullptr);
    // Whether a plugin was turned on or off here (a restart applies it).
    bool changed() const { return changed_; }

signals:
    void restartRequested();

private:
    void fill();
    void showDetails();
    QLineEdit* search_;
    QListWidget* list_;
    QTextBrowser* details_;
    QLabel* note_;
    QPushButton* restart_;
    bool changed_ = false;
};

} // namespace gs
