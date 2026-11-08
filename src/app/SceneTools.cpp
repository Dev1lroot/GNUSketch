// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneTools.h"

#include "Icons.h"
#include "Viewport.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace gs {

PlaceObjectTool::PlaceObjectTool(Viewport* vp, ToolId id) : Tool(vp), id_(id) {
    height_ = id == ToolId::PlaceDirectionalLight ? 5.0 : id == ToolId::PlaceCamera ? 1.6 : 2.0;
}

SceneObject::Kind PlaceObjectTool::kind() const {
    switch (id_) {
    case ToolId::PlaceSpotLight: return SceneObject::SpotLight;
    case ToolId::PlaceDirectionalLight: return SceneObject::DirectionalLight;
    case ToolId::PlaceCamera: return SceneObject::ViewCamera;
    default: return SceneObject::PointLight;
    }
}

QString PlaceObjectTool::status() const {
    if (aiming_) return QStringLiteral("Click the point it aims at. Esc cancels.");
    return QStringLiteral("Click a surface or the ground to place it %1 above that point; type a value for another height.").arg(fmtLen(height_));
}

void PlaceObjectTool::reset() {
    aiming_ = false;
    hasPoint_ = false;
    clearLocks();
    refreshStatus();
    update();
}

Vec3 PlaceObjectTool::standPoint() const {
    Vec3 up(0, 0, 1);
    if (inf_.face != kNone && inf_.face < (Id)vp_->doc().mesh().faces.size()) {
        const Vec3 n = vp_->doc().mesh().faces[inf_.face].normal;
        // Whichever side of the face the viewer is on.
        up = n.dot(vp_->camera().eye - inf_.p) >= 0 ? n : -n;
    }
    return inf_.p + up * height_;
}

void PlaceObjectTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    inf_ = inferPoint(e, aiming_ ? std::optional<Vec3>(pos_) : std::nullopt);
    hasPoint_ = inf_.type != InferenceResult::None;
    update();
}

void PlaceObjectTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    mouseMove(e);
    if (!hasPoint_) return;
    if (!aiming_) {
        pos_ = standPoint();
        if (kind() == SceneObject::PointLight) {
            place(pos_);
            return;
        }
        aiming_ = true;
        refreshStatus();
        update();
        return;
    }
    if ((inf_.p - pos_).length2() < 1e-6) return; // it needs a direction
    place(inf_.p);
}

void PlaceObjectTool::place(const Vec3& target) {
    SceneObject o = vp_->objectFromView(kind());
    o.pos = pos_;
    o.target = target;
    if (kind() == SceneObject::DirectionalLight) o.intensity = 0.5;
    doc().beginOp(std::string("Add ") + sceneObjectTypeName(kind()));
    const Id id = doc().addObject(std::move(o));
    doc().selection.clear();
    doc().selection.objects.insert(id);
    doc().commitOp();
    doc().notifySelection();
    aiming_ = false;
    // Next click selects again; not from here, setTool destroys this tool.
    QTimer::singleShot(0, vp_, [vp = vp_] { vp->setTool(ToolId::Select); });
}

bool PlaceObjectTool::vcbEnter(const QString& text) {
    if (aiming_) return false;
    double v;
    if (!parseLen(text, v)) return false;
    height_ = v;
    setVcb(fmtLen(height_));
    refreshStatus();
    return true;
}

void PlaceObjectTool::buildPreview(Preview& pv) {
    if (!hasPoint_) return;
    engine().addPreview(inf_, pv);
    const Color c = Color::rgb8(225, 150, 0);
    if (aiming_) {
        pv.line(pos_, inf_.p, c, 1.5f, Dash::Dashed);
        pv.markers.push_back({pos_, Preview::MarkerKind::Center});
    } else {
        const Vec3 p = standPoint();
        pv.line(inf_.p, p, c, 1.5f, Dash::Dashed);
        pv.markers.push_back({p, Preview::MarkerKind::Center});
    }
}

// ===========================================================================
// Panel
// ===========================================================================

namespace {
void setButtonColor(QPushButton* b, const Color& c) {
    QPixmap pm(28, 14);
    pm.fill(QColor::fromRgbF(c.r, c.g, c.b));
    QPainter p(&pm);
    p.setPen(QColor(80, 80, 80));
    p.drawRect(0, 0, 27, 13);
    b->setIcon(QIcon(pm));
}
void setSpin(QDoubleSpinBox* s, double v) {
    if (std::abs(s->value() - v) > 1e-9) s->setValue(v);
}
} // namespace

QDoubleSpinBox* ScenePanel::number(double min, double max, double step, const QString& suffix) {
    auto* s = new QDoubleSpinBox(editor_);
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setDecimals(2);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false); // one undo step per value, not per keystroke
    return s;
}

