// SPDX-License-Identifier: GPL-3.0-or-later
#include "Properties.h"

#include "Panels.h"
#include "ModifyTools.h"
#include "SceneTools.h"
#include "Viewport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSettings>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace gs {

namespace {

bool isTerraform(ToolId id) {
    return id == ToolId::Mountain || id == ToolId::Pit || id == ToolId::Flatten || id == ToolId::TextureBrush;
}

bool isLightTool(ToolId id) {
    return id == ToolId::PlacePointLight || id == ToolId::PlaceSpotLight || id == ToolId::PlaceDirectionalLight;
}

} // namespace

UnitsSettings::UnitsSettings(Document* doc, bool showUnit, QWidget* parent) : QWidget(parent), doc_(doc) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(8, 6, 8, 8);
    if (showUnit) {
        unit_ = new QComboBox(this);
        unit_->setObjectName(QStringLiteral("UnitCombo"));
        unit_->addItems({QStringLiteral("Millimeters"), QStringLiteral("Centimeters"), QStringLiteral("Meters"), QStringLiteral("Inches"),
                         QStringLiteral("Feet and inches")});
        form->addRow(QStringLiteral("Length unit:"), unit_);
        connect(unit_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (updating_) return;
            // A new unit starts with its usual decimals.
            doc_->units.unit = (LengthUnit)i;
            doc_->units.precision = Units::defaultPrecision((LengthUnit)i);
            refresh();
            apply();
        });
    }
    precision_ = new QComboBox(this);
    precision_->setObjectName(QStringLiteral("PrecisionCombo"));
    precision_->setToolTip(QStringLiteral("How lengths are shown (and the step of \"To display precision\" snapping)"));
    form->addRow(QStringLiteral("Display precision:"), precision_);
    snap_ = new QComboBox(this);
    snap_->setObjectName(QStringLiteral("SnapCombo"));
    snap_->addItems({QStringLiteral("Off"), QStringLiteral("To display precision"), QStringLiteral("Every step:")});
    snap_->setToolTip(QStringLiteral("Lengths drawn freely (lines, rectangles, circles, push/pull, move, offset, tape) become whole steps"));
    form->addRow(QStringLiteral("Length snapping:"), snap_);
    step_ = new QLineEdit(this);
    step_->setObjectName(QStringLiteral("SnapStep"));
    step_->setToolTip(QStringLiteral("Any length: 1m, 10cm, 5mm, 1/16\", 6\""));
    stepLabel_ = new QLabel(QStringLiteral("Step:"), this);
    form->addRow(stepLabel_, step_);
    connect(precision_, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(snap_, &QComboBox::currentIndexChanged, this, [this] { apply(); });
    connect(step_, &QLineEdit::editingFinished, this, [this] { apply(); });
    refresh();
}

void UnitsSettings::fillPrecision() {
    precision_->clear();
    Units sample = doc_->units;
    for (int p = 0; p <= 6; ++p) {
        sample.precision = p;
        precision_->addItem(QString::fromStdString(sample.format(0)), p);
    }
}

void UnitsSettings::refresh() {
    updating_ = true;
    const Units& u = doc_->units;
    if (unit_) unit_->setCurrentIndex((int)u.unit);
    fillPrecision();
    precision_->setCurrentIndex(std::clamp(u.precision, 0, 6));
    snap_->setCurrentIndex((int)u.snap);
    if (!step_->hasFocus()) step_->setText(QString::fromStdString(u.format(u.snapStep)));
    step_->setVisible(u.snap == Units::Snap::Step);
    stepLabel_->setVisible(u.snap == Units::Snap::Step);
    updating_ = false;
}

void UnitsSettings::apply() {
    if (updating_) return;
    Units& u = doc_->units;
    u.precision = precision_->currentData().toInt();
    u.snap = (Units::Snap)snap_->currentIndex();
    double step;
    if (u.parseLength(step_->text().toStdString(), step) && step > 1e-9) u.snapStep = step;
    remember(u);
    refresh();
    emit changed();
}

