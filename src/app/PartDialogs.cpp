// SPDX-License-Identifier: GPL-3.0-or-later
#include "PartDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace forma {

namespace {

QString mm(double v) { return QLocale::c().toString(v, 'g', 6); }

// An editable list of standard values (millimetres or degrees): any number
// typed in works too. value() is NaN when the text is not a number.
class PresetCombo : public QComboBox {
public:
    PresetCombo(const std::vector<double>& presets, double current, const QString& objectName, QWidget* parent)
        : QComboBox(parent) {
        setObjectName(objectName);
        setEditable(true);
        setInsertPolicy(QComboBox::NoInsert);
        for (double v : presets) addItem(mm(v));
        setValue(current);
    }
    double value() const {
        bool ok = false;
        const double v = QLocale::c().toDouble(currentText().trimmed().replace(QLatin1Char(','), QLatin1Char('.')), &ok);
        return ok ? v : std::nan("");
    }
    void setValue(double v) {
        const int i = findText(mm(v));
        if (i >= 0) setCurrentIndex(i);
        else setEditText(mm(v));
    }
};

QDoubleSpinBox* mmBox(QWidget* parent, const char* name, double value, double lo, double hi, int decimals = 2) {
    auto* sb = new QDoubleSpinBox(parent);
    sb->setObjectName(QString::fromUtf8(name));
    sb->setDecimals(decimals);
    sb->setRange(lo, hi);
    sb->setSuffix(QStringLiteral(" mm"));
    sb->setValue(value);
    return sb;
}

QComboBox* sizeCombo(QWidget* parent, int current) {
    auto* c = new QComboBox(parent);
    c->setObjectName(QStringLiteral("partSize"));
    for (const MetricSize& s : metricSizes()) c->addItem(QString::fromUtf8(s.name));
    c->setCurrentIndex(std::clamp(current, 0, c->count() - 1));
    return c;
}

QDialogButtonBox* placeButtons(QDialog& dlg) {
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Place"));
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    return bb;
}

bool fastenerDialog(QWidget* parent, PartFamily family, FastenerParams& out) {
    QSettings settings;
    settings.beginGroup(family == PartFamily::Screw ? QStringLiteral("part_screw")
                        : family == PartFamily::Nut ? QStringLiteral("part_nut")
                                                    : QStringLiteral("part_washer"));
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("PartDialog"));
    dlg.setWindowTitle(family == PartFamily::Screw ? QStringLiteral("Screw") : family == PartFamily::Nut ? QStringLiteral("Nut")
                                                                                                         : QStringLiteral("Washer"));
    auto* fl = new QFormLayout(&dlg);

    std::vector<FastenerKind> kinds;
    if (family == PartFamily::Screw)
        kinds = {FastenerKind::HexBolt, FastenerKind::SocketCap, FastenerKind::Countersunk, FastenerKind::ThreadedRod};
    else if (family == PartFamily::Nut) kinds = {FastenerKind::HexNut, FastenerKind::SquareNut};
    else kinds = {FastenerKind::Washer};
    auto* kind = new QComboBox(&dlg);
    kind->setObjectName(QStringLiteral("partType"));
    for (FastenerKind k : kinds)
        kind->addItem(QStringLiteral("%1 (%2)").arg(QString::fromUtf8(fastenerKindName(k)), QString::fromUtf8(fastenerStandard(k))),
                      int(k));
    kind->setCurrentIndex(std::max(0, kind->findData(settings.value("kind", int(kinds[0])).toInt())));
    if (kinds.size() > 1) fl->addRow(QStringLiteral("Type:"), kind);

    auto* size = sizeCombo(&dlg, settings.value("size", metricSizeIndex("M6")).toInt());
    fl->addRow(QStringLiteral("Size:"), size);
    auto* length = new PresetCombo(standardScrewLengths(), settings.value("length", 20.0).toDouble(), QStringLiteral("partLength"), &dlg);
    auto* lengthLabel = new QLabel(QStringLiteral("Length (mm):"), &dlg);
    fl->addRow(lengthLabel, length);
    auto* pitch = new QComboBox(&dlg);
    pitch->setObjectName(QStringLiteral("partPitch"));
    fl->addRow(QStringLiteral("Pitch:"), pitch);
    auto* thread = new QCheckBox(QStringLiteral("Model the thread (off: a plain shank or hole, far fewer polygons)"), &dlg);
    thread->setObjectName(QStringLiteral("partThread"));
    thread->setChecked(settings.value("thread", true).toBool());
    fl->addRow(QString(), thread);
    auto* leftHand = new QCheckBox(QStringLiteral("Left-hand thread"), &dlg);
    leftHand->setObjectName(QStringLiteral("partLeftHand"));
    leftHand->setChecked(settings.value("leftHand", false).toBool());
    fl->addRow(QString(), leftHand);
    auto* segments = new QSpinBox(&dlg);
    segments->setObjectName(QStringLiteral("partSegments"));
    segments->setRange(8, 256);
    segments->setValue(settings.value("segments", 36).toInt());
    segments->setToolTip(QStringLiteral("Pieces around round surfaces. More is smoother but makes more polygons."));
    fl->addRow(QStringLiteral("Segments:"), segments);
    auto* info = new QLabel(&dlg);
    info->setObjectName(QStringLiteral("partInfo"));
    info->setWordWrap(true);
    info->setMinimumWidth(340);
    fl->addRow(info);
    auto* bb = placeButtons(dlg);
    fl->addRow(bb);
    pitch->addItem(QString(), false);
    pitch->addItem(QString(), true);
    pitch->setCurrentIndex(settings.value("fine", false).toBool() ? 1 : 0);

