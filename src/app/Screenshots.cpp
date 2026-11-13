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
    doc.reset();
    doc.modified = false;
    return 0;
}

} // namespace gs
