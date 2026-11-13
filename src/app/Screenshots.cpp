// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - renders the pictures of the README (`--screenshots DIR`): a small
// house built with the real tools' kernel calls, the window with its panels,
// cinematic lighting, and frame sequences for animated GIFs (tools/readme-media.sh
// runs it on a virtual screen and assembles the GIFs).
#include "SelfTest.h"

#include "Animator.h"
#include "MainWindow.h"
#include "Viewport.h"
#include "core/Components.h"
#include "core/Effects.h"
#include "core/Primitives.h"
#include "core/FollowMe.h"
#include "core/RoundCorners.h"
#include "core/Solids.h"
#include "core/Terrain.h"
#include "GameMode.h"
#include "io/NativeFormat.h"
#include "io/ArchMaterials.h"
#include <QTemporaryDir>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>

#include <cstdio>

namespace gs {

namespace {

void pump(int ms = 60) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
}

Id facesWhere(const Mesh& m, const std::function<bool(Id)>& pick) {
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && pick(f)) return f;
    return kNone;
}

Id addColor(Document& doc, const char* name, Color c) {
    Material m;
    m.name = name;
    m.color = c;
    return doc.addMaterial(m);
}

// A small house as a component: walls, a gable roof, a door and windows.
void buildHouse(Document& doc) {
    const Id walls = addColor(doc, "Walls", Color::rgb8(232, 222, 200));
    const Id roof = addColor(doc, "Roof", Color::rgb8(170, 70, 55));
    const Id door = addColor(doc, "Door", Color::rgb8(110, 75, 50));
    const Id glass = addColor(doc, "Glass", Color::rgb8(120, 170, 210));
    Mesh m;
    m.viewHint = Vec3(5, -10, 8);
    m.groundFacesDown = false;
    // Up from the ground (whichever way the new face points).
    for (double sign : {1.0, -1.0}) {
        m = Mesh();
        m.viewHint = Vec3(5, -10, 8);
        m.groundFacesDown = false;
        m.drawPolyline({{0, 0, 0}, {6, 0, 0}, {6, 4, 0}, {0, 4, 0}}, true);
        const Id base = facesWhere(m, [&](Id) { return true; });
        m.pushPull(base, 3 * sign);
        Vec3 mn, mx;
        bool any = false;
        m.bounds(mn, mx, any);
        if (mx.z > 2.9) break;
    }
    // Gable: the ridge line across the top, lifted.
    m.drawLine({0, 2, 3}, {6, 2, 3});
    std::set<Id> ridge = {m.findVertex({0, 2, 3}), m.findVertex({6, 2, 3})};
    m.transformVertices(ridge, Mat4::translation({0, 0, 1.6}), true);
    // Door and windows on the front wall (y = 0), pushed in.
    auto opening = [&](double x0, double z0, double x1, double z1, Id mat, double depth) {
        m.drawPolyline({{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}}, true);
        const Id f = facesWhere(m, [&](Id g) { return std::fabs(m.faces[g].normal.y) > 0.99 && m.pointInFace(g, {(x0 + x1) / 2, 0, (z0 + z1) / 2}, false); });
        if (f == kNone) return;
        const Id moved = m.pushPull(f, -depth);
        if (moved != kNone) m.faces[moved].front.material = m.faces[moved].back.material = mat;
    };
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) m.faces[f].front.material = m.faces[f].back.material = walls;
    opening(2.6, 0.01, 3.6, 2.1, door, 0.08);
    opening(0.7, 1.0, 1.9, 2.1, glass, 0.12);
    opening(4.3, 1.0, 5.5, 2.1, glass, 0.12);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && m.faceInteriorPoint(f).z > 3 && std::fabs(m.faces[f].normal.z) > 0.2)
            m.faces[f].front.material = m.faces[f].back.material = roof;
    Selection sel;
    appendMesh(doc.mesh(), m, Mat4(), nullptr, nullptr, false, &sel);
    makeComponent(doc.state, sel, "House");
    ++doc.materialsRevision;
}


// ---- small builders for the documentation scenes ------------------------------

std::set<Id> allFaces(const Mesh& m) {
    std::set<Id> f;
    for (Id i = 0; i < (Id)m.faces.size(); ++i)
        if (m.faces[i].alive) f.insert(i);
    return f;
}

void paintAll(Mesh& m, Id mat, const std::set<Id>* only = nullptr) {
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && (!only || only->count(f))) m.faces[f].front.material = m.faces[f].back.material = mat;
}

// A box standing on z = lo.z (push/pull upwards whichever way the face points).
Mesh boxMesh(const Vec3& lo, const Vec3& hi) {
    Mesh m;
    m.groundFacesDown = false;
    m.drawPolyline({{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}}, true);
    Id f = kNone;
    for (Id i = 0; i < (Id)m.faces.size(); ++i)
        if (m.faces[i].alive) f = i;
    const double h = hi.z - lo.z;
    Mesh up = m;
    up.pushPull(f, h);
    Vec3 mn, mx;
    bool any;
    up.bounds(mn, mx, any);
    if (mx.z > hi.z - 1e-6) return up;
    m.pushPull(f, -h);
    return m;
}

Mesh primitive(PrimitiveKind k, double r, double r2 = 0.3) {
    PrimitiveParams p;
    p.kind = k;
    p.radius = r;
    p.radius2 = r2;
    p.width = p.depth = p.height = r * 2;
    p.segments = 32;
    p.rings = 16;
    p.frequency = 3;
    Mesh m;
    buildPrimitive(p, m);
    return m;
}

