// SPDX-License-Identifier: GPL-3.0-or-later
#include "PartDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace gs {

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

bool gearDialog(QWidget* parent, PartFamily family, GearParams& out) {
    const bool rack = family == PartFamily::Rack;
    QSettings settings;
    settings.beginGroup(rack ? QStringLiteral("part_rack") : QStringLiteral("part_gear"));
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("PartDialog"));
    dlg.setWindowTitle(rack ? QStringLiteral("Gear Rack") : QStringLiteral("Spur Gear"));
    auto* fl = new QFormLayout(&dlg);
    auto* module = new PresetCombo(standardModules(), settings.value("module", 1.0).toDouble(), QStringLiteral("gearModule"), &dlg);
    module->setToolTip(QStringLiteral("ISO 54 modules. The pitch diameter is module × teeth; gears that mesh share a module "
                                      "and a pressure angle."));
    fl->addRow(QStringLiteral("Module (mm):"), module);
    auto* teeth = new QSpinBox(&dlg);
    teeth->setObjectName(QStringLiteral("gearTeeth"));
    teeth->setRange(rack ? 1 : 6, rack ? 2000 : 1000);
    teeth->setValue(settings.value("teeth", rack ? 10 : 20).toInt());
    fl->addRow(QStringLiteral("Teeth:"), teeth);
    auto* angle = new PresetCombo({20, 14.5, 25}, settings.value("angle", 20.0).toDouble(), QStringLiteral("gearPressureAngle"), &dlg);
    fl->addRow(QStringLiteral("Pressure angle (°):"), angle);
    auto* width = mmBox(&dlg, "gearFaceWidth", settings.value("faceWidth", 8.0).toDouble(), 0.01, 10000);
    fl->addRow(QStringLiteral("Face width:"), width);
    auto* bore = mmBox(&dlg, "gearBore", settings.value("bore", 5.0).toDouble(), 0, 10000);
    bore->setSpecialValueText(QStringLiteral("None"));
    fl->addRow(QStringLiteral("Bore:"), bore);
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
    fl->setRowVisible(bore, !rack);
    fl->setRowVisible(steps, !rack);
    fl->setRowVisible(height, rack);
    auto* info = new QLabel(&dlg);
    info->setObjectName(QStringLiteral("partInfo"));
    info->setWordWrap(true);
    info->setMinimumWidth(340);
    fl->addRow(info);
    auto* bb = placeButtons(dlg);
    fl->addRow(bb);

    auto params = [&] {
        GearParams p;
        p.kind = rack ? GearKind::Rack : GearKind::Spur;
        p.module = module->value() * 0.001;
        p.teeth = teeth->value();
        p.pressureAngle = angle->value();
        p.faceWidth = width->value() * 0.001;
        p.bore = bore->value() * 0.001;
        p.rackHeight = height->value() * 0.001;
        p.backlash = backlash->value() * 0.001;
        p.flankSteps = steps->value();
        return p;
    };
    auto refresh = [&] {
        const GearParams p = params();
        Mesh m;
        std::string name, err;
        GearInfo gi;
        const bool ok = !std::isnan(p.module) && !std::isnan(p.pressureAngle) && buildGear(p, m, &name, &gi, &err);
        if (!ok) info->setText(err.empty() ? QStringLiteral("Type a number.") : QString::fromStdString(err));
        else if (rack)
            info->setText(QStringLiteral("<b>%1</b><br>circular pitch %2 mm, length %3 mm<br>%4 faces")
                              .arg(QString::fromStdString(name))
                              .arg(gi.pitch * 1000, 0, 'f', 3)
                              .arg(gi.length * 1000, 0, 'f', 2)
                              .arg(m.aliveFaceCount()));
        else
            info->setText(QStringLiteral("<b>%1</b><br>pitch Ø %2 mm, outside Ø %3 mm, root Ø %4 mm, base Ø %5 mm<br>%6%7 faces")
                              .arg(QString::fromStdString(name))
                              .arg(gi.pitchDiameter * 1000, 0, 'f', 2)
                              .arg(gi.outsideDiameter * 1000, 0, 'f', 2)
                              .arg(gi.rootDiameter * 1000, 0, 'f', 2)
                              .arg(gi.baseDiameter * 1000, 0, 'f', 2)
                              .arg(gi.undercut ? QStringLiteral("Few teeth for this pressure angle: a cut gear would be "
                                                                "undercut (not modelled). ")
                                               : QString())
                              .arg(m.aliveFaceCount()));
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        dlg.adjustSize();
    };
    QObject::connect(module, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(angle, &QComboBox::currentTextChanged, &dlg, refresh);
    QObject::connect(teeth, &QSpinBox::valueChanged, &dlg, refresh);
    QObject::connect(steps, &QSpinBox::valueChanged, &dlg, refresh);
    for (QDoubleSpinBox* sb : {width, bore, height, backlash}) QObject::connect(sb, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    refresh();
    if (dlg.exec() != QDialog::Accepted) return false;
    out = params();
    settings.setValue("module", out.module * 1000);
    settings.setValue("teeth", out.teeth);
    settings.setValue("angle", out.pressureAngle);
    settings.setValue("faceWidth", out.faceWidth * 1000);
    if (!rack) settings.setValue("bore", out.bore * 1000);
    else settings.setValue("rackHeight", out.rackHeight * 1000);
    settings.setValue("backlash", out.backlash * 1000);
    settings.setValue("flankSteps", out.flankSteps);
    return true;
}

} // namespace

bool runPartDialog(QWidget* parent, PartFamily family, PartChoice& out) {
    out = PartChoice();
    if (family == PartFamily::SpurGear || family == PartFamily::Rack) {
        out.gear = true;
        return gearDialog(parent, family, out.gearParams);
    }
    return fastenerDialog(parent, family, out.fastener);
}

} // namespace gs