template <class Fn> void ScenePanel::edit(const char* op, Fn fn) {
    if (updating_) return;
    const Id id = current();
    if (id == kNone) return;
    doc_->beginOp(op);
    fn(doc_->state.objects[id]);
    doc_->commitOp();
    vp_->update();
}

Id ScenePanel::current() const {
    const auto* it = list_->currentItem();
    if (!it) return kNone;
    const Id id = it->data(Qt::UserRole).toInt();
    return id >= 0 && id < (Id)doc_->state.objects.size() && doc_->state.objects[id].alive ? id : kNone;
}

ScenePanel::ScenePanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    auto* addRow = new QHBoxLayout();
    struct Add { ToolId id; const char* tip; };
    for (const Add& a : {Add{ToolId::PlacePointLight, "Add a point light"}, Add{ToolId::PlaceSpotLight, "Add a spot light"},
                         Add{ToolId::PlaceDirectionalLight, "Add a directional light"}, Add{ToolId::PlaceCamera, "Place a camera"}}) {
        auto* b = new QToolButton(this);
        b->setIcon(Icons::get(Icons::toolIconName(a.id)));
        b->setIconSize(QSize(22, 22));
        b->setToolTip(QString::fromLatin1(a.tip));
        const ToolId id = a.id;
        connect(b, &QToolButton::clicked, this, [this, id] { vp_->setTool(id); });
        addRow->addWidget(b);
    }
    addRow->addStretch(1);
    lay->addLayout(addRow);

    list_ = new QListWidget(this);
    list_->setMaximumHeight(120);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(list_);

    editor_ = new QWidget(this);
    form_ = new QFormLayout(editor_);
    form_->setContentsMargins(0, 0, 0, 0);
    name_ = new QLineEdit(editor_);
    enabled_ = new QCheckBox(QStringLiteral("On"), editor_);
    color_ = new QPushButton(QStringLiteral("Color…"), editor_);
    intensity_ = number(0, 10000, 0.5, {});
    range_ = number(0.1, 100000, 1, QStringLiteral(" m"));
    cone_ = number(1, 170, 5, QStringLiteral("°"));
    softness_ = number(0, 1, 0.05, {});
    fov_ = number(1, 150, 5, QStringLiteral("°"));
    perspective_ = new QCheckBox(QStringLiteral("Perspective"), editor_);
    form_->addRow(QStringLiteral("Name:"), name_);
    form_->addRow(enabled_);
    form_->addRow(QStringLiteral("Color:"), color_);
    form_->addRow(QStringLiteral("Brightness:"), intensity_);
    form_->addRow(QStringLiteral("Range:"), range_);
    form_->addRow(QStringLiteral("Cone angle:"), cone_);
    form_->addRow(QStringLiteral("Softness:"), softness_);
    form_->addRow(QStringLiteral("Field of view:"), fov_);
    form_->addRow(perspective_);
    auto vecRow = [&](const QString& label, QDoubleSpinBox** out) {
        auto* row = new QWidget(editor_);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        for (int i = 0; i < 3; ++i) {
            out[i] = number(-1e6, 1e6, 0.1, {});
            out[i]->setButtonSymbols(QAbstractSpinBox::NoButtons);
            h->addWidget(out[i]);
        }
        form_->addRow(label, row);
    };
    vecRow(QStringLiteral("Position (m):"), pos_);
    vecRow(QStringLiteral("Aim at (m):"), target_);
    auto* btns = new QHBoxLayout();
    lookThrough_ = new QPushButton(QStringLiteral("Look Through"), editor_);
    fromView_ = new QPushButton(QStringLiteral("Set From View"), editor_);
    erase_ = new QPushButton(QStringLiteral("Delete"), editor_);
    btns->addWidget(lookThrough_);
    btns->addWidget(fromView_);
    btns->addWidget(erase_);
    form_->addRow(btns);
    lay->addWidget(editor_);

    connect(list_, &QListWidget::currentItemChanged, this, [this] {
        if (updating_) return;
        const Id id = current();
        doc_->selection.clear();
        if (id != kNone) doc_->selection.objects.insert(id);
        doc_->notifySelection();
        refresh();
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { vp_->lookThrough(current()); });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const std::string n = name_->text().trimmed().toStdString();
        const Id id = current();
        if (id == kNone || n.empty() || n == doc_->state.objects[id].name) return;
        edit("Rename", [&](SceneObject& o) { o.name = n; });
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) { edit("Switch Light", [&](SceneObject& o) { o.enabled = on; }); });
    connect(perspective_, &QCheckBox::toggled, this, [this](bool on) { edit("Camera Projection", [&](SceneObject& o) { o.perspective = on; }); });
    connect(color_, &QPushButton::clicked, this, [this] {
        const Id id = current();
        if (id == kNone) return;
        const Color c0 = doc_->state.objects[id].color;
        const QColor c = QColorDialog::getColor(QColor::fromRgbF(c0.r, c0.g, c0.b), this, QStringLiteral("Light Color"));
        if (c.isValid()) edit("Light Color", [&](SceneObject& o) { o.color = Color((float)c.redF(), (float)c.greenF(), (float)c.blueF()); });
    });
    auto bind = [this](QDoubleSpinBox* s, const char* op, double SceneObject::*field) {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, op, field](double v) { edit(op, [&](SceneObject& o) { o.*field = v; }); });
    };
    bind(intensity_, "Light Brightness", &SceneObject::intensity);
    bind(range_, "Light Range", &SceneObject::range);
    bind(cone_, "Spot Angle", &SceneObject::coneAngle);
    bind(softness_, "Spot Softness", &SceneObject::softness);
    bind(fov_, "Camera Field of View", &SceneObject::fov);
    for (int i = 0; i < 3; ++i) {
        connect(pos_[i], &QDoubleSpinBox::valueChanged, this, [this, i](double v) { edit("Move", [&](SceneObject& o) { o.pos[i] = v; }); });
        connect(target_[i], &QDoubleSpinBox::valueChanged, this, [this, i](double v) { edit("Aim", [&](SceneObject& o) { o.target[i] = v; }); });
    }
    connect(lookThrough_, &QPushButton::clicked, this, [this] { vp_->lookThrough(current()); });
    connect(fromView_, &QPushButton::clicked, this, [this] {
        const SceneObject v = vp_->objectFromView(SceneObject::ViewCamera);
        edit("Set Camera From View", [&](SceneObject& o) {
            o.pos = v.pos;
            o.target = v.target;
            o.fov = v.fov;
            o.perspective = v.perspective;
        });
    });
    connect(erase_, &QPushButton::clicked, this, [this] {
        edit("Erase", [](SceneObject& o) { o.alive = false; });
    });
    refresh();
}