void UnitsSettings::remember(const Units& u) {
    QSettings s;
    s.setValue("model/units", (int)u.unit);
    s.setValue("model/precision", u.precision);
    s.setValue("model/snapMode", (int)u.snap);
    s.setValue("model/snapStep", u.snapStep);
}

void UnitsSettings::loadDefaults(Units& u) {
    QSettings s;
    u.unit = (LengthUnit)std::clamp(s.value("model/units", (int)LengthUnit::Millimeters).toInt(), 0, 4);
    u.precision = std::clamp(s.value("model/precision", Units::defaultPrecision(u.unit)).toInt(), 0, 6);
    u.snap = (Units::Snap)std::clamp(s.value("model/snapMode", 0).toInt(), 0, 2);
    u.snapStep = std::max(1e-6, s.value("model/snapStep", 0.01).toDouble());
}

ScaleSettings::ScaleSettings(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(8, 6, 8, 8);
    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setProperty("hint", true);
    form->addRow(note_);
    const char* names[3] = {"W (red):", "L (green):", "H (blue):"};
    for (int k = 0; k < 3; ++k) {
        fields_[k] = new QLineEdit(this);
        fields_[k]->setObjectName(QStringLiteral("ScaleSize%1").arg(k));
        fields_[k]->setToolTip(QStringLiteral("Type a size (any unit: 120, 12cm, 1.2m, 4'), then Enter"));
        form->addRow(QString::fromLatin1(names[k]), fields_[k]);
        connect(fields_[k], &QLineEdit::editingFinished, this, [this, k] {
            if (!updating_ && fields_[k]->isModified()) enter(k, fields_[k]->text());
        });
    }
    keep_ = new QCheckBox(QStringLiteral("Keep proportions"), this);
    keep_->setToolTip(QStringLiteral("Changing one size changes the other two by the same ratio"));
    keep_->setChecked(QSettings().value("scale/keepProportions", true).toBool());
    connect(keep_, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("scale/keepProportions", on); });
    form->addRow(keep_);
    anchor_ = new QComboBox(this);
    anchor_->addItems({QStringLiteral("Bottom centre"), QStringLiteral("Corner (min)"), QStringLiteral("Centre")});
    anchor_->setToolTip(QStringLiteral("The point of the box that stays where it is"));
    anchor_->setCurrentIndex(std::clamp(QSettings().value("scale/anchor", 0).toInt(), 0, 2));
    connect(anchor_, &QComboBox::currentIndexChanged, this, [](int i) { QSettings().setValue("scale/anchor", i); });
    form->addRow(QStringLiteral("Keep in place:"), anchor_);
    refresh();
}

bool ScaleSettings::proportional() const { return keep_->isChecked(); }
void ScaleSettings::setProportional(bool on) { keep_->setChecked(on); }

void ScaleSettings::refresh() {
    updating_ = true;
    auto* tool = dynamic_cast<ScaleTool*>(vp_->tool());
    Vec3 l;
    const bool have = tool && tool->boxLengths(l);
    for (int k = 0; k < 3; ++k) {
        fields_[k]->setEnabled(have && l[k] > kTol);
        if (!fields_[k]->hasFocus() || !fields_[k]->isModified())
            fields_[k]->setText(have ? QString::fromStdString(doc_->units.format(l[k])) : QString());
        fields_[k]->setModified(false);
    }
    note_->setText(have ? QStringLiteral("Sizes of the selection's box. Type a new one to rescale it.")
                        : QStringLiteral("Select something to see its sizes and set new ones."));
    updating_ = false;
}

bool ScaleSettings::enter(int axis, const QString& text) {
    auto* tool = dynamic_cast<ScaleTool*>(vp_->tool());
    Vec3 l;
    double v;
    if (!tool || axis < 0 || axis > 2 || !tool->boxLengths(l) || l[axis] <= kTol || !doc_->units.parseLength(text.toStdString(), v) || v <= kTol) {
        refresh();
        return false;
    }
    Vec3 want = l;
    if (keep_->isChecked()) {
        const double r = v / l[axis];
        want = l * r;
    } else {
        want[axis] = v;
    }
    const bool ok = tool->setDimensions(want, (ScaleTool::Anchor)anchor_->currentIndex());
    refresh();
    return ok;
}