void place(Mesh& dst, const Mesh& src, const Mat4& xf) { appendMesh(dst, src, xf); }

Mesh bentStairs() {
    Document tmp;
    QString err;
    if (!loadNative(tmp, QStringLiteral(GNUSKETCH_SOURCE_DIR "/samples/stairs.gsketch"), nullptr, &err)) return Mesh();
    Mesh m = flattenModel(tmp.state);
    Vec3 mn, mx;
    bool any;
    m.bounds(mn, mx, any);
    BendParams bp;
    // Along the longer side, around the vertical.
    const bool alongX = mx.x - mn.x >= mx.y - mn.y;
    const Vec3 a(mn.x, mn.y, mn.z), b = alongX ? Vec3(mx.x, mn.y, mn.z) : Vec3(mn.x, mx.y, mn.z);
    if (makeBend(a, b, Vec3(0, 0, 1), kPi / 2, 24, bp)) effectBend(m, allFaces(m), nullptr, bp);
    m.bounds(mn, mx, any);
    Mesh out;
    appendMesh(out, m, Mat4::translation(-Vec3(mn.x, mn.y, mn.z)));
    return out;
}

Mesh pipe() {
    Mesh m;
    m.groundFacesDown = false;
    std::vector<Vec3> circle;
    for (int i = 0; i < 24; ++i) circle.push_back(Vec3(0.25 * std::cos(i * kPi / 12), 0, 0.5 + 0.25 * std::sin(i * kPi / 12)));
    m.drawPolyline(circle, true);
    const Id profile = *allFaces(m).begin();
    const std::vector<Id> path = m.drawPolyline({{0, 0, 0.5}, {0, 3, 0.5}, {2.5, 3, 0.5}, {2.5, 3, 2.5}}, false);
    followMe(m, profile, std::set<Id>(path.begin(), path.end()));
    return m;
}

Mesh roundedBox() {
    Mesh m = boxMesh({0, 0, 0}, {1.6, 1.6, 1.2});
    std::set<Id> edges;
    for (Id e = 0; e < (Id)m.edges.size(); ++e)
        if (m.edges[e].alive) edges.insert(e);
    roundCorners(m, edges, 0.25, 6);
    return m;
}

Mesh noisyGround(double size) {
    Mesh m;
    m.groundFacesDown = false;
    m.drawPolyline({{0, 0, 0}, {size, 0, 0}, {size, size, 0}, {0, size, 0}}, true);
    NoiseParams np;
    np.x = np.y = false;
    np.amount = 0.6;
    np.scale = 2.5;
    np.detail = 0.3;
    effectNoise(m, allFaces(m), np);
    return m;
}


// A material from the built-in PBR library, added to the model.
Id libMaterial(Document& doc, const char* name) {
    for (int i = 0; i < architecturalMaterialCount(); ++i)
        if (architecturalMaterialName(i) == name) return doc.addMaterial(architecturalMaterial(i));
    return kNone;
}

// An H beam (I section) swept along a rising curve, built up to `upTo` of it.
Mesh hBeam(double upTo) {
    Mesh m;
    m.groundFacesDown = false;
    const double R = 3, rise = 2.2, turn = kPi * 0.9;
    const int n = 32;
    auto at = [&](double t) { return Vec3(R * std::cos(t * turn), R * std::sin(t * turn), 0.6 + rise * t * t); };
    const int count = std::max(1, (int)std::lround(n * upTo));
    std::vector<Vec3> path;
    for (int i = 0; i <= count; ++i) path.push_back(at(i / double(n)));
    // The profile at the start, square to the first piece of the path.
    const Vec3 d = (path[1] - path[0]).normalized();
    const Vec3 u = Vec3(1, 0, 0) - d * d.x;
    const Vec3 un = u.normalized(), vn = d.cross(un).normalized();
    const double w = 0.22, h = 0.3, tw = 0.035, tf = 0.04; // half width/height, web and flange
    const double pts[12][2] = {{-w, -h}, {w, -h}, {w, -h + tf}, {tw, -h + tf}, {tw, h - tf}, {w, h - tf},
                               {w, h}, {-w, h}, {-w, h - tf}, {-tw, h - tf}, {-tw, -h + tf}, {-w, -h + tf}};
    std::vector<Vec3> prof;
    for (const auto& q : pts) prof.push_back(path[0] + un * q[1] + vn * q[0]); // web upright
    m.drawPolyline(prof, true);
    const Id face = *allFaces(m).begin();
    const std::vector<Id> pe = m.drawPolyline(path, false);
    followMe(m, face, std::set<Id>(pe.begin(), pe.end()));
    return m;
}

void save(const QImage& img, const QString& path) {
    img.save(path);
    std::printf("screenshots: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
}

} // namespace

