// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the Open Block Model plugin's windows: a picture not found (Missing
// Texture, with "Fix it": pick the file), and the OBM tab of a component's
// Properties (its group's tags, its cubes' tags, the model's, its pictures).
#pragma once

#include "core/Document.h"

#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace gs::obm {

// "Fix it": the picture for a material whose picture wasn't found, picked from the
// files. Picked from where it would be in a textures folder ("…/textures/block/
// lamp.png" for block/lamp), that folder is remembered (obm/texturesDir) and every
// other missing picture found there is taken from it too. Returns how many were fixed.
int fixTexture(QWidget* parent, Document& doc, Id material, const QString& modelPath, const std::vector<Id>& others = {});

// After reading a model: each picture not found, one popup at a time (Fix it, Skip,
// Skip all). Those still missing are named in `messages`.
void askMissing(QWidget* parent, Document& doc, std::vector<Id> missing, const QString& modelPath, QStringList* messages);

class ObmTab : public QWidget {
    Q_OBJECT
public:
    ObmTab(Document* doc, QWidget* parent = nullptr);
    // What it shows: the group of this component instance, and its model. false: not from an .obm.
    bool showFor(Id instance);

private:
    void edit(const char* what, const std::function<void()>& fn);
    Document* doc_;
    Id inst_ = kNone, def_ = kNone;
    bool filling_ = false;
    QLabel* group_;
    QPlainTextEdit* tags_;
    QPushButton* applyTags_;
    QTableWidget* cubes_;
    QLabel* model_;
    QLineEdit* modelTags_;
    QTableWidget* pictures_;
};

} // namespace gs::obm
