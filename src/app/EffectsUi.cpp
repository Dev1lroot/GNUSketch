// SPDX-License-Identifier: GPL-3.0-or-later
#include "EffectsUi.h"

#include "core/GearNotch.h"
#include "core/Slicer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

EffectParam real(const QString& key, const QString& label, double min, double max, double def, int decimals,
                 const QString& suffix, bool log = false, const QString& tip = {}) {
    EffectParam p;
    p.key = key;
    p.label = label;
    p.type = EffectParam::Double;
    p.min = min;
    p.max = max;
    p.def = def;
    p.decimals = decimals;
    p.suffix = suffix;
    p.logScale = log;
    p.tip = tip;
    return p;
}
EffectParam whole(const QString& key, const QString& label, double min, double max, double def) {
    EffectParam p = real(key, label, min, max, def, 0, {});
    p.type = EffectParam::Int;
    return p;
}
EffectParam flag(const QString& key, const QString& label, bool def, const QString& tip = {}) {
    EffectParam p;
    p.key = key;
    p.label = label;
    p.type = EffectParam::Bool;
    p.def = def;
    p.tip = tip;
    return p;
}

// Densities in the settings are per meter; the kernel takes lengths.
double lengthFrom(const QVariantMap& v, const char* key) {
    const double d = v.value(key).toDouble();
    return d > 0 ? 1.0 / d : 0.0;
}

const QString kDetailTip = QStringLiteral("Most edges per meter along the surface. Higher follows the effect more closely but makes more polygons. 0 = chosen automatically.");