int runScreenshots(MainWindow& w, const QString& dirPath) {
    QDir dir(dirPath);
    dir.mkpath(QStringLiteral("."));
    auto* vp = w.findChild<Viewport*>();
    if (!vp || !vp->renderOk()) return 1;
    Document& doc = vp->doc();
    w.resize(1500, 920);
    pump(300);

    // 1. The window: the house, its panels and the Animator.
    doc.reset();
    buildHouse(doc);
    doc.style.sun = true;
    doc.style.shadows = true;
    doc.style.ambientOcclusion = true;
    doc.style.sunAzimuth = 220;
    doc.style.sunAltitude = 38;
    doc.notifyChanged();
    vp->setStandardView(Vec3(-0.55, 1, -0.6).normalized());
    vp->zoomExtents();
    if (auto* animDock = w.findChild<QDockWidget*>(QStringLiteral("AnimatorPanel"))) animDock->show();
    // A component to animate: a box rolling across the lawn.
    {
        Mesh crate;
        PrimitiveParams bp;
        bp.kind = PrimitiveKind::Box;
        bp.width = bp.depth = bp.height = 0.8;
        buildPrimitive(bp, crate);
        Selection sel;
        Mesh& m = doc.mesh();
        appendMesh(m, crate, Mat4::translation({8, -2.5, 0}), nullptr, nullptr, false, &sel);
        doc.beginOp("Crate");
        const Id inst = makeComponent(doc.state, sel, "Crate");
        doc.commitOp();
        doc.selection.clear();
        doc.selection.instances.insert(inst);
        doc.notifySelection();
        if (auto* anim = w.findChild<AnimatorPanel*>()) {
            anim->modelReplaced();
            anim->addLayersFromSelection();
            anim->setFrame(40);
            doc.beginOp("Move");
            const Vec3 c(8, -2.5, 0.4);
            doc.state.instances[inst].xf = Mat4::translation({0, 6, 0}) * Mat4::translation(c) * Mat4::rotation({1, 0, 0}, -kPi) *
                                           Mat4::translation(-c) * doc.state.instances[inst].xf;
            doc.commitOp();
            anim->setFrame(18);
        }
    }
    pump(400);
    save(w.grab().toImage(), dir.filePath(QStringLiteral("window.png")));

    // 2. Animator frames (the viewport) for animator.gif.
    if (auto* anim = w.findChild<AnimatorPanel*>()) {
        for (int f = 1; f <= 40; f += 1) {
            anim->setFrame(f);
            pump(30);
            save(vp->grabFramebuffer(), dir.filePath(QStringLiteral("animator_%1.png").arg(f, 3, 10, QLatin1Char('0'))));
        }
        anim->setFrame(1);
    }
    if (auto* animDock = w.findChild<QDockWidget*>(QStringLiteral("AnimatorPanel"))) animDock->hide();

    // 3. Cinematic lighting: ray traced shadows and light rays.
    doc.style.cinematic = true;
    doc.style.lightRays = 0.8;
    doc.style.sunAltitude = 24;
    doc.style.sunAzimuth = 250;
    doc.selection.clear();
    doc.notifySelection();
    vp->setStandardView(Vec3(-0.9, 1, -0.35).normalized());
    vp->zoomExtents();
    pump(600);
    save(vp->grabFramebuffer(), dir.filePath(QStringLiteral("cinematic.png")));
    doc.style.cinematic = false;

    // 4. Push/Pull frames: a rectangle grows into a tower next to the house.
    {
        Mesh& m = doc.mesh();
        m.drawPolyline({{8, 3, 0}, {10, 3, 0}, {10, 6, 0}, {8, 6, 0}}, true);
        doc.state.timeline.layers.clear();
        doc.notifyChanged();
        vp->setStandardView(Vec3(-0.55, 1, -0.6).normalized());
        vp->zoomExtents();
        pump(200);
        for (int i = 0; i <= 24; ++i) {
            const double h = 0.2 * i;
            doc.beginOp("Push/Pull");
            Mesh& mm = doc.mesh();
            const Id f = facesWhere(mm, [&](Id g) { return mm.pointInFace(g, {9, 4.5, 0}, false) && std::fabs(mm.faces[g].normal.z) > 0.99; });
            if (f != kNone && h > 0) {
                const Id moved = mm.pushPull(f, h);
                Vec3 mn, mx;
                bool any = false;
                mm.bounds(mn, mx, any);
                if (moved == kNone || mn.z < -0.01) { // went down: the other way
                    doc.cancelOp();
                    doc.beginOp("Push/Pull");
                    Mesh& m2 = doc.mesh();
                    const Id g = facesWhere(m2, [&](Id x) { return m2.pointInFace(x, {9, 4.5, 0}, false) && std::fabs(m2.faces[x].normal.z) > 0.99; });
                    if (g != kNone) m2.pushPull(g, -h);
                }
            }
            vp->update();
            pump(40);
            save(vp->grabFramebuffer(), dir.filePath(QStringLiteral("pushpull_%1.png").arg(i, 3, 10, QLatin1Char('0'))));
            doc.cancelOp();
        }
    }

    // 5. Hollow: a cut-away sphere for 3D printing.
    {
        doc.reset();
        Mesh& m = doc.mesh();
        PrimitiveParams sp;
        sp.kind = PrimitiveKind::Sphere;
        sp.radius = 1;
        sp.segments = 48;
        sp.rings = 24;
        buildPrimitive(sp, m);
        std::set<Id> all;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) all.insert(f);
        HollowParams hp;
        hp.thickness = 0.15;
        hp.detail = 64;
        effectHollow(m, all, hp);
        // Cut the front half away to show the wall.
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faceInteriorPoint(f).y < -0.02) m.eraseFace(f);
        m.touch();
        doc.style = RenderStyle();
        doc.style.sun = true;
        doc.style.shadows = true;
        doc.notifyChanged();
        vp->setStandardView(Vec3(-0.5, 1, -0.55).normalized());
        vp->zoomExtents();
        pump(300);
        save(vp->grabFramebuffer(), dir.filePath(QStringLiteral("hollow.png")));
    }
    // ---- Documentation scenes -----------------------------------------------
    // A wide viewport: no panels.
    for (QDockWidget* dk : w.findChildren<QDockWidget*>()) dk->hide();
    w.resize(1500, 900);
    pump(300);
    auto frame = [&](const QString& name) {
        vp->update();
        pump(40);
        save(vp->grabFramebuffer(), dir.filePath(name + QStringLiteral(".png")));
    };
    auto seq = [&](const QString& name, int i) { frame(QStringLiteral("%1_%2").arg(name).arg(i, 3, 10, QLatin1Char('0'))); };
    auto fresh = [&](bool shadows = true) {
        doc.reset();
        doc.style = RenderStyle();
        doc.style.sun = true;
        doc.style.shadows = shadows;
        doc.style.sunAzimuth = 230;
        doc.style.sunAltitude = 40;
        doc.style.showAxes = false;
        doc.mesh().groundFacesDown = false;
        doc.mesh().viewHint = Vec3(-5, -10, 8);
    };
    auto look = [&](const Vec3& dir, double closer = 0.72) {
        doc.notifyChanged();
        vp->setStandardView(dir.normalized());
        vp->zoomExtents();
        Camera& c = vp->camera();
        c.eye = c.target + (c.eye - c.target) * closer;
        pump(80);
    };
    const Vec3 iso(-0.55, 1, -0.6);

    // Hero: a little of everything, cinematic light.
    {
        fresh();
        Mesh& m = doc.mesh();
        const Id stone = addColor(doc, "Stone", Color::rgb8(190, 185, 175));
        const Id grass = addColor(doc, "Grass", Color::rgb8(105, 150, 75));
        const Id metal = addColor(doc, "Copper", Color::rgb8(196, 120, 70));
        const Id blue = addColor(doc, "Blue", Color::rgb8(70, 120, 200));
        const Id gold = addColor(doc, "Gold", Color::rgb8(230, 180, 60));
        const Id white = addColor(doc, "White", Color::rgb8(240, 240, 235));
        buildHouse(doc);
        Mesh part;
        part = bentStairs();
        paintAll(part, stone);
        place(m, part, Mat4::translation({8, 1, 0}));
        part = noisyGround(7);
        paintAll(part, grass);
        place(m, part, Mat4::translation({-9, -1, 0}));
        part = pipe();
        paintAll(part, metal);
        place(m, part, Mat4::translation({-1, -6, 0}));
        part = roundedBox();
        paintAll(part, white);
        place(m, part, Mat4::translation({3, -6.5, 0}));
        const struct { PrimitiveKind k; Id mat; double r; } prims[] = {
            {PrimitiveKind::Sphere, blue, 0.7}, {PrimitiveKind::Torus, gold, 0.7}, {PrimitiveKind::Dodecahedron, stone, 0.7},
            {PrimitiveKind::Cone, metal, 0.7}, {PrimitiveKind::GeoSphere, white, 0.7}};
        for (int i = 0; i < 5; ++i) {
            part = primitive(prims[i].k, prims[i].r, 0.25);
            paintAll(part, prims[i].mat);
            place(m, part, Mat4::translation({6.5 + i * 1.9, -6, 0}));
        }
        part = primitive(PrimitiveKind::Sphere, 1.0);
        VoxelateParams vx;
        vx.size = 0.25;
        effectVoxelate(part, allFaces(part), vx);
        paintAll(part, gold);
        place(m, part, Mat4::translation({-5.5, -7, 0}));
        m.touch();
        ++doc.materialsRevision;
        doc.style.cinematic = true;
        doc.style.lightRays = 0.4;
        doc.style.ambientOcclusion = true;
        doc.style.sunAltitude = 30;
        look(Vec3(-0.35, 1, -0.5), 0.62);
        frame(QStringLiteral("hero"));
        doc.style.cinematic = false;
    }

    // Line: a closed outline becomes a face.
    {
        fresh(false);
        const std::vector<Vec3> pts = {{0, 0, 0}, {3, 0, 0}, {3, 2, 0}, {1.5, 3, 0}, {0, 2, 0}, {0, 0, 0}};
        doc.mesh().drawLine(pts[0], pts[1]);
        look(iso);
        doc.mesh() = Mesh();
        doc.mesh().groundFacesDown = false;
        for (int i = 0; i + 1 < (int)pts.size(); ++i)
            for (int k = 1; k <= 6; ++k) {
                Mesh& m = doc.mesh();
                const Vec3 b = pts[i] + (pts[i + 1] - pts[i]) * (k / 6.0);
                Mesh before = m;
                m.drawLine(pts[i], b);
                seq(QStringLiteral("line"), i * 6 + k);
                if (k < 6) m = before;
            }
    }
    // Rectangle, then Circle and Polygon growing.
    {
        fresh(false);
        doc.mesh().drawPolyline({{0, 0, 0}, {3, 0, 0}, {3, 2, 0}, {0, 2, 0}}, true);
        look(iso);
        for (int i = 1; i <= 20; ++i) {
            doc.mesh() = Mesh();
            const double t = i / 20.0;
            doc.mesh().drawPolyline({{0, 0, 0}, {3 * t, 0, 0}, {3 * t, 2 * t, 0}, {0, 2 * t, 0}}, true);
            seq(QStringLiteral("rectangle"), i);
        }
        for (int i = 1; i <= 20; ++i) {
            doc.mesh() = Mesh();
            const double r = 1.2 * i / 20.0;
            std::vector<Vec3> c, h;
            for (int k = 0; k < 32; ++k) c.push_back(Vec3(1.5 + r * std::cos(k * kPi / 16), 1 + r * std::sin(k * kPi / 16), 0));
            for (int k = 0; k < 6; ++k) h.push_back(Vec3(4.5 + r * std::cos(k * kPi / 3), 1 + r * std::sin(k * kPi / 3), 0));
            doc.mesh().drawPolyline(c, true);
            doc.mesh().drawPolyline(h, true);
            seq(QStringLiteral("circle"), i);
        }
    }
    // Move (with a copy), Rotate and Scale a box.
    {
        fresh();
        const Id red = addColor(doc, "Red", Color::rgb8(200, 80, 70));
        Mesh b = boxMesh({0, 0, 0}, {1, 1, 1});
        paintAll(b, red);
        doc.mesh() = b;
        place(doc.mesh(), b, Mat4::translation({4, 2, 0}));
        look(iso);
        for (int i = 0; i <= 20; ++i) {
            doc.mesh() = b;
            place(doc.mesh(), b, Mat4::translation(Vec3(4, 2, 0) * (i / 20.0)));
            seq(QStringLiteral("move"), i);
        }
        doc.mesh() = b;
        look(iso);
        for (int i = 0; i <= 24; ++i) {
            doc.mesh() = Mesh();
            place(doc.mesh(), b, Mat4::translation({0.5, 0.5, 0}) * Mat4::rotation({0, 0, 1}, kPi / 2 * i / 24.0) * Mat4::translation({-0.5, -0.5, 0}));
            seq(QStringLiteral("rotate"), i);
        }
        doc.mesh() = b;
        place(doc.mesh(), b, Mat4::scale({2, 1.5, 2}));
        look(iso);
        for (int i = 0; i <= 20; ++i) {
            const double t = 1 + i / 20.0;
            doc.mesh() = Mesh();
            place(doc.mesh(), b, Mat4::scale({t, 1 + (t - 1) * 0.5, t}));
            seq(QStringLiteral("scale"), i);
        }
    }
    // Offset: an inset outline, pushed in.
    {
        fresh();
        const Id wood = addColor(doc, "Wood", Color::rgb8(170, 120, 70));
        Mesh slab = boxMesh({0, 0, 0}, {3, 2, 0.3});
        paintAll(slab, wood);
        doc.mesh() = slab;
        look(iso);
        for (int i = 0; i <= 20; ++i) {
            doc.mesh() = slab;
            Mesh& m = doc.mesh();
            const double d = 0.4 * std::min(1.0, i / 12.0);
            if (d > 0.01) m.drawPolyline({{d, d, 0.3}, {3 - d, d, 0.3}, {3 - d, 2 - d, 0.3}, {d, 2 - d, 0.3}}, true);
            if (i > 12) {
                for (Id f = 0; f < (Id)m.faces.size(); ++f)
                    if (m.faces[f].alive && m.faces[f].normal.z > 0.99 && m.pointInFace(f, {1.5, 1, 0.3}, false)) {
                        m.pushPull(f, -0.2 * (i - 12) / 8.0);
                        break;
                    }
            }
            seq(QStringLiteral("offset"), i);
        }
    }
    // Bend: straight stairs curving round.
    {
        fresh();
        Document tmp;
        QString err;
        loadNative(tmp, QStringLiteral(GNUSKETCH_SOURCE_DIR "/samples/stairs.gsketch"), nullptr, &err);
        const Mesh straight = flattenModel(tmp.state);
        Vec3 mn, mx;
        bool any;
        straight.bounds(mn, mx, any);
        const bool alongX = mx.x - mn.x >= mx.y - mn.y;
        const Vec3 a(mn.x, mn.y, mn.z), bEnd = alongX ? Vec3(mx.x, mn.y, mn.z) : Vec3(mn.x, mx.y, mn.z);
        const Id stone = addColor(doc, "Stone", Color::rgb8(190, 185, 175));
        doc.mesh() = straight;
        paintAll(doc.mesh(), stone);
        look(iso);
        for (int i = 0; i <= 24; ++i) {
            Mesh m = straight;
            BendParams bp;
            if (i > 0 && makeBend(a, bEnd, Vec3(0, 0, 1), kPi / 2 * i / 24.0, 24, bp)) effectBend(m, allFaces(m), nullptr, bp);
            paintAll(m, stone);
            doc.mesh() = m;
            seq(QStringLiteral("bend"), i);
        }
    }
    // Terraform: a mountain rising under the brush.
    {
        fresh();
        const Id grass = addColor(doc, "Grass", Color::rgb8(105, 150, 75));
        Mesh& m = doc.mesh();
        m.drawPolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}, true);
        paintAll(m, grass);
        look(Vec3(-0.5, 1, -0.55));
        TerrainBrush brush;
        brush.radius = 3;
        brush.force = 1.2;
        brush.hardness = 0.3;
        TerrainStroke stroke(TerrainMode::Raise, brush);
        stroke.begin(m, {5, 5, 0}, *allFaces(m).begin(), {0, 0, 1});
        for (int i = 0; i <= 24; ++i) {
            if (i > 0) stroke.apply(m, {5 + std::sin(i * 0.4), 5, 0}, 0.08);
            paintAll(m, grass);
            m.touch();
            seq(QStringLiteral("terraform"), i);
        }
    }
    // Still pictures: Follow Me, Round Corners, primitives, solid tools, effects.
    auto still = [&](const QString& name, const std::function<void(Mesh&)>& build, Vec3 view = Vec3(-0.55, 1, -0.6)) {
        fresh();
        build(doc.mesh());
        doc.mesh().touch();
        ++doc.materialsRevision;
        look(view);
        frame(name);
    };
    const Id cMetal = kNone;
    (void)cMetal;
    // Follow Me: an H beam swept along a rising curve.
    {
        fresh();
        const Id steel = libMaterial(doc, "Brushed Steel");
        doc.mesh() = hBeam(1.0);
        paintAll(doc.mesh(), steel);
        ++doc.materialsRevision;
        look(Vec3(-0.6, 1, -0.55), 0.8);
        for (int i = 1; i <= 24; ++i) {
            doc.mesh() = hBeam(i / 24.0);
            paintAll(doc.mesh(), steel);
            seq(QStringLiteral("followme"), i);
        }
    }
    still(QStringLiteral("roundcorners"), [&](Mesh& m) {
        const Id white = addColor(doc, "White", Color::rgb8(235, 235, 230));
        Mesh sharp = boxMesh({0, 0, 0}, {1.6, 1.6, 1.2}), round = roundedBox();
        paintAll(sharp, white);
        paintAll(round, white);
        m = sharp;
        place(m, round, Mat4::translation({2.5, 0, 0}));
    });
    still(QStringLiteral("primitives"), [&](Mesh& m) {
        const PrimitiveKind kinds[] = {PrimitiveKind::Box, PrimitiveKind::Sphere, PrimitiveKind::GeoSphere, PrimitiveKind::Cylinder, PrimitiveKind::Cone,
                                       PrimitiveKind::Torus, PrimitiveKind::Tetrahedron, PrimitiveKind::Octahedron, PrimitiveKind::Dodecahedron,
                                       PrimitiveKind::Icosahedron};
        const Color cols[] = {Color::rgb8(200, 80, 70), Color::rgb8(70, 120, 200), Color::rgb8(230, 180, 60), Color::rgb8(105, 150, 75),
                              Color::rgb8(196, 120, 70), Color::rgb8(160, 90, 190), Color::rgb8(200, 80, 70), Color::rgb8(70, 120, 200),
                              Color::rgb8(230, 180, 60), Color::rgb8(105, 150, 75)};
        for (int i = 0; i < 10; ++i) {
            Mesh p = primitive(kinds[i], 0.6, 0.2);
            paintAll(p, addColor(doc, primitiveName(kinds[i]), cols[i]));
            place(m, p, Mat4::translation({(i % 5) * 1.8, (i / 5) * 1.8, 0}));
        }
    }, Vec3(-0.3, 1, -0.75));
    still(QStringLiteral("solidtools"), [&](Mesh& m) {
        const Id a = addColor(doc, "A", Color::rgb8(200, 80, 70)), b = addColor(doc, "B", Color::rgb8(70, 120, 200));
        Mesh box = boxMesh({0, 0, 0}, {1.2, 1.2, 1.2}), ball = primitive(PrimitiveKind::Sphere, 0.75);
        paintAll(box, a);
        paintAll(ball, b);
        Mesh ballAt;
        place(ballAt, ball, Mat4::translation({1.1, 1.1, 0.45}));
        const BoolOp ops[] = {BoolOp::Union, BoolOp::Difference, BoolOp::Intersect};
        for (int i = 0; i < 3; ++i) {
            Mesh r;
            if (booleanMeshes(box, ballAt, ops[i], r)) place(m, r, Mat4::translation({i * 3.0, 0, 0}));
        }
    });
    auto effectStill = [&](const QString& name, const std::function<void(Mesh&)>& fx) {
        still(name, [&](Mesh& m) {
            const Id c1 = addColor(doc, "Before", Color::rgb8(190, 190, 185)), c2 = addColor(doc, "After", Color::rgb8(90, 140, 210));
            Mesh before = primitive(PrimitiveKind::Sphere, 1.0), after = before;
            fx(after);
            paintAll(before, c1);
            paintAll(after, c2);
            m = before;
            place(m, after, Mat4::translation({2.8, 0, 0}));
        });
    };
    effectStill(QStringLiteral("fx_noise"), [](Mesh& m) {
        NoiseParams p;
        p.amount = 0.15;
        p.scale = 0.5;
        p.detail = 0.08;
        effectNoise(m, allFaces(m), p);
    });
    effectStill(QStringLiteral("fx_voxelate"), [](Mesh& m) {
        VoxelateParams p;
        p.size = 0.2;
        effectVoxelate(m, allFaces(m), p);
    });
    effectStill(QStringLiteral("fx_remesh"), [](Mesh& m) {
        RemeshParams p;
        p.shape = RemeshParams::Hexagons;
        p.cellsPerMeter = 4;
        effectRemesh(m, allFaces(m), p);
    });
    effectStill(QStringLiteral("fx_detriangulate"), [](Mesh& m) {
        effectTriangulate(m, allFaces(m));
        DetriangulateParams p;
        p.quads = true;
        effectDetriangulate(m, allFaces(m), p);
    });

    // Lights at night: coloured lamps over shapes and a terrain.
    {
        fresh();
        doc.style.sunAltitude = -20;
        doc.style.cinematic = true;
        doc.style.lightRays = 0.7;
        doc.style.showObjects = false;
        const Id white = addColor(doc, "White", Color::rgb8(235, 235, 230));
        Mesh& m = doc.mesh();
        Mesh g = noisyGround(12);
        paintAll(g, white);
        place(m, g, Mat4::translation({-6, -6, 0}));
        const PrimitiveKind kinds[] = {PrimitiveKind::Sphere, PrimitiveKind::Torus, PrimitiveKind::GeoSphere, PrimitiveKind::Dodecahedron};
        const Vec3 spots[] = {{-2, -1.5, 0.6}, {1.5, -2, 0.6}, {-1.5, 2, 0.6}, {2, 1.5, 0.6}};
        for (int i = 0; i < 4; ++i) {
            Mesh p = primitive(kinds[i], 0.9, 0.3);
            paintAll(p, white);
            place(m, p, Mat4::translation(spots[i]));
        }
        m.touch();
        ++doc.materialsRevision;
        const struct { Vec3 pos; Color c; } lamps[] = {
            {{-3, 0, 2.2}, Color(1.0f, 0.25f, 0.2f)}, {{3, -1, 2.2}, Color(0.2f, 0.45f, 1.0f)}, {{0, 3, 2.2}, Color(0.3f, 1.0f, 0.4f)}};
        for (const auto& l : lamps) {
            SceneObject o;
            o.pos = l.pos;
            o.color = l.c;
            o.intensity = 9;
            o.range = 9;
            doc.addObject(o);
        }
        SceneObject spot;
        spot.kind = SceneObject::SpotLight;
        spot.pos = Vec3(0, -3, 5);
        spot.target = Vec3(0, 0, 0);
        spot.color = Color(1.0f, 0.85f, 0.55f);
        spot.intensity = 18;
        spot.range = 14;
        spot.coneAngle = 40;
        doc.addObject(spot);
        look(Vec3(-0.3, 1, -0.6), 0.7);
        frame(QStringLiteral("lights"));
        doc.style.cinematic = false;
    }

    // PBR materials in a low sun: depth from the normal and height maps.
    {
        fresh();
        doc.style.sunAltitude = 18;
        doc.style.sunAzimuth = 250;
        doc.style.ambientOcclusion = true;
        Mesh& m = doc.mesh();
        auto box = [&](Vec3 lo, Vec3 hi, Id mat) {
            Mesh b = boxMesh(lo, hi);
            paintAll(b, mat);
            place(m, b, Mat4());
        };
        const Id floorMat = libMaterial(doc, "Oak Floor");
        Mesh floor;
        floor.groundFacesDown = false;
        floor.drawPolyline({{-1, -1, 0}, {9, -1, 0}, {9, 4, 0}, {-1, 4, 0}}, true);
        paintAll(floor, floorMat);
        place(m, floor, Mat4());
        box({0, 2.5, 0}, {4, 3, 2.6}, libMaterial(doc, "Red Brick"));
        box({4.2, 2.5, 0}, {8, 3, 2.6}, libMaterial(doc, "Stone Wall"));
        Mesh col = primitive(PrimitiveKind::Cylinder, 0.35);
        {
            PrimitiveParams cp;
            cp.kind = PrimitiveKind::Cylinder;
            cp.radius = 0.35;
            cp.height = 2.4;
            cp.segments = 32;
            col = Mesh();
            buildPrimitive(cp, col);
        }
        paintAll(col, libMaterial(doc, "White Marble"));
        place(m, col, Mat4::translation({1, 1, 0}));
        Mesh ball = primitive(PrimitiveKind::Sphere, 0.6);
        paintAll(ball, libMaterial(doc, "Brushed Steel"));
        place(m, ball, Mat4::translation({3, 0.8, 0}));
        box({4.6, 0.2, 0}, {6.2, 1.6, 0.9}, libMaterial(doc, "Concrete"));
        box({6.6, 0.4, 0}, {8.2, 1.8, 1.4}, libMaterial(doc, "Corrugated Metal"));
        m.touch();
        ++doc.materialsRevision;
        look(Vec3(-0.25, 1, -0.45), 0.7);
        frame(QStringLiteral("pbr"));
    }

    // Texture Brush with PBR materials on a terrain.
    {
        fresh();
        doc.style.sunAltitude = 30;
        Mesh& m = doc.mesh();
        const Id ground = libMaterial(doc, "Asphalt");
        const Id stone = libMaterial(doc, "Stone Wall");
        const Id brick = libMaterial(doc, "Red Brick");
        m.drawPolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}}, true);
        paintAll(m, ground);
        TerrainBrush hill;
        hill.radius = 3.5;
        hill.force = 1.5;
        hill.hardness = 0.2;
        {
            TerrainStroke raise(TerrainMode::Raise, hill);
            raise.begin(m, {5, 5, 0}, *allFaces(m).begin(), {0, 0, 1});
            for (int i = 0; i < 12; ++i) raise.apply(m, {5, 5, 0}, 0.08);
            raise.finish(m, {5, 5, 0});
        }
        paintAll(m, ground);
        ++doc.materialsRevision;
        look(Vec3(-0.45, 1, -0.6), 0.75);
        TerrainBrush paint;
        paint.radius = 1.1;
        paint.force = 1;
        paint.hardness = 0.5;
        auto surfacePoint = [&](double x, double y) {
            Ray r{Vec3(x, y, 50), Vec3(0, 0, -1)};
            for (Id f = 0; f < (Id)m.faces.size(); ++f) {
                double t;
                if (m.faces[f].alive && m.rayFace(f, r, t)) return std::make_pair(r.at(t), f);
            }
            return std::make_pair(Vec3(x, y, 0), kNone);
        };
        int frameNo = 0;
        auto strokeAlong = [&](Id mat, const std::function<Vec2(double)>& path, int steps) {
            TerrainStroke st(TerrainMode::Paint, paint);
            st.setPaintMaterial(mat);
            const Vec2 p0 = path(0);
            auto [hit, face] = surfacePoint(p0.x, p0.y);
            if (face == kNone || !st.begin(m, hit, face, {0, 0, 1})) return;
            for (int i = 0; i <= steps; ++i) {
                const Vec2 q = path(i / double(steps));
                auto [c, f2] = surfacePoint(q.x, q.y);
                st.apply(m, c, 0.12);
                m.touch();
                if (i % 2 == 0) seq(QStringLiteral("texturebrush"), ++frameNo);
            }
            st.finish(m, path(1).x > -1 ? surfacePoint(path(1).x, path(1).y).first : hit);
        };
        seq(QStringLiteral("texturebrush"), ++frameNo);
        strokeAlong(stone, [](double t) { return Vec2(1.5 + 7 * t, 2 + 6 * t * t); }, 24);
        strokeAlong(brick, [](double t) { return Vec2(5 + 2.2 * std::cos(t * 5), 5 + 2.2 * std::sin(t * 5)); }, 24);
        frame(QStringLiteral("texturebrush"));
    }


    // ---- Animator tutorial (ANIMATION.md) -------------------------------------
    {
        fresh();
        buildHouse(doc);
        // The door as a component of its own, standing in the doorway.
        Mesh door = boxMesh({2.6, -0.12, 0.01}, {3.6, -0.08, 2.1});
        paintAll(door, addColor(doc, "Door Wood", Color::rgb8(120, 80, 50)));
        Selection sel;
        appendMesh(doc.mesh(), door, Mat4(), nullptr, nullptr, false, &sel);
        const Id doorInst = makeComponent(doc.state, sel, "Door");
        SceneObject lamp;
        lamp.name = "Porch Lamp";
        lamp.pos = Vec3(3.1, -0.8, 2.6);
        lamp.intensity = 0;
        lamp.range = 8;
        lamp.color = Color(1.0f, 0.75f, 0.45f);
        const Id lampId = doc.addObject(lamp);
        doc.style.sunAltitude = 12;
        doc.style.showObjects = true;
        doc.clearHistory();
        for (const char* n : {"Hierarchy", "PropertiesPanel", "AnimatorPanel"})
            if (auto* dk = w.findChild<QDockWidget*>(QString::fromLatin1(n))) dk->show();
        w.resize(1500, 920);
        auto* anim = w.findChild<AnimatorPanel*>();
        look(Vec3(-0.35, 1, -0.45), 0.8);
        pump(300);
        if (anim) {
            anim->modelReplaced();
            // 1. Select the door, New Layer.
            doc.selection.clear();
            doc.selection.instances.insert(doorInst);
            doc.notifySelection();
            anim->addLayersFromSelection();
            pump(200);
            save(w.grab().toImage(), dir.filePath(QStringLiteral("anim_1_layer.png")));
            // 2. Frame 30, turn the door on its hinge: Auto Key adds a keyframe.
            anim->setFrame(30);
            const Vec3 hinge(2.6, -0.1, 0);
            doc.beginOp("Rotate");
            doc.state.instances[doorInst].xf = Mat4::translation(hinge) * Mat4::rotation({0, 0, 1}, -1.75) * Mat4::translation(-hinge) *
                                               doc.state.instances[doorInst].xf;
            doc.commitOp();
            anim->setFrame(18);
            pump(200);
            save(w.grab().toImage(), dir.filePath(QStringLiteral("anim_2_tween.png")));
            // 3. The lamp: a second layer, off until frame 30, then bright by 45.
            anim->setFrame(1);
            doc.selection.clear();
            doc.selection.objects.insert(lampId);
            doc.notifySelection();
            anim->addLayersFromSelection();
            anim->setFrame(30);
            doc.beginOp("Light");
            doc.state.objects[lampId].intensity = 0.01;
            doc.commitOp();
            anim->setCurrentLayer(1);
            anim->setFrame(1);
            anim->toggleTween(); // hold: off from 1 to 30
            anim->setFrame(45);
            doc.beginOp("Light");
            doc.state.objects[lampId].intensity = 9;
            doc.commitOp();
            anim->setFrame(38);
            pump(200);
            save(w.grab().toImage(), dir.filePath(QStringLiteral("anim_3_layers.png")));
            // 4. Played: the viewport frame by frame.
            doc.selection.clear();
            doc.notifySelection();
            doc.style.showObjects = false;
            doc.style.cinematic = true;
            doc.style.lightRays = 0.5;
            for (int f = 1; f <= 50; ++f) {
                anim->setFrame(f);
                seq(QStringLiteral("anim_door"), f);
            }
            doc.style.cinematic = false;
            anim->modelReplaced();
        }
        for (QDockWidget* dk : w.findChildren<QDockWidget*>()) dk->hide();
    }

    // Game Mode: first person.
    {
        fresh();
        buildHouse(doc);
        Mesh p = pipe();
        place(doc.mesh(), p, Mat4::translation({-4, -3, 0}));
        QTemporaryDir tmp;
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("walk.gsketch"));
        if (saveNative(doc, path, {})) {
            GameWindow game;
            QString err;
            if (game.load(path, Vec3(1.5, -7, 0.01), 0, &err)) {
                double best = -2, bestYaw = 0;
                for (int i = 0; i < 360; ++i) {
                    game.walker().setYaw(i * kPi / 180);
                    const double dd = game.walker().lookDirection().dot((Vec3(3, 2, 1.5) - game.walker().eye()).normalized());
                    if (dd > best) best = dd, bestYaw = i * kPi / 180;
                }
                game.walker().setYaw(bestYaw);
                game.resize(960, 600);
                game.show();
                pump(400);
                save(game.grabFramebuffer(), dir.filePath(QStringLiteral("gamemode.png")));
                game.hide();
            }
        }
    }

    doc.reset();
    doc.modified = false;
    return 0;
}

} // namespace gs