void ScenePanel::refresh() {
    updating_ = true;
    const auto& objs = doc_->state.objects;
    Id want = doc_->selection.objects.size() == 1 ? *doc_->selection.objects.begin() : kNone;
    // Rebuild only when the list really changed: it keeps the scroll position.
    QStringList names;
    std::vector<Id> ids;
    for (Id i = 0; i < (Id)objs.size(); ++i)
        if (objs[i].alive) {
            names << QString::fromStdString(objs[i].name);
            ids.push_back(i);
        }
    bool same = list_->count() == names.size();
    for (int i = 0; same && i < names.size(); ++i)
        same = list_->item(i)->text() == names[i] && list_->item(i)->data(Qt::UserRole).toInt() == ids[i];
    if (!same) {
        list_->clear();
        for (int i = 0; i < names.size(); ++i) {
            auto* it = new QListWidgetItem(names[i], list_);
            it->setData(Qt::UserRole, ids[i]);
            const ToolId tool = objs[ids[i]].kind == SceneObject::SpotLight ? ToolId::PlaceSpotLight
                              : objs[ids[i]].kind == SceneObject::DirectionalLight ? ToolId::PlaceDirectionalLight
                              : objs[ids[i]].kind == SceneObject::ViewCamera ? ToolId::PlaceCamera : ToolId::PlacePointLight;
            it->setIcon(Icons::get(Icons::toolIconName(tool)));
        }
    }
    list_->setCurrentRow(-1);
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toInt() == want) list_->setCurrentRow(i);

    const Id id = current();
    editor_->setVisible(id != kNone);
    if (id != kNone) {
        const SceneObject& o = objs[id];
        if (!name_->hasFocus()) name_->setText(QString::fromStdString(o.name));
        enabled_->setChecked(o.enabled);
        setButtonColor(color_, o.color);
        setSpin(intensity_, o.intensity);
        setSpin(range_, o.range);
        setSpin(cone_, o.coneAngle);
        setSpin(softness_, o.softness);
        setSpin(fov_, o.fov);
        perspective_->setChecked(o.perspective);
        for (int i = 0; i < 3; ++i) {
            setSpin(pos_[i], o.pos[i]);
            setSpin(target_[i], o.target[i]);
        }
        const bool light = o.isLight(), distance = o.kind == SceneObject::PointLight || o.kind == SceneObject::SpotLight;
        auto show = [&](QWidget* w, bool on) {
            w->setVisible(on);
            if (QWidget* label = form_->labelForField(w)) label->setVisible(on);
        };
        show(enabled_, light);
        show(color_, light);
        show(intensity_, light);
        show(range_, distance);
        show(cone_, o.kind == SceneObject::SpotLight);
        show(softness_, o.kind == SceneObject::SpotLight);
        show(fov_, !light);
        show(perspective_, !light);
        show(target_[0]->parentWidget(), o.aims());
        lookThrough_->setVisible(!light);
        fromView_->setVisible(!light);
    }
    updating_ = false;
}

} // namespace gs