PropertiesPanel::PropertiesPanel(Document* doc, Viewport* vp, EntityInfoPanel* info, ScenePanel* scene, TerrainPanel* terrain,
                                 PlacementSettings* placement, QWidget* parent)
    : QWidget(parent), doc_(doc), vp_(vp), info_(info), scene_(scene), terrain_(terrain), placement_(placement) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("SectionHeader"));
    hint_ = new QLabel(this);
    hint_->setWordWrap(true);
    hint_->setProperty("hint", true);
    scale_ = new ScaleSettings(doc, vp, this);
    units_ = new UnitsSettings(doc, false, this);
    connect(units_, &UnitsSettings::changed, this, [this] { scale_->refresh(); info_->refresh(); });
    entityBtn_ = new QPushButton(QStringLiteral("Game Properties…"), this);
    entityBtn_->setToolTip(QStringLiteral("How the component behaves in Game Mode: door, button, script, physics"));
    connect(entityBtn_, &QPushButton::clicked, this, &PropertiesPanel::entityPropertiesRequested);
    scene_->setListVisible(false);
    for (QWidget* w : {static_cast<QWidget*>(title_), static_cast<QWidget*>(hint_), static_cast<QWidget*>(terrain_),
                       static_cast<QWidget*>(placement_), static_cast<QWidget*>(scale_), static_cast<QWidget*>(units_), static_cast<QWidget*>(scene_), static_cast<QWidget*>(info_),
                       static_cast<QWidget*>(entityBtn_)}) {
        w->setParent(this);
        lay->addWidget(w);
    }
    lay->addStretch(1);
    refresh();
}

void PropertiesPanel::setTool(ToolId id, const QString& name) {
    tool_ = id;
    toolName_ = QString(name).remove('&');
    refresh();
}

void PropertiesPanel::refresh() {
    const Selection& s = doc_->selection;
    const bool terraform = isTerraform(tool_), light = isLightTool(tool_), scale = tool_ == ToolId::Scale;
    // The tools that draw lengths show the snapping and precision settings
    // (above what is selected).
    const bool lengths = tool_ == ToolId::Line || tool_ == ToolId::Rectangle || tool_ == ToolId::Circle || tool_ == ToolId::Polygon ||
                         tool_ == ToolId::Arc || tool_ == ToolId::PushPull || tool_ == ToolId::Move || tool_ == ToolId::Offset ||
                         tool_ == ToolId::Tape;
    units_->setVisible(lengths || scale);
    if (lengths || scale) units_->refresh();
    terrain_->setVisible(terraform);
    placement_->setVisible(light);
    scale_->setVisible(scale);
    if (scale) scale_->refresh();
    if (terraform || light || scale) {
        // The equipped tool's settings.
        showing_ = QStringLiteral("tool");
        title_->setText(toolName_);
        hint_->hide();
        scene_->hide();
        info_->hide();
        entityBtn_->hide();
        if (terraform) terrain_->refresh();
        if (light) {
            placement_->setKind(tool_ == ToolId::PlaceSpotLight ? SceneObject::SpotLight
                                : tool_ == ToolId::PlaceDirectionalLight ? SceneObject::DirectionalLight
                                                                          : SceneObject::PointLight);
        }
        return;
    }
    showing_ = QStringLiteral("selection");
    const bool oneObject = s.objects.size() == 1 && s.faces.empty() && s.edges.empty() && s.instances.empty();
    scene_->setVisible(oneObject);
    info_->setVisible(!oneObject);
    entityBtn_->setVisible(s.instances.size() == 1 && s.faces.empty() && s.edges.empty());
    if (oneObject) {
        const SceneObject& o = doc_->state.objects[*s.objects.begin()];
        title_->setText(QString::fromLatin1(sceneObjectTypeName(o.kind)));
        scene_->refresh();
    } else {
        title_->setText(s.empty() ? toolName_ : QStringLiteral("Selection"));
        info_->refresh();
    }
    hint_->setVisible(s.empty());
    hint_->setText(QStringLiteral("Select something to see and change its properties. The terraform brushes and the light tools show their settings here."));
}

} // namespace gs