std::vector<EffectDef> buildRegistry() {
    std::vector<EffectDef> r;
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Noise");
        d.submenu = QStringLiteral("Distort");
        d.icon = QStringLiteral("fx_noise");
        d.tip = QStringLiteral("Moves the surface randomly along the chosen axes: bumps, rock, crumpled shapes.");
        d.params = {flag("x", QStringLiteral("Red axis (X)"), true), flag("y", QStringLiteral("Green axis (Y)"), true),
                    flag("z", QStringLiteral("Blue axis (Z)"), true),
                    real("amount", QStringLiteral("Distance"), 0.001, 50, 0.05, 3, QStringLiteral(" m"), true,
                         QStringLiteral("Farthest the surface moves along each axis.")),
                    real("scale", QStringLiteral("Bump size"), 0.01, 100, 0.5, 2, QStringLiteral(" m"), true),
                    whole("octaves", QStringLiteral("Roughness"), 1, 6, 3),
                    real("density", QStringLiteral("Edges per meter"), 0, 200, 0, 1, QStringLiteral(" /m"), false, kDetailTip)};
        EffectParam seed;
        seed.key = "seed";
        seed.label = QStringLiteral("Pattern");
        seed.type = EffectParam::Seed;
        seed.def = 1;
        d.params.push_back(seed);
        d.params.push_back(flag("smooth", QStringLiteral("Smooth shading"), true));
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            NoiseParams p;
            p.x = v["x"].toBool();
            p.y = v["y"].toBool();
            p.z = v["z"].toBool();
            p.amount = v["amount"].toDouble();
            p.scale = v["scale"].toDouble();
            p.octaves = v["octaves"].toInt();
            p.detail = lengthFrom(v, "density");
            p.seed = v["seed"].toInt();
            p.smooth = v["smooth"].toBool();
            return effectNoise(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Blur");
        d.submenu = QStringLiteral("Soften");
        d.icon = QStringLiteral("fx_blur");
        d.tip = QStringLiteral("Softens the edges and corners of the surface. Where the selection joins other faces it stays in place.");
        d.params = {real("radius", QStringLiteral("Softness"), 0.005, 20, 0.1, 3, QStringLiteral(" m"), true,
                         QStringLiteral("How far the rounding reaches from an edge.")),
                    real("strength", QStringLiteral("Strength"), 1, 100, 100, 0, QStringLiteral(" %")),
                    real("density", QStringLiteral("Edges per meter"), 0, 200, 0, 1, QStringLiteral(" /m"), false, kDetailTip),
                    flag("volume", QStringLiteral("Keep volume"), true, QStringLiteral("Stops the shape from shrinking as it softens."))};
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            BlurParams p;
            p.radius = v["radius"].toDouble();
            p.strength = v["strength"].toDouble() / 100;
            p.detail = lengthFrom(v, "density");
            p.keepVolume = v["volume"].toBool();
            return effectBlur(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Decimate");
        d.submenu = QStringLiteral("Polygons");
        d.icon = QStringLiteral("fx_polygons");
        d.tip = QStringLiteral("Removes polygons while keeping the shape as close as possible.");
        EffectParam mode;
        mode.key = "mode";
        mode.label = QStringLiteral("Target");
        mode.type = EffectParam::Choice;
        mode.choices = {QStringLiteral("Percentage to keep"), QStringLiteral("Polygons per square meter")};
        d.params = {mode, real("ratio", QStringLiteral("Keep"), 1, 100, 50, 0, QStringLiteral(" %")),
                    real("density", QStringLiteral("Per square meter"), 0.01, 10000, 10, 2, QStringLiteral(" /m²"), true),
                    flag("merge", QStringLiteral("Merge flat areas into n-gons"), true)};
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            DecimateParams p;
            p.mode = v["mode"].toInt() == 0 ? DecimateParams::Ratio : DecimateParams::Density;
            p.ratio = v["ratio"].toDouble() / 100;
            p.density = v["density"].toDouble();
            p.mergeFlat = v["merge"].toBool();
            return effectDecimate(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Subdivide");
        d.submenu = QStringLiteral("Polygons");
        d.tip = QStringLiteral("Splits the surface into triangles until no edge is longer than the density asks for.");
        d.params = {real("density", QStringLiteral("Edges per meter"), 0.1, 200, 4, 1, QStringLiteral(" /m"), true)};
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            return effectSubdivide(m, f, v["density"].toDouble(), out, err);
        };
        r.push_back(std::move(d));
    }
    auto remesh = [&](const QString& name, const QString& text, RemeshParams::Shape shape) {
        EffectDef d;
        d.name = name;
        d.menuText = text;
        d.submenu = QStringLiteral("Polygons");
        d.tip = QStringLiteral("Rebuilds the surface as a regular pattern of %1. Flat areas stay perfectly flat; on curved surfaces the cells are slightly warped.").arg(text.toLower());
        d.params = {real("density", QStringLiteral("Cells per meter"), 0.05, 100, 2, 2, QStringLiteral(" /m"), true),
                    real("angle", QStringLiteral("Rotation"), -90, 90, 0, 0, QStringLiteral("°")),
                    flag("smooth", QStringLiteral("Smooth shading"), true)};
        d.apply = [shape](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            RemeshParams p;
            p.shape = shape;
            p.cellsPerMeter = v["density"].toDouble();
            p.angle = v["angle"].toDouble();
            p.smooth = v["smooth"].toBool();
            return effectRemesh(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    };
    remesh(QStringLiteral("Remesh to Quads"), QStringLiteral("Quads"), RemeshParams::Quads);
    remesh(QStringLiteral("Remesh to Hexagons"), QStringLiteral("Hexagons"), RemeshParams::Hexagons);
    remesh(QStringLiteral("Remesh to Triangles"), QStringLiteral("Triangles"), RemeshParams::Triangles);
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Voxelate");
        d.submenu = QStringLiteral("Polygons");
        d.icon = QStringLiteral("fx_voxelate");
        d.tip = QStringLiteral("Rebuilds the geometry out of cubes of this size, on a grid aligned with the world. A shape that fits the grid keeps its exact size.");
        d.params = {real("size", QStringLiteral("Cube size"), 0.01, 10, 0.1, 3, QStringLiteral(" m"), true),
                    flag("fill", QStringLiteral("Solid inside"), true),
                    flag("merge", QStringLiteral("Merge flat sides"), false)};
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            VoxelateParams p;
            p.size = v["size"].toDouble();
            p.fill = v["fill"].toBool();
            p.mergeFlat = v["merge"].toBool();
            return effectVoxelate(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Triangulate");
        d.submenu = QStringLiteral("Polygons");
        d.tip = QStringLiteral("Splits every polygon into triangles, without moving anything.");
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap&, EffectResult* out, std::string* err) {
            return effectTriangulate(m, f, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("De-triangulate");
        d.submenu = QStringLiteral("Polygons");
        d.icon = QStringLiteral("fx_detriangulate");
        d.tip = QStringLiteral("Joins triangles back into polygons, for imported models (STL, 3MF...): flat neighbours become one face and the points in between on straight outlines go. Optionally pairs of triangles on curved surfaces become quads.");
        d.params = {real("angle", QStringLiteral("Flatness tolerance"), 0, 5, 0.1, 2, QStringLiteral("°"), false,
                         QStringLiteral("Neighbours this close to one plane count as flat (imported files are slightly uneven).")),
                    flag("quads", QStringLiteral("Quads on curved surfaces"), false,
                         QStringLiteral("Pairs of the remaining triangles become quads across their longest edge.")),
                    real("quadAngle", QStringLiteral("Quad bend up to"), 1, 60, 30, 0, QStringLiteral("°"))};
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            DetriangulateParams p;
            p.angle = v["angle"].toDouble();
            p.quads = v["quads"].toBool();
            p.quadAngle = v["quadAngle"].toDouble();
            return effectDetriangulate(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Hollow");
        d.submenu = QStringLiteral("Solid");
        d.icon = QStringLiteral("fx_hollow");
        d.tip = QStringLiteral("Makes a closed solid hollow with walls of this thickness (for 3D printing: less material). Works on any shape; where it is thinner than two walls it stays solid. The outside is not changed.");
        d.params = {real("thickness", QStringLiteral("Wall thickness"), 0.0001, 10, 0.002, 4, QStringLiteral(" m"), true),
                    whole("detail", QStringLiteral("Detail"), 16, 256, 96)};
        d.params.back().tip = QStringLiteral("Grid cells along the longest side for the inner wall (more: finer, slower).");
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            HollowParams p;
            p.thickness = v["thickness"].toDouble();
            p.detail = v["detail"].toInt();
            return effectHollow(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Solidify");
        d.submenu = QStringLiteral("Solid");
        d.icon = QStringLiteral("fx_solidify");
        d.tip = QStringLiteral("Leaves one solid seen from outside: overlapping parts merge, inner walls, sealed hollows and loose sheets go, faces showing their back outside are turned around. Gaps narrower than a grid cell count as closed.");
        d.params = {whole("detail", QStringLiteral("Detail"), 32, 400, 128), flag("merge", QStringLiteral("Merge overlapping parts"), true)};
        d.params.front().tip = QStringLiteral("Grid cells along the longest side for finding the outside.");
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap& v, EffectResult* out, std::string* err) {
            SolidifyParams p;
            p.detail = v["detail"].toInt();
            p.mergeParts = v["merge"].toBool();
            return effectSolidify(m, f, p, out, err);
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Gear Notches");
        d.submenu = QStringLiteral("Shape");
        d.icon = QStringLiteral("fx_gear");
        d.tip = QStringLiteral("Teeth or notches along what is selected: a circle or any flat shape (its outline), "
                               "a curve, the sides of a rectangle, a single line. Straight sides get their own row of "
                               "teeth so corners stay clean; on a circle the teeth point out from the centre. "
                               "Push/pull the toothed shape afterwards to make a gear.");
        EffectParam profile;
        profile.key = "profile";
        profile.label = QStringLiteral("Tooth shape");
        profile.type = EffectParam::Choice;
        profile.choices = {QStringLiteral("Square"), QStringLiteral("Trapezoid"), QStringLiteral("Triangle (V)"),
                           QStringLiteral("Sawtooth"), QStringLiteral("Rounded"), QStringLiteral("Gear (curved flanks)")};
        profile.def = GearNotchParams::Gear;
        EffectParam spacing;
        spacing.key = "spacing";
        spacing.label = QStringLiteral("Spacing by");
        spacing.type = EffectParam::Choice;
        spacing.choices = {QStringLiteral("Number of teeth"), QStringLiteral("Distance between teeth")};
        spacing.def = 0;
        EffectParam direction;
        direction.key = "direction";
        direction.label = QStringLiteral("Direction");
        direction.type = EffectParam::Choice;
        direction.choices = {QStringLiteral("Teeth outward"), QStringLiteral("Notches inward")};
        direction.def = 0;
        direction.tip = QStringLiteral("Outward: teeth stand out of the shape. Inward: notches are cut into it.");
        d.params = {profile, spacing,
                    whole("count", QStringLiteral("Teeth"), 1, 1000, 24),
                    real("pitch", QStringLiteral("Distance between teeth"), 0.001, 100, 0.1, 3, QStringLiteral(" m"), true,
                         QStringLiteral("Used when spacing by distance: one tooth this far from the next (adjusted to fit evenly).")),
                    real("depth", QStringLiteral("Depth"), 0.0001, 10, 0.05, 4, QStringLiteral(" m"), true,
                         QStringLiteral("Height of a tooth, or depth of a notch.")),
                    real("width", QStringLiteral("Tooth width"), 2, 100, 50, 0, QStringLiteral(" %"), false,
                         QStringLiteral("Share of each tooth's space the tooth takes at its base; the rest is the gap.")),
                    real("tip", QStringLiteral("Tip width"), 0, 100, 50, 0, QStringLiteral(" %"), false,
                         QStringLiteral("Width at the tip as a share of the base (Trapezoid and Gear).")),
                    direction,
                    real("phase", QStringLiteral("Offset"), 0, 100, 0, 0, QStringLiteral(" %"), false,
                         QStringLiteral("Turns the teeth along a closed curve, in parts of one tooth.")),
                    real("corner", QStringLiteral("Corner angle"), 1, 180, 30, 0, QStringLiteral("°"), false,
                         QStringLiteral("A turn sharper than this is a corner: each side between corners gets its own teeth.")),
                    whole("segments", QStringLiteral("Curve segments"), 2, 32, 6),
                    flag("holes", QStringLiteral("Holes of selected faces too"), false,
                         QStringLiteral("Teeth on the inner outlines as well (an internal gear).")),
                    flag("flip", QStringLiteral("Other side (lines without a face)"), false)};
        d.params[10].tip = QStringLiteral("Pieces of a rounded tooth or a curved flank.");
        d.applyLines = [](Mesh& m, Selection& sel, const QVariantMap& v, QString& message, std::string* err) {
            GearNotchParams p;
            p.profile = (GearNotchParams::Profile)std::clamp(v["profile"].toInt(), 0, (int)GearNotchParams::Gear);
            p.byCount = v["spacing"].toInt() == 0;
            p.count = v["count"].toInt();
            p.pitch = v["pitch"].toDouble();
            p.depth = v["depth"].toDouble();
            p.toothWidth = v["width"].toDouble() / 100;
            p.tipWidth = v["tip"].toDouble() / 100;
            p.inward = v["direction"].toInt() == 1;
            p.phase = v["phase"].toDouble() / 100;
            p.cornerAngle = v["corner"].toDouble();
            p.curveSegments = v["segments"].toInt();
            p.holes = v["holes"].toBool();
            p.flip = v["flip"].toBool();
            GearNotchResult r;
            if (!gearNotch(m, sel.faces, sel.edges, p, &r, err)) return false;
            sel.faces = r.faces;
            sel.edges = r.edges;
            message = QStringLiteral("%1 teeth on %2 %3").arg(r.teeth).arg(r.paths).arg(r.paths == 1 ? QStringLiteral("outline") : QStringLiteral("outlines"));
            return true;
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Slicer");
        d.submenu = QStringLiteral("Solid");
        d.icon = QStringLiteral("fx_slicer");
        d.tip = QStringLiteral("Cuts a solid (selected faces or components) into pieces no larger than the given size, "
                               "for example a 3D printer's build plate, each piece a component of its own. "
                               "Connectors on the cuts (pegs, keys or ribs, with matching sockets) line the pieces up "
                               "and make the glued joints stronger.");
        EffectParam conn;
        conn.key = "connector";
        conn.label = QStringLiteral("Connectors");
        conn.type = EffectParam::Choice;
        conn.choices = {QStringLiteral("None"), QStringLiteral("Round pegs"), QStringLiteral("Square keys"),
                        QStringLiteral("Ribs (tongue and groove)")};
        conn.def = 0;
        conn.tip = QStringLiteral("Standing out of one piece's cut face, sunk into the next one's: the parts only fit one way and the glue holds on more surface.");
        d.params = {real("w", QStringLiteral("Width (red, X)"), 0.001, 100, 0.22, 4, QStringLiteral(" m"), true,
                         QStringLiteral("Largest piece along the red axis, e.g. the printer's build plate width.")),
                    real("l", QStringLiteral("Length (green, Y)"), 0.001, 100, 0.22, 4, QStringLiteral(" m"), true),
                    real("h", QStringLiteral("Height (blue, Z)"), 0.001, 100, 0.25, 4, QStringLiteral(" m"), true),
                    flag("equal", QStringLiteral("Equal pieces"), true,
                         QStringLiteral("Divide each axis into equal parts no larger than the size, instead of full-size parts and a remainder.")),
                    conn,
                    real("csize", QStringLiteral("Connector size"), 0.0005, 1, 0.008, 4, QStringLiteral(" m"), true,
                         QStringLiteral("Peg diameter, key or rib width.")),
                    real("clength", QStringLiteral("Connector length"), 0.0005, 1, 0.006, 4, QStringLiteral(" m"), true,
                         QStringLiteral("How far it stands out of the cut, and how deep its socket is.")),
                    real("clearance", QStringLiteral("Clearance"), 0, 0.01, 0.0002, 5, QStringLiteral(" m"), false,
                         QStringLiteral("The socket is larger by this much all round so the parts fit after printing (0.1–0.3 mm is usual).")),
                    real("margin", QStringLiteral("Wall around"), 0, 0.1, 0.003, 4, QStringLiteral(" m"), false,
                         QStringLiteral("Solid material kept around each connector; where there is not that much room, none is placed.")),
                    whole("percut", QStringLiteral("Connectors per cut"), 1, 16, 4),
                    real("spread", QStringLiteral("Gap between pieces"), 0, 10, 0, 3, QStringLiteral(" m"), false,
                         QStringLiteral("Moves the pieces apart to see them separately (0 = in place).")),};
        d.applyModel = [](ModelState& st, Selection& sel, const QVariantMap& v, QString& message, std::string* err) {
            SlicerParams p;
            p.size = Vec3(v["w"].toDouble(), v["l"].toDouble(), v["h"].toDouble());
            p.equal = v["equal"].toBool();
            p.connector = (SlicerParams::Connector)std::clamp(v["connector"].toInt(), 0, (int)SlicerParams::Ribs);
            p.connectorSize = v["csize"].toDouble();
            p.connectorLength = v["clength"].toDouble();
            p.clearance = v["clearance"].toDouble();
            p.margin = v["margin"].toDouble();
            p.perCut = v["percut"].toInt();
            p.spread = v["spread"].toDouble();
            SlicerResult r;
            if (!sliceSelection(st, sel, p, &r, err)) return false;
            message = QStringLiteral("%1 pieces (%2 × %3 × %4 cells of %5 × %6 × %7 mm)")
                          .arg(r.pieces)
                          .arg(r.nx)
                          .arg(r.ny)
                          .arg(r.nz)
                          .arg(r.cell.x * 1000, 0, 'f', 1)
                          .arg(r.cell.y * 1000, 0, 'f', 1)
                          .arg(r.cell.z * 1000, 0, 'f', 1);
            if (p.connector != SlicerParams::None) message += QStringLiteral(", %1 connectors").arg(r.connectors);
            return true;
        };
        r.push_back(std::move(d));
    }
    {
        EffectDef d;
        d.name = d.menuText = QStringLiteral("Merge Flat Faces");
        d.submenu = QStringLiteral("Polygons");
        d.tip = QStringLiteral("Joins connected faces lying in one plane with the same materials into single n-gons.");
        d.apply = [](Mesh& m, const std::set<Id>& f, const QVariantMap&, EffectResult* out, std::string* err) {
            return effectMergeFlat(m, f, out, err);
        };
        r.push_back(std::move(d));
    }
    return r;
}

} // namespace

QString EffectDef::key() const { return QStringLiteral("effect_") + QString(name).toLower().replace(' ', '_'); }

const std::vector<EffectDef>& effectRegistry() {
    static const std::vector<EffectDef> reg = buildRegistry();
    return reg;
}

EffectDialog::EffectDialog(const EffectDef& def, const QVariantMap& values, Preview preview, QWidget* parent)
    : QDialog(parent), values_(values), preview_(std::move(preview)) {
    setWindowTitle(def.name);
    timer_.setSingleShot(true);
    timer_.setInterval(120);
    connect(&timer_, &QTimer::timeout, this, &EffectDialog::runPreview);

    auto* top = new QVBoxLayout(this);
    if (!def.tip.isEmpty()) {
        auto* tip = new QLabel(def.tip, this);
        tip->setWordWrap(true);
        tip->setProperty("hint", true);
        top->addWidget(tip);
    }
    auto* form = new QFormLayout();
    top->addLayout(form);
    auto schedule = [this] { timer_.start(); };

    for (const EffectParam& p : def.params) {
        const QString key = p.key;
        QWidget* tipWidget = nullptr;
        switch (p.type) {
        case EffectParam::Double:
        case EffectParam::Int: {
            auto* row = new QWidget(this);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* slider = new QSlider(Qt::Horizontal, row);
            constexpr int kSteps = 1000;
            slider->setRange(0, kSteps);
            slider->setMinimumWidth(200);
            auto* spin = new QDoubleSpinBox(row);
            spin->setDecimals(p.decimals);
            spin->setRange(p.min, p.max);
            spin->setSingleStep(p.decimals ? std::pow(10.0, -p.decimals + 1) : 1);
            spin->setSuffix(p.suffix);
            spin->setFixedWidth(104);
            auto* reset = new QToolButton(row);
            reset->setText(QString::fromUtf8("↺"));
            reset->setToolTip(QStringLiteral("Reset"));
            reset->setAutoRaise(true);
            const bool log = p.logScale && p.min > 0;
            const double lo = p.min, hi = p.max;
            auto toSlider = [=](double x) {
                const double t = log ? std::log(x / lo) / std::log(hi / lo) : (x - lo) / (hi - lo);
                return (int)std::lround(std::clamp(t, 0.0, 1.0) * kSteps);
            };
            auto fromSlider = [=](int s) {
                const double t = double(s) / kSteps;
                return log ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t;
            };
            const double cur = values_.value(key, p.def).toDouble();
            spin->setValue(cur);
            slider->setValue(toSlider(cur));
            connect(slider, &QSlider::valueChanged, spin, [spin, fromSlider](int s) { spin->setValue(fromSlider(s)); });
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, slider, key, toSlider, schedule, isInt = p.type == EffectParam::Int](double x) {
                QSignalBlocker block(slider);
                slider->setValue(toSlider(x));
                values_[key] = isInt ? QVariant((int)std::lround(x)) : QVariant(x);
                schedule();
            });
            const double defValue = p.def;
            connect(reset, &QToolButton::clicked, spin, [spin, defValue] { spin->setValue(defValue); });
            h->addWidget(slider, 1);
            h->addWidget(spin);
            h->addWidget(reset);
            form->addRow(p.label + QLatin1Char(':'), row);
            tipWidget = row;
            break;
        }
        case EffectParam::Bool: {
            auto* cb = new QCheckBox(p.label, this);
            cb->setChecked(values_.value(key, p.def != 0).toBool());
            connect(cb, &QCheckBox::toggled, this, [this, key, schedule](bool on) {
                values_[key] = on;
                schedule();
            });
            form->addRow(QString(), cb);
            tipWidget = cb;
            break;
        }
        case EffectParam::Choice: {
            auto* combo = new QComboBox(this);
            combo->addItems(p.choices);
            combo->setCurrentIndex(values_.value(key, (int)p.def).toInt());
            connect(combo, &QComboBox::currentIndexChanged, this, [this, key, schedule](int i) {
                values_[key] = i;
                schedule();
            });
            form->addRow(p.label + QLatin1Char(':'), combo);
            tipWidget = combo;
            break;
        }
        case EffectParam::Seed: {
            auto* btn = new QPushButton(QStringLiteral("New Pattern"), this);
            connect(btn, &QPushButton::clicked, this, [this, key, schedule] {
                values_[key] = (int)QRandomGenerator::global()->bounded(100000);
                schedule();
            });
            form->addRow(p.label + QLatin1Char(':'), btn);
            tipWidget = btn;
            break;
        }
        }
        if (tipWidget && !p.tip.isEmpty()) tipWidget->setToolTip(p.tip);
    }

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    top->addWidget(message_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    top->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        // Settings changed a moment ago must be what gets kept.
        if (timer_.isActive()) {
            timer_.stop();
            runPreview();
        }
        if (buttons_->button(QDialogButtonBox::Ok)->isEnabled()) accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    runPreview();
}

void EffectDialog::runPreview() {
    bool ok = false;
    message_->setText(preview_(values_, ok));
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(ok);
}

} // namespace gs
