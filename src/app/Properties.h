// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the Properties panel: the settings of the equipped tool (the
// terraform brushes, the light tools) or, for the other tools, what is
// selected: a light or camera to edit, or the faces, edges and components.
#pragma once

#include "Tool.h"
#include "core/Document.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace gs {

class EntityInfoPanel;
class PlacementSettings;
class ScenePanel;
class TerrainPanel;
class Viewport;

// Length unit, display precision and length snapping (Model Info, and the
// Properties panel of the drawing tools). Changes apply at once and are
// remembered for new models.
class UnitsSettings : public QWidget {
    Q_OBJECT
public:
    UnitsSettings(Document* doc, bool showUnit, QWidget* parent = nullptr);
    void refresh();
    // Writes the document's settings as the defaults for new models.
    static void remember(const Units& u);
    static void loadDefaults(Units& u);

signals:
    void changed();

private:
    void apply();
    void fillPrecision();
    Document* doc_;
    QComboBox* unit_ = nullptr;
    QComboBox* precision_;
    QComboBox* snap_;
    QLineEdit* step_;
    QLabel* stepLabel_;
    bool updating_ = false;
};

// Scale tool settings: the selection's sizes, typed to rescale it.
class ScaleSettings : public QWidget {
    Q_OBJECT
public:
    ScaleSettings(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();
    // For tests: type a size (0 W, 1 L, 2 H) as the field would take it.
    bool enter(int axis, const QString& text);
    bool proportional() const;
    void setProportional(bool on);

private:
    Document* doc_;
    Viewport* vp_;
    QLineEdit* fields_[3];
    QCheckBox* keep_;
    QComboBox* anchor_;
    QLabel* note_;
    bool updating_ = false;
};

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    PropertiesPanel(Document* doc, Viewport* vp, EntityInfoPanel* info, ScenePanel* scene, TerrainPanel* terrain,
                    PlacementSettings* placement, QWidget* parent = nullptr);
    // The equipped tool and its name.
    void setTool(ToolId id, const QString& name);
    void refresh();
    // What is shown, for tests: "tool" or "selection".
    QString showing() const { return showing_; }

signals:
    // "Game Properties…" for a selected component.
    void entityPropertiesRequested();

private:
    Document* doc_;
    Viewport* vp_;
    EntityInfoPanel* info_;
    ScenePanel* scene_;
    TerrainPanel* terrain_;
    PlacementSettings* placement_;
    ScaleSettings* scale_;
    UnitsSettings* units_;
    QLabel* title_;
    QLabel* hint_;
    QPushButton* entityBtn_;
    ToolId tool_ = ToolId::Select;
    QString toolName_;
    QString showing_;
};

} // namespace gs