    auto params = [&] {
        FastenerParams p;
        p.kind = FastenerKind(kind->currentData().toInt());
        p.size = size->currentIndex();
        p.length = length->value() * 0.001;
        p.finePitch = pitch->currentIndex() == 1;
        p.thread = thread->isChecked();
        p.leftHand = leftHand->isChecked();
        p.segments = segments->value();
        return p;
    };
    auto refresh = [&] {
        const FastenerParams p = params();
        const MetricSize& s = metricSizes()[p.size];
        pitch->setItemText(0, QStringLiteral("Coarse (%1 mm)").arg(mm(s.pitch)));
        pitch->setItemText(1, QStringLiteral("Fine (%1 mm)").arg(mm(s.finePitch)));
        const bool hasLength = fastenerHasLength(p.kind), hasThread = fastenerHasThread(p.kind);
        fl->setRowVisible(length, hasLength);
        fl->setRowVisible(pitch, hasThread);
        fl->setRowVisible(thread, hasThread);
        fl->setRowVisible(leftHand, hasThread && p.thread);
        lengthLabel->setText(p.kind == FastenerKind::Countersunk ? QStringLiteral("Length, overall (mm):")
                             : p.kind == FastenerKind::ThreadedRod ? QStringLiteral("Length (mm):")
                                                                    : QStringLiteral("Length under head (mm):"));
        QString dims;
        switch (p.kind) {
        case FastenerKind::HexBolt: dims = QStringLiteral("head %1 across flats × %2").arg(mm(s.nutS), mm(s.headK)); break;
        case FastenerKind::SocketCap: dims = QStringLiteral("head Ø %1 × %2, hex key %3").arg(mm(s.capDk), mm(s.d), mm(s.capKey)); break;
        case FastenerKind::Countersunk: dims = QStringLiteral("head Ø %1 at 90°, hex key %2").arg(mm(s.cskDk), mm(s.cskKey)); break;
        case FastenerKind::ThreadedRod: dims = QStringLiteral("threaded all along"); break;
        case FastenerKind::HexNut: dims = QStringLiteral("%1 across flats × %2 high").arg(mm(s.nutS), mm(s.nutM)); break;
        case FastenerKind::SquareNut: dims = QStringLiteral("%1 square × %2 high").arg(mm(s.sqS), mm(s.sqM)); break;
        case FastenerKind::Washer: dims = QStringLiteral("Ø %1 / %2 × %3").arg(mm(s.washD1), mm(s.washD2), mm(s.washH)); break;
        }
        Mesh m;
        std::string name, err;
        const bool ok = !(hasLength && std::isnan(length->value())) && buildFastener(p, m, &name, &err);
        if (ok)
            info->setText(QStringLiteral("<b>%1</b> — %2<br>%3%4<br>%5 faces")
                              .arg(QString::fromStdString(name), QString::fromUtf8(fastenerStandard(p.kind)), dims,
                                   hasThread ? QStringLiteral(", pitch %1 mm, thread depth %2 mm")
                                                   .arg(mm(p.finePitch ? s.finePitch : s.pitch))
                                                   .arg(metricThreadDepth(p.finePitch ? s.finePitch : s.pitch), 0, 'f', 3)
                                             : QString())
                              .arg(m.aliveFaceCount()));
        else info->setText(err.empty() ? QStringLiteral("Type a length in millimetres.") : QString::fromStdString(err));
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        dlg.adjustSize();
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(size, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(pitch, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(length, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(thread, &QCheckBox::toggled, &dlg, refresh);
    QObject::connect(leftHand, &QCheckBox::toggled, &dlg, refresh);
    QObject::connect(segments, &QSpinBox::valueChanged, &dlg, refresh);
    refresh();
    if (dlg.exec() != QDialog::Accepted) return false;
    out = params();
    settings.setValue("kind", int(out.kind));
    settings.setValue("size", out.size);
    if (fastenerHasLength(out.kind)) settings.setValue("length", out.length * 1000);
    settings.setValue("fine", out.finePitch);
    settings.setValue("thread", out.thread);
    settings.setValue("leftHand", out.leftHand);
    settings.setValue("segments", out.segments);
    return true;
}

bool gearDialog(QWidget* parent, PartFamily family, PartChoice& choice, const std::vector<GearRef>& allGears) {
    const bool rack = family == PartFamily::Rack;
    // A rack meshes with spur gears only.
    std::vector<GearRef> gears;
    for (const GearRef& g : allGears)
        if (g.params.kind == GearKind::Spur || (!rack && g.params.kind == GearKind::Rack)) gears.push_back(g);
    QSettings settings;
    settings.beginGroup(rack ? QStringLiteral("part_rack") : QStringLiteral("part_gear"));
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("PartDialog"));
    dlg.setWindowTitle(rack ? QStringLiteral("Gear Rack") : QStringLiteral("Spur Gear"));
    auto* fl = new QFormLayout(&dlg);

    // ---- what it fits ------------------------------------------------------------
    auto* fit = new QComboBox(&dlg);
    fit->setObjectName(QStringLiteral("gearFitTo"));
    fit->addItem(QStringLiteral("Nothing: start a new gear set"));
    int selected = 0;
    for (size_t i = 0; i < gears.size(); ++i) {
        fit->addItem(QString::fromStdString(gears[i].name) +
                     (gears[i].instance != kNone ? QStringLiteral(" (selected)") : QString()));
        if (gears[i].instance != kNone && !selected) selected = int(i) + 1;
    }
    fit->setCurrentIndex(selected);
    fit->setToolTip(QStringLiteral("Gears with the same tooth size (module) and pressure angle mesh. Fitting to a gear "
                                   "in the model takes both from it, so only the size is left to choose."));
    fl->addRow(QStringLiteral("Fit to:"), fit);
    fl->setRowVisible(fit, !gears.empty());
    auto* beside = new QCheckBox(rack ? QStringLiteral("Place it under the selected gear, meshing")
                                      : QStringLiteral("Place it next to the selected gear, meshing"),
                                 &dlg);
    beside->setObjectName(QStringLiteral("gearPlaceMeshing"));
    beside->setChecked(true);
    beside->setToolTip(QStringLiteral("Off: it comes on the cursor like any part."));
    fl->addRow(QString(), beside);

    auto* mode = new QComboBox(&dlg);
    mode->setObjectName(QStringLiteral("gearMode"));
    mode->addItem(rack ? QStringLiteral("Length (the number of teeth follows)") : QStringLiteral("Size (the number of teeth follows)"));
    mode->addItem(QStringLiteral("Module and number of teeth (engineering)"));
    mode->setCurrentIndex(std::clamp(settings.value("mode", 0).toInt(), 0, 1));
    fl->addRow(QStringLiteral("Choose by:"), mode);

    auto* module = new PresetCombo(standardModules(), settings.value("module", 1.0).toDouble(), QStringLiteral("gearModule"), &dlg);
    module->setToolTip(QStringLiteral("The tooth size (ISO 54 modules): teeth are π × module apart and a gear's pitch "
                                      "diameter is module × teeth. Keep it the same for gears that mesh."));
    fl->addRow(QStringLiteral("Tooth size, module (mm):"), module);
    auto* diameter = mmBox(&dlg, rack ? "gearLength" : "gearDiameter",
                           settings.value(rack ? "length" : "diameter", rack ? 31.4 : 22.0).toDouble(), 0.1, 100000);
    diameter->setToolTip(rack ? QStringLiteral("About this long: rounded to whole teeth.")
                              : QStringLiteral("About this wide across the tips: rounded to whole teeth, so it keeps meshing."));
    fl->addRow(rack ? QStringLiteral("Length:") : QStringLiteral("Diameter:"), diameter);
    auto* teeth = new QSpinBox(&dlg);
    teeth->setObjectName(QStringLiteral("gearTeeth"));
    teeth->setRange(rack ? 1 : 6, rack ? 2000 : 1000);
    teeth->setValue(settings.value("teeth", rack ? 10 : 20).toInt());
    fl->addRow(QStringLiteral("Teeth:"), teeth);
    auto* angle = new PresetCombo({20, 14.5, 25}, settings.value("angle", 20.0).toDouble(), QStringLiteral("gearPressureAngle"), &dlg);
    fl->addRow(QStringLiteral("Pressure angle (°):"), angle);
    auto* width = mmBox(&dlg, "gearFaceWidth", settings.value("faceWidth", 8.0).toDouble(), 0.01, 10000);
    fl->addRow(QStringLiteral("Thickness:"), width);
    auto* bore = mmBox(&dlg, "gearBore", settings.value("bore", 5.0).toDouble(), 0, 10000);
    bore->setSpecialValueText(QStringLiteral("None"));
    bore->setToolTip(QStringLiteral("The hole for the shaft."));
    fl->addRow(QStringLiteral("Shaft hole:"), bore);
    auto* height = mmBox(&dlg, "gearRackHeight", settings.value("rackHeight", 0.0).toDouble(), 0, 10000);
    height->setSpecialValueText(QStringLiteral("3 × module"));
    fl->addRow(QStringLiteral("Height to pitch line:"), height);
    auto* backlash = mmBox(&dlg, "gearBacklash", settings.value("backlash", 0.0).toDouble(), 0, 100, 3);
    backlash->setToolTip(QStringLiteral("Taken off each tooth's thickness so meshing gears do not bind (0 = exact profile)."));
    fl->addRow(QStringLiteral("Backlash:"), backlash);
    auto* steps = new QSpinBox(&dlg);
    steps->setObjectName(QStringLiteral("gearFlankSteps"));
    steps->setRange(1, 64);
    steps->setValue(settings.value("flankSteps", 6).toInt());
    fl->addRow(QStringLiteral("Flank pieces:"), steps);
    auto* info = new QLabel(&dlg);
    info->setObjectName(QStringLiteral("partInfo"));
    info->setWordWrap(true);
    info->setMinimumWidth(360);
    fl->addRow(info);
    auto* bb = placeButtons(dlg);
    fl->addRow(bb);

    auto target = [&]() -> const GearRef* { return fit->currentIndex() > 0 ? &gears[fit->currentIndex() - 1] : nullptr; };
    auto bySize = [&] { return mode->currentIndex() == 0; };
    auto teethNow = [&](double m) {
        if (!bySize()) return teeth->value();
        const double v = diameter->value() * 0.001;
        return rack ? rackTeethForLength(m, v) : gearTeethForDiameter(m, v - 2 * m);
    };
    auto params = [&] {
        GearParams p;
        p.kind = rack ? GearKind::Rack : GearKind::Spur;
        const GearRef* t = target();
        p.module = t ? t->params.module : module->value() * 0.001;
        p.pressureAngle = t ? t->params.pressureAngle : angle->value();
        p.teeth = teethNow(p.module);
        p.faceWidth = width->value() * 0.001;
        p.bore = bore->value() * 0.001;
        p.rackHeight = height->value() * 0.001;
        p.backlash = backlash->value() * 0.001;
        p.flankSteps = steps->value();
        return p;
    };
    auto layoutRows = [&] {
        const bool size = bySize(), fitted = target() != nullptr;
        fl->setRowVisible(beside, fitted && target()->instance != kNone);
        fl->setRowVisible(diameter, size);
        fl->setRowVisible(teeth, !size);
        // The finer points are for whoever chose to give the teeth.
        fl->setRowVisible(angle, !size);
        fl->setRowVisible(backlash, !size);
        fl->setRowVisible(steps, !size && !rack);
        fl->setRowVisible(bore, !rack);
        fl->setRowVisible(height, rack && !size);
        module->setEnabled(!fitted);
        angle->setEnabled(!fitted);
    };
    auto refresh = [&] {
        layoutRows();
        const GearParams p = params();
        if (const GearRef* t = target()) {
            QSignalBlocker b1(module), b2(angle);
            module->setValue(t->params.module * 1000);
            angle->setValue(t->params.pressureAngle);
        }
        Mesh m;
        std::string name, err;
        GearInfo gi;
        const bool ok = !std::isnan(p.module) && !std::isnan(p.pressureAngle) && buildGear(p, m, &name, &gi, &err);
        QString text;
        if (!ok) text = err.empty() ? QStringLiteral("Type a number.") : QString::fromStdString(err);
        else if (rack)
            text = QStringLiteral("<b>%1</b><br>%2 teeth, %3 mm long (teeth %4 mm apart)<br>%5 faces")
                       .arg(QString::fromStdString(name))
                       .arg(p.teeth)
                       .arg(gi.length * 1000, 0, 'f', 2)
                       .arg(gi.pitch * 1000, 0, 'f', 3)
                       .arg(m.aliveFaceCount());
        else
            text = QStringLiteral("<b>%1</b><br>%2 teeth, Ø %3 mm across the tips<br>"
                                  "pitch Ø %4 mm, root Ø %5 mm, base Ø %6 mm<br>%7%8 faces")
                       .arg(QString::fromStdString(name))
                       .arg(p.teeth)
                       .arg(gi.outsideDiameter * 1000, 0, 'f', 2)
                       .arg(gi.pitchDiameter * 1000, 0, 'f', 2)
                       .arg(gi.rootDiameter * 1000, 0, 'f', 2)
                       .arg(gi.baseDiameter * 1000, 0, 'f', 2)
                       .arg(gi.undercut ? QStringLiteral("Few teeth for this pressure angle: a cut gear would be "
                                                         "undercut (not modelled). ")
                                        : QString())
                       .arg(m.aliveFaceCount());
        if (ok) {
            if (const GearRef* t = target()) {
                const QString other = QString::fromStdString(t->name);
                if (t->params.kind == GearKind::Spur && !rack)
                    text += QStringLiteral("<br>Meshes with %1: centres %2 mm apart, turns %3× as fast")
                                .arg(other)
                                .arg(gearCentreDistance(p.module, t->params.teeth, p.teeth) * 1000, 0, 'f', 2)
                                .arg(double(t->params.teeth) / p.teeth, 0, 'g', 4);
                else if (t->params.kind == GearKind::Spur)
                    text += QStringLiteral("<br>Meshes with %1: its pitch line %2 mm from the gear's centre, "
                                           "moving %3 mm per turn")
                                .arg(other)
                                .arg(t->params.module * t->params.teeth / 2 * 1000, 0, 'f', 2)
                                .arg(kPi * t->params.module * t->params.teeth * 1000, 0, 'f', 2);
                else
                    text += QStringLiteral("<br>Meshes with %1: its centre %2 mm above the rack's pitch line, "
                                           "%3 mm along it per turn")
                                .arg(other)
                                .arg(p.module * p.teeth / 2 * 1000, 0, 'f', 2)
                                .arg(kPi * p.module * p.teeth * 1000, 0, 'f', 2);
            } else {
                text += QStringLiteral("<br>Meshes with any gear or rack of module %1 and %2°.")
                            .arg(p.module * 1000, 0, 'g', 4)
                            .arg(p.pressureAngle, 0, 'g', 4);
            }
        }
        info->setText(text);
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        dlg.adjustSize();
    };
    // Switching modes keeps the gear: the size follows the teeth and back.
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg, [&](int i) {
        const double m = params().module;
        if (!std::isnan(m)) {
            QSignalBlocker b1(teeth), b2(diameter);
            if (i == 0) diameter->setValue((rack ? kPi * m * teeth->value() : m * (teeth->value() + 2)) * 1000);
            else {
                const double v = diameter->value() * 0.001;
                teeth->setValue(rack ? rackTeethForLength(m, v) : gearTeethForDiameter(m, v - 2 * m));
            }
        }
        refresh();
    });
    // A gear to fit to brings its thickness along (still changeable).
    QObject::connect(fit, &QComboBox::currentIndexChanged, &dlg, [&] {
        if (const GearRef* t = target()) {
            QSignalBlocker b(width);
            width->setValue(t->params.faceWidth * 1000);
        }
        refresh();
    });
    QObject::connect(module, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(angle, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(teeth, &QSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(steps, &QSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(beside, &QCheckBox::toggled, &dlg, refresh);
    for (QDoubleSpinBox* sb : {diameter, width, bore, height, backlash}) QObject::connect(sb, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    if (target()) width->setValue(target()->params.faceWidth * 1000);
    refresh();
    if (dlg.exec() != QDialog::Accepted) return false;
    GearParams& out = choice.gearParams;
    out = params();
    if (const GearRef* t = target(); t && t->instance != kNone && beside->isChecked()) {
        choice.meshWith = t->instance;
        choice.meshParams = t->params;
    }
    settings.setValue("mode", mode->currentIndex());
    if (!target()) {
        settings.setValue("module", out.module * 1000);
        settings.setValue("angle", out.pressureAngle);
    }
    settings.setValue("teeth", out.teeth);
    settings.setValue(rack ? "length" : "diameter", diameter->value());
    settings.setValue("faceWidth", out.faceWidth * 1000);
    if (!rack) settings.setValue("bore", out.bore * 1000);
    else settings.setValue("rackHeight", out.rackHeight * 1000);
    settings.setValue("backlash", out.backlash * 1000);
    settings.setValue("flankSteps", out.flankSteps);
    return true;
}

bool bevelDialog(QWidget* parent, PartChoice& choice, const std::vector<GearRef>& allGears) {
    std::vector<GearRef> gears;
    for (const GearRef& g : allGears)
        if (g.params.kind == GearKind::Bevel) gears.push_back(g);
    QSettings settings;
    settings.beginGroup(QStringLiteral("part_bevel"));
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("PartDialog"));
    dlg.setWindowTitle(QStringLiteral("Bevel Gear"));
    auto* fl = new QFormLayout(&dlg);
    auto* fit = new QComboBox(&dlg);
    fit->setObjectName(QStringLiteral("gearFitTo"));
    fit->addItem(QStringLiteral("Nothing: a new pair"));
    int selected = 0;
    for (size_t i = 0; i < gears.size(); ++i) {
        fit->addItem(QString::fromStdString(gears[i].name) + (gears[i].instance != kNone ? QStringLiteral(" (selected)") : QString()));
        if (gears[i].instance != kNone && !selected) selected = int(i) + 1;
    }
    fit->setCurrentIndex(selected);
    fit->setToolTip(QStringLiteral("A bevel gear is made for one mate: fitting to a bevel gear in the model makes that mate."));
    fl->addRow(QStringLiteral("Fit to:"), fit);
    fl->setRowVisible(fit, !gears.empty());
    auto* beside = new QCheckBox(QStringLiteral("Place it on the selected gear, meshing"), &dlg);
    beside->setObjectName(QStringLiteral("gearPlaceMeshing"));
    beside->setChecked(true);
    fl->addRow(QString(), beside);
    auto* pair = new QCheckBox(QStringLiteral("Place both gears, meshing (one component, explode to separate)"), &dlg);
    pair->setObjectName(QStringLiteral("gearPlacePair"));
    pair->setChecked(settings.value("pair", true).toBool());
    fl->addRow(QString(), pair);
    auto* mode = new QComboBox(&dlg);
    mode->setObjectName(QStringLiteral("gearMode"));
    mode->addItem(QStringLiteral("Size (the number of teeth follows)"));
    mode->addItem(QStringLiteral("Module and number of teeth (engineering)"));
    mode->setCurrentIndex(std::clamp(settings.value("mode", 0).toInt(), 0, 1));
    fl->addRow(QStringLiteral("Choose by:"), mode);
    auto* module = new PresetCombo(standardModules(), settings.value("module", 1.0).toDouble(), QStringLiteral("gearModule"), &dlg);
    module->setToolTip(QStringLiteral("The tooth size at the large end (ISO 54 modules)."));
    fl->addRow(QStringLiteral("Tooth size, module (mm):"), module);
    auto* diameter = mmBox(&dlg, "gearDiameter", settings.value("diameter", 20.0).toDouble(), 1, 100000);
    diameter->setToolTip(QStringLiteral("Pitch diameter at the large end, about: rounded to whole teeth."));
    fl->addRow(QStringLiteral("Diameter:"), diameter);
    auto* mateDiameter = mmBox(&dlg, "gearMateDiameter", settings.value("mateDiameter", 20.0).toDouble(), 1, 100000);
    mateDiameter->setToolTip(QStringLiteral("The gear it meshes with, at right angles. The same size makes a 45° miter pair."));
    fl->addRow(QStringLiteral("Other gear's diameter:"), mateDiameter);
    auto* teeth = new QSpinBox(&dlg);
    teeth->setObjectName(QStringLiteral("gearTeeth"));
    teeth->setRange(6, 400);
    teeth->setValue(settings.value("teeth", 20).toInt());
    fl->addRow(QStringLiteral("Teeth:"), teeth);
    auto* mateTeeth = new QSpinBox(&dlg);
    mateTeeth->setObjectName(QStringLiteral("gearMateTeeth"));
    mateTeeth->setRange(6, 400);
    mateTeeth->setValue(settings.value("mateTeeth", 20).toInt());
    fl->addRow(QStringLiteral("Other gear's teeth:"), mateTeeth);
    auto* angle = new PresetCombo({20, 14.5, 25}, settings.value("angle", 20.0).toDouble(), QStringLiteral("gearPressureAngle"), &dlg);
    fl->addRow(QStringLiteral("Pressure angle (°):"), angle);
    auto* width = mmBox(&dlg, "gearFaceWidth", settings.value("faceWidth", 0.0).toDouble(), 0, 10000);
    width->setSpecialValueText(QStringLiteral("Auto (a third of the cone)"));
    width->setToolTip(QStringLiteral("How long the teeth are, along the cone towards its tip."));
    fl->addRow(QStringLiteral("Tooth length:"), width);
    auto* bore = mmBox(&dlg, "gearBore", settings.value("bore", 0.0).toDouble(), 0, 10000);
    bore->setSpecialValueText(QStringLiteral("None"));
    fl->addRow(QStringLiteral("Shaft hole:"), bore);
    auto* backlash = mmBox(&dlg, "gearBacklash", settings.value("backlash", 0.0).toDouble(), 0, 100, 3);
    fl->addRow(QStringLiteral("Backlash:"), backlash);
    auto* steps = new QSpinBox(&dlg);
    steps->setObjectName(QStringLiteral("gearFlankSteps"));
    steps->setRange(1, 64);
    steps->setValue(settings.value("flankSteps", 6).toInt());
    fl->addRow(QStringLiteral("Flank pieces:"), steps);
    auto* info = new QLabel(&dlg);
    info->setObjectName(QStringLiteral("partInfo"));
    info->setWordWrap(true);
    info->setMinimumWidth(380);
    fl->addRow(info);
    auto* bb = placeButtons(dlg);
    fl->addRow(bb);

    auto target = [&]() -> const GearRef* { return fit->currentIndex() > 0 ? &gears[fit->currentIndex() - 1] : nullptr; };
    auto bySize = [&] { return mode->currentIndex() == 0; };
    auto params = [&] {
        GearParams p;
        p.kind = GearKind::Bevel;
        const GearRef* t = target();
        p.module = t ? t->params.module : module->value() * 0.001;
        p.pressureAngle = t ? t->params.pressureAngle : angle->value();
        if (t) {
            p.teeth = t->params.mateTeeth > 0 ? t->params.mateTeeth : t->params.teeth;
            p.mateTeeth = t->params.teeth;
        } else if (bySize()) {
            p.teeth = std::clamp(gearTeethForDiameter(p.module, diameter->value() * 0.001), 6, 400);
            p.mateTeeth = std::clamp(gearTeethForDiameter(p.module, mateDiameter->value() * 0.001), 6, 400);
        } else {
            p.teeth = teeth->value();
            p.mateTeeth = mateTeeth->value();
        }
        p.faceWidth = width->value() * 0.001;
        p.bore = bore->value() * 0.001;
        p.backlash = backlash->value() * 0.001;
        p.flankSteps = steps->value();
        return p;
    };
    auto refresh = [&] {
        const GearRef* t = target();
        const bool size = bySize();
        fl->setRowVisible(beside, t && t->instance != kNone);
        fl->setRowVisible(pair, !t);
        fl->setRowVisible(mode, !t);
        fl->setRowVisible(diameter, !t && size);
        fl->setRowVisible(mateDiameter, !t && size);
        fl->setRowVisible(teeth, !t && !size);
        fl->setRowVisible(mateTeeth, !t && !size);
        fl->setRowVisible(angle, !size || t);
        fl->setRowVisible(backlash, !size);
        fl->setRowVisible(steps, !size);
        module->setEnabled(!t);
        angle->setEnabled(!t);
        if (t) {
            QSignalBlocker b1(module), b2(angle);
            module->setValue(t->params.module * 1000);
            angle->setValue(t->params.pressureAngle);
        }
        const GearParams p = params();
        Mesh m;
        std::string name, err;
        GearInfo gi;
        const bool ok = !std::isnan(p.module) && !std::isnan(p.pressureAngle) && buildGear(p, m, &name, &gi, &err);
        QString text;
        if (!ok) text = err.empty() ? QStringLiteral("Type a number.") : QString::fromStdString(err);
        else {
            text = QStringLiteral("<b>%1</b><br>%2 teeth, cone %3°, pitch Ø %4 mm, outside Ø %5 mm<br>"
                                  "teeth %6 mm long; meshes at right angles with a %7-tooth bevel gear (ratio %8)<br>%9%10 faces")
                       .arg(QString::fromStdString(name))
                       .arg(p.teeth)
                       .arg(gi.coneAngle, 0, 'f', 2)
                       .arg(gi.pitchDiameter * 1000, 0, 'f', 2)
                       .arg(gi.outsideDiameter * 1000, 0, 'f', 2)
                       .arg(gi.faceWidth * 1000, 0, 'f', 2)
                       .arg(p.mateTeeth)
                       .arg(double(p.mateTeeth) / p.teeth, 0, 'g', 4)
                       .arg(gi.undercut ? QStringLiteral("Few teeth for this pressure angle: a cut gear would be undercut "
                                                         "(not modelled). ")
                                        : QString())
                       .arg(m.aliveFaceCount());
            if (t) text += QStringLiteral("<br>The mate of %1.").arg(QString::fromStdString(t->name));
            if (!t && pair->isChecked()) {
                GearParams q = p;
                std::swap(q.teeth, q.mateTeeth);
                Mesh m2;
                std::string e2;
                if (!buildGear(q, m2, nullptr, nullptr, &e2)) {
                    text += QStringLiteral("<br>The other gear: ") + QString::fromStdString(e2);
                    info->setText(text);
                    bb->button(QDialogButtonBox::Ok)->setEnabled(false);
                    dlg.adjustSize();
                    return;
                }
            }
        }
        info->setText(text);
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        dlg.adjustSize();
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg, [&](int i) {
        const double m = module->value() * 0.001;
        if (!std::isnan(m)) {
            QSignalBlocker b1(teeth), b2(diameter), b3(mateTeeth), b4(mateDiameter);
            if (i == 0) {
                diameter->setValue(m * teeth->value() * 1000);
                mateDiameter->setValue(m * mateTeeth->value() * 1000);
            } else {
                teeth->setValue(gearTeethForDiameter(m, diameter->value() * 0.001));
                mateTeeth->setValue(gearTeethForDiameter(m, mateDiameter->value() * 0.001));
            }
        }
        refresh();
    });
    QObject::connect(fit, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(module, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(angle, &QComboBox::currentTextChanged, &dlg, refresh);
    for (QSpinBox* sb : {teeth, mateTeeth, steps}) QObject::connect(sb, &QSpinBox::valueChanged, &dlg, refresh);
    for (QCheckBox* cb : {beside, pair}) QObject::connect(cb, &QCheckBox::toggled, &dlg, refresh);
    for (QDoubleSpinBox* sb : {diameter, mateDiameter, width, bore, backlash}) QObject::connect(sb, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    refresh();
    if (dlg.exec() != QDialog::Accepted) return false;
    choice.gearParams = params();
    const GearRef* t = target();
    if (t && t->instance != kNone && beside->isChecked()) {
        choice.meshWith = t->instance;
        choice.meshParams = t->params;
    }
    choice.pair = !t && pair->isChecked();
    settings.setValue("mode", mode->currentIndex());
    settings.setValue("pair", pair->isChecked());
    if (!t) {
        settings.setValue("module", choice.gearParams.module * 1000);
        settings.setValue("angle", choice.gearParams.pressureAngle);
        settings.setValue("teeth", choice.gearParams.teeth);
        settings.setValue("mateTeeth", choice.gearParams.mateTeeth);
        settings.setValue("diameter", diameter->value());
        settings.setValue("mateDiameter", mateDiameter->value());
    }
    settings.setValue("faceWidth", width->value());
    settings.setValue("bore", bore->value());
    settings.setValue("backlash", backlash->value());
    settings.setValue("flankSteps", steps->value());
    return true;
}

} // namespace

bool runPartDialog(QWidget* parent, PartFamily family, PartChoice& out, const std::vector<GearRef>& gears) {
    out = PartChoice();
    if (family == PartFamily::BevelGear) {
        out.gear = true;
        return bevelDialog(parent, out, gears);
    }
    if (family == PartFamily::SpurGear || family == PartFamily::Rack) {
        out.gear = true;
        return gearDialog(parent, family, out, gears);
    }
    return fastenerDialog(parent, family, out.fastener);
}

std::string withGearTag(const std::string& extra, const GearParams& p) {
    QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(extra)).object();
    QJsonObject g;
    g[QStringLiteral("kind")] = p.kind == GearKind::Rack    ? QStringLiteral("rack")
                                : p.kind == GearKind::Bevel ? QStringLiteral("bevel")
                                                            : QStringLiteral("spur");
    if (p.kind == GearKind::Bevel) g[QStringLiteral("mateTeeth")] = p.mateTeeth;
    g[QStringLiteral("module")] = p.module;
    g[QStringLiteral("teeth")] = p.teeth;
    g[QStringLiteral("pressureAngle")] = p.pressureAngle;
    g[QStringLiteral("faceWidth")] = p.faceWidth;
    g[QStringLiteral("bore")] = p.bore;
    g[QStringLiteral("rackHeight")] = p.rackHeight;
    g[QStringLiteral("backlash")] = p.backlash;
    root[QStringLiteral("formaGear")] = g;
    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

bool gearFromComponent(const std::string& extra, const std::string& name, GearParams& out) {
    if (!extra.empty()) {
        const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(extra)).object();
        QJsonObject g = root.value(QStringLiteral("formaGear")).toObject();
        if (g.isEmpty()) g = root.value(QStringLiteral("gnusketchGear")).toObject(); // saved before the rename to Forma
        if (!g.isEmpty() && g.value(QStringLiteral("module")).toDouble() > 0 && g.value(QStringLiteral("teeth")).toInt() > 0) {
            out = GearParams();
            const QString kind = g.value(QStringLiteral("kind")).toString();
            out.kind = kind == QLatin1String("rack") ? GearKind::Rack : kind == QLatin1String("bevel") ? GearKind::Bevel : GearKind::Spur;
            out.mateTeeth = g.value(QStringLiteral("mateTeeth")).toInt(0);
            out.module = g.value(QStringLiteral("module")).toDouble();
            out.teeth = g.value(QStringLiteral("teeth")).toInt();
            out.pressureAngle = g.value(QStringLiteral("pressureAngle")).toDouble(20);
            out.faceWidth = g.value(QStringLiteral("faceWidth")).toDouble(out.faceWidth);
            out.bore = g.value(QStringLiteral("bore")).toDouble(out.bore);
            out.rackHeight = g.value(QStringLiteral("rackHeight")).toDouble(0);
            out.backlash = g.value(QStringLiteral("backlash")).toDouble(0);
            return true;
        }
    }
    // Gears placed before the parameters were kept: their names say enough
    // (the pressure angle is taken as the usual 20°).
    static const QRegularExpression spur(QStringLiteral("^Spur Gear m([0-9.]+) z([0-9]+)")),
        rackName(QString::fromUtf8("^Gear Rack m([0-9.]+) \u00d7 ([0-9]+)"));
    const QString n = QString::fromStdString(name);
    QRegularExpressionMatch mt = spur.match(n);
    const bool isRack = !mt.hasMatch() && (mt = rackName.match(n)).hasMatch();
    if (!mt.hasMatch()) return false;
    out = GearParams();
    out.kind = isRack ? GearKind::Rack : GearKind::Spur;
    out.module = mt.captured(1).toDouble() * 0.001;
    out.teeth = mt.captured(2).toInt();
    return out.module > 0 && out.teeth > 0;
}

} // namespace forma
