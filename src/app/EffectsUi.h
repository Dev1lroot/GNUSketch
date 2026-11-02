// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the Effects menu: a list of effects and their settings dialog
// with live preview.
#pragma once

#include "core/Effects.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <functional>
#include <string>
#include <vector>

class QLabel;
class QDialogButtonBox;

namespace gs {

struct EffectParam {
    enum Type { Double, Int, Bool, Choice, Seed };
    QString key, label;
    Type type = Double;
    double min = 0, max = 100, def = 0;
    int decimals = 0;
    QString suffix;
    bool logScale = false; // slider moves in ratios (for ranges over several decades)
    QStringList choices;
    QString tip;
};

struct EffectDef {
    QString name, menuText;
    QString submenu; // groups entries in the menu
    QString icon;
    QString tip;
    std::vector<EffectParam> params; // none: runs without a dialog
    std::function<bool(Mesh&, const std::set<Id>&, const QVariantMap&, EffectResult*, std::string*)> apply;
    QString key() const; // object name of the menu action, settings key
};

const std::vector<EffectDef>& effectRegistry();

// Calls `preview` shortly after a setting changes; it applies the effect and
// returns the text to show. `setOk` of the same call enables the OK button.
class EffectDialog : public QDialog {
    Q_OBJECT
public:
    using Preview = std::function<QString(const QVariantMap& values, bool& ok)>;
    EffectDialog(const EffectDef& def, const QVariantMap& values, Preview preview, QWidget* parent);
    const QVariantMap& values() const { return values_; }

private:
    void runPreview();
    QVariantMap values_;
    Preview preview_;
    QTimer timer_;
    QLabel* message_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace gs
