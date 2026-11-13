// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - lights and cameras in the world: the placing tool and the panel.
#pragma once

#include "Tool.h"
#include "core/Document.h"

#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QFormLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QWidget;

namespace gs {

class Viewport;

class PlaceObjectTool : public Tool {
public:
    PlaceObjectTool(Viewport* vp, ToolId id);
    ToolId id() const override { return id_; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Height"); }
    bool busy() const override { return aiming_; }
    void reset() override;
    void mouseMove(const MouseEvent& e) override;
    void mousePress(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    Vec3 standPoint() const;
    void place(const Vec3& target);
    SceneObject::Kind kind() const;

    ToolId id_;
    bool aiming_ = false;
    bool hasPoint_ = false;
    double height_;
    Vec3 pos_;
};

// What the next light placed by the light tools is like (Properties panel
// while such a tool is equipped).
SceneObject& placementDefaults(SceneObject::Kind kind);

// Edits those settings for one kind of light.
class PlacementSettings : public QWidget {
    Q_OBJECT
public:
    explicit PlacementSettings(QWidget* parent = nullptr);
    void setKind(SceneObject::Kind kind);
    void refresh();

private:
    SceneObject::Kind kind_ = SceneObject::PointLight;
    QFormLayout* form_;
    QPushButton* color_;
    QDoubleSpinBox *intensity_, *range_, *cone_, *softness_;
    bool updating_ = false;
};

// Edits the selected light or camera (the one selected alone). With the list
// shown it also lists them all.
class ScenePanel : public QWidget {
    Q_OBJECT
public:
    ScenePanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();
    // Without the list and the add buttons (inside the Properties panel).
    void setListVisible(bool on);

private:
    template <class Fn> void edit(const char* op, Fn fn);
    Id current() const;
    QDoubleSpinBox* number(double min, double max, double step, const QString& suffix);
    Document* doc_;
    Viewport* vp_;
    QListWidget* list_;
    QWidget* addRow_ = nullptr;
    QWidget* editor_;
    QFormLayout* form_;
    QLineEdit* name_;
    QCheckBox* enabled_;
    QPushButton* color_;
    QDoubleSpinBox* intensity_;
    QDoubleSpinBox* range_;
    QDoubleSpinBox* cone_;
    QDoubleSpinBox* softness_;
    QDoubleSpinBox* fov_;
    QCheckBox* perspective_;
    QDoubleSpinBox* pos_[3];
    QDoubleSpinBox* target_[3];
    QPushButton* lookThrough_;
    QPushButton* fromView_;
    QPushButton* erase_;
    bool updating_ = false;
};

} // namespace gs
