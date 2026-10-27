// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelfTest.h"

#include "MainWindow.h"
#include "ModifyTools.h"
#include "TerrainTools.h"
#include "Viewport.h"
#include "core/Components.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "render/Renderer.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QMimeData>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QSet>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace gs {

namespace {

struct Driver {
    Viewport* vp;
    int failures = 0;

    void pump() { QApplication::processEvents(); }
    void send(QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QPointF g = vp->mapToGlobal(p.toPoint());
        QMouseEvent e(t, p, g, b, bs, m);
        QApplication::sendEvent(vp, &e);
    }
    void move(QPointF p, Qt::MouseButtons bs = Qt::NoButton, Qt::KeyboardModifiers m = Qt::NoModifier) {
        send(QEvent::MouseMove, p, Qt::NoButton, bs, m);
    }
    void click(QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier) {
        move(p, Qt::NoButton, m);
        send(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, m);
        send(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, m);
        pump();
    }
    void drag(QPointF a, QPointF b, Qt::KeyboardModifiers m = Qt::NoModifier) {
        move(a, Qt::NoButton, m);
        send(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, m);
        for (int i = 1; i <= 8; ++i) move(a + (b - a) * (i / 8.0), Qt::LeftButton, m);
        send(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, m);
        pump();
    }
    void dblclick(QPointF p) {
        move(p);
        send(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
        pump();
    }
    void key(int k, const QString& text = QString(), Qt::KeyboardModifiers m = Qt::NoModifier) {
        QKeyEvent p(QEvent::KeyPress, k, m, text);
        QApplication::sendEvent(vp, &p);
        QKeyEvent r(QEvent::KeyRelease, k, m, text);
        QApplication::sendEvent(vp, &r);
        pump();
    }
    void type(const QString& s) {
        for (QChar c : s) key(0, QString(c));
        key(Qt::Key_Return);
    }
    QPointF screen(const Vec3& p) {
        double x, y;
        vp->camera().project(p, x, y);
        return QPointF(x, y);
    }
    void check(bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok) ++failures;
    }
    Mesh& mesh() { return vp->doc().mesh(); }
    Id topFace() {
        Id best = kNone;
        double z = -1e300;
        for (Id f = 0; f < (Id)mesh().faces.size(); ++f)
            if (mesh().faces[f].alive && mesh().faces[f].normal.z > 0.99) {
                double fz = mesh().faceInteriorPoint(f).z;
                if (fz > z) { z = fz; best = f; }
            }
        return best;
    }
};

} // namespace

int runSelfTest(MainWindow& w) {
    auto* vp = w.findChild<Viewport*>();
    if (!vp) return 2;
    Driver d{vp};
    Document& doc = vp->doc();
    std::printf("selftest: renderer %s\n", vp->renderOk() ? "initialised" : "FAILED");
    if (!vp->renderOk()) {
        std::printf("%s\n", qPrintable(vp->glError()));
        return 1;
    }
    vp->setFocus();
    vp->camera().setViewport(vp->width(), vp->height());

    // Rectangle by two clicks on the ground.
    vp->setTool(ToolId::Rectangle);
    d.click(d.screen(Vec3(0, 0, 0)));
    d.click(d.screen(Vec3(3, 2, 0)));
    d.check(d.mesh().aliveFaceCount() == 1, "rectangle tool creates a face");
    for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
        if (d.mesh().faces[f].alive) d.check(d.mesh().faces[f].normal.z > 0.99, "ground rectangle faces the camera");

    // Push/pull it up 1.5 m by typing the distance.
    vp->setTool(ToolId::PushPull);
    Id f0 = kNone;
    for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
        if (d.mesh().faces[f].alive) f0 = f;
    QPointF fc = d.screen(d.mesh().faceInteriorPoint(f0));
    d.click(fc);
    d.move(fc - QPointF(0, 60));
    d.type("1500");
    d.check(d.mesh().aliveFaceCount() == 6, "push/pull with typed distance makes a box");
    Vec3 mn, mx;
    bool any;
    d.mesh().bounds(mn, mx, any);
    d.check(std::fabs(mx.z - 1.5) < 1e-6 && std::fabs(mn.z) < 1e-6, "box pulled up 1500mm in the drag direction");
    vp->zoomExtents();
    d.pump();

    // Line across the top face splits it.
    vp->setTool(ToolId::Line);
    double top = std::fabs(mx.z) > std::fabs(mn.z) ? mx.z : mn.z;
    d.click(d.screen(Vec3(1.5, 0, top)));
    d.click(d.screen(Vec3(1.5, 2, top)));
    d.key(Qt::Key_Escape);
    d.check(d.mesh().aliveFaceCount() == 7, "line across top face splits it");

    // Line with typed length and axis lock.
    d.click(d.screen(Vec3(5, 0, 0)));
    d.move(d.screen(Vec3(6, 0, 0)));
    d.key(Qt::Key_Right);
    d.type("2000");
    d.key(Qt::Key_Escape);
    d.check(d.mesh().findVertex(Vec3(7, 0, 0)) != kNone, "typed line length along locked red axis");

    // Push/pull one half of the top up.
    vp->setTool(ToolId::PushPull);
    Id half = kNone;
    for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
        if (d.mesh().faces[f].alive && d.mesh().faces[f].normal.z > 0.99 && d.mesh().faceInteriorPoint(f).x > 1.5) half = f;
    d.check(half != kNone, "found half of the top face");
    if (half != kNone) {
        QPointF hc = d.screen(d.mesh().faceInteriorPoint(half));
        d.drag(hc, hc - QPointF(0, 50));
        d.check(d.mesh().aliveFaceCount() >= 8, "push/pull by dragging");
    }

    // Circle on the ground and extrude it.
    vp->setTool(ToolId::Circle);
    d.click(d.screen(Vec3(-3, 0, 0)));
    d.click(d.screen(Vec3(-2, 0, 0)));
    int faces = d.mesh().aliveFaceCount();
    vp->setTool(ToolId::PushPull);
    QPointF cc = d.screen(Vec3(-3, 0, 0));
    d.click(cc);
    d.move(cc - QPointF(0, 40));
    d.type("800");
    d.check(d.mesh().aliveFaceCount() == faces + 25, "circle extruded into a cylinder");

    // Polygon, arc and freehand.
    vp->setTool(ToolId::Polygon);
    d.click(d.screen(Vec3(-3, 4, 0)));
    d.click(d.screen(Vec3(-2, 4, 0)));
    vp->setTool(ToolId::Arc);
    d.click(d.screen(Vec3(2, 5, 0)));
    d.click(d.screen(Vec3(4, 5, 0)));
    d.click(d.screen(Vec3(3, 6, 0)));
    vp->setTool(ToolId::Freehand);
    d.drag(d.screen(Vec3(0, 6, 0)), d.screen(Vec3(1, 7, 0)));
    d.check(true, "polygon / arc / freehand did not crash");

    // Selection: click, window and crossing.
    vp->setTool(ToolId::Select);
    d.click(d.screen(Vec3(1.5, 1, 0.0)) + QPointF(0, 0));
    QPointF a(5, 5), b(vp->width() - 5, vp->height() - 5);
    d.drag(a, b);
    d.check(doc.selection.count() > 10, "window selection selects geometry");
    d.drag(b, a);
    d.check(doc.selection.count() > 10, "crossing selection selects geometry");
    doc.selection.clear();
    doc.notifySelection();

    // Move with copy then array.
    vp->setTool(ToolId::Select);
    Id tf = d.topFace();
    if (tf != kNone) {
        doc.selection.faces = {tf};
        doc.notifySelection();
        vp->setTool(ToolId::Move);
        QPointF p = d.screen(d.mesh().faceInteriorPoint(tf));
        d.move(p);
        d.key(Qt::Key_Control);
        d.click(p);
        d.move(p + QPointF(80, 0));
        d.type("4000");
        d.type("3x");
        d.check(doc.undoName() == "Array", "move-copy + array");
    }

    // Rotate selection 90 degrees by typing.
    vp->setTool(ToolId::Select);
    d.click(d.screen(Vec3(-3, 0, 0.4)));
    vp->setTool(ToolId::Rotate);
    d.move(d.screen(Vec3(0, 0, 0)));
    d.key(Qt::Key_Up); // lock the protractor to the ground plane
    d.click(d.screen(Vec3(0, 0, 0)));
    d.click(d.screen(Vec3(1, 0, 0)));
    d.move(d.screen(Vec3(0, 1, 0)));
    d.type("45");
    d.check(doc.undoName() == "Rotate", "rotate with typed angle");

    // Scale.
    vp->setTool(ToolId::Scale);
    Vec3 smx(-1e300, -1e300, -1e300);
    for (Id v : selectionVertices(d.mesh(), doc.selection))
        for (int k = 0; k < 3; ++k) smx[k] = std::max(smx[k], d.mesh().pos(v)[k]);
    QPointF sp = d.screen(smx); // the max corner grip
    d.click(sp);
    d.move(sp + QPointF(30, -30));
    d.type("2");
    d.check(doc.undoName() == "Scale", "scale with typed factor");

    // Offset a face.
    vp->setTool(ToolId::Offset);
    tf = d.topFace();
    if (tf != kNone) {
        int before = d.mesh().aliveFaceCount();
        QPointF p = d.screen(d.mesh().faceInteriorPoint(tf));
        d.click(p);
        d.move(p + QPointF(5, 5));
        d.type("100");
        d.check(d.mesh().aliveFaceCount() == before + 1, "offset creates an inner face");
    }

    // Paint and texture drop.
    QTemporaryDir tmp;
    QImage img(64, 32, QImage::Format_RGB32);
    img.fill(Qt::darkCyan);
    QString png = tmp.filePath("tex.png");
    img.save(png);
    tf = d.topFace();
    if (tf != kNone) {
        QPointF p = d.screen(d.mesh().faceInteriorPoint(tf));
        bool applied = vp->dropImage(png, p.x(), p.y(), false);
        d.check(applied && doc.state.materials.size() >= 1, "image drop applies a texture material");
        vp->setTool(ToolId::Paint);
        d.click(p, Qt::AltModifier);
        d.click(d.screen(Vec3(0.2, 0.0, 0.3)));
        d.check(true, "paint bucket did not crash");
    }

    // Eraser drag over edges.
    vp->setTool(ToolId::Eraser);
    d.drag(d.screen(Vec3(5.5, -0.2, 0)), d.screen(Vec3(6.5, 0.2, 0)));
    d.check(d.mesh().findVertex(Vec3(7, 0, 0)) == kNone, "eraser drag erases the loose line");

    // Tape measure guide.
    vp->setTool(ToolId::Tape);
    d.click(d.screen(Vec3(1.5, 0, 0)));
    d.click(d.screen(Vec3(1.5, -1, 0)));
    d.check(!doc.state.guides.empty(), "tape measure creates a guide");

    // Camera tools and styles.
    vp->setTool(ToolId::Orbit);
    d.drag(QPointF(300, 300), QPointF(360, 320));
    vp->setTool(ToolId::Pan);
    d.drag(QPointF(300, 300), QPointF(320, 340));
    vp->setTool(ToolId::Zoom);
    d.drag(QPointF(300, 300), QPointF(300, 260));
    vp->setTool(ToolId::ZoomWindow);
    d.drag(QPointF(200, 200), QPointF(500, 400));
    vp->zoomExtents();
    vp->setPerspective(false);
    vp->update();
    d.pump();
    vp->setPerspective(true);

    QSet<QRgb> colors;
    for (int s = 0; s <= 4; ++s) {
        doc.style.faceStyle = (RenderStyle::FaceStyle)s;
        doc.style.backEdges = s % 2;
        doc.style.xray = s == 2;
        QImage fb = vp->grabFramebuffer();
        for (int y = 0; y < fb.height(); y += 7)
            for (int x = 0; x < fb.width(); x += 7) colors.insert(fb.pixel(x, y));
    }
    doc.style = RenderStyle();
    d.check(colors.size() > 20, "rendered frames are not blank");

    // View > Render toggles: shadows darken part of the frame, antialiasing softens edges.
    {
        auto action = [&](const char* name) { return w.findChild<QAction*>(QString::fromLatin1(name)); };
        auto diffCount = [](const QImage& a, const QImage& b, int* darker) {
            int n = 0;
            if (darker) *darker = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) {
                    QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                    if (p == q) continue;
                    ++n;
                    if (darker && qGray(q) + 8 < qGray(p)) ++*darker;
                }
            return n;
        };
        QAction* shadows = action("shadows");
        QAction* aa = action("antialiasing");
        d.check(shadows && aa && !shadows->isChecked() && aa->isChecked(), "View > Render has Shadows and Antialiasing toggles");
        if (shadows && aa) {
            QImage base = vp->grabFramebuffer();
            shadows->trigger();
            QImage shadowed = vp->grabFramebuffer();
            int darker = 0;
            diffCount(base, shadowed, &darker);
            d.check(doc.style.shadows && darker > base.width() * base.height() / 200, "shadows darken part of the scene");
            shadows->trigger();
            aa->trigger();
            QImage aliased = vp->grabFramebuffer();
            d.check(!doc.style.antialiasing && diffCount(base, aliased, nullptr) > 100, "antialiasing can be switched off");
            aa->trigger();
            d.check(diffCount(base, vp->grabFramebuffer(), nullptr) == 0, "toggling both back restores the frame");
        }
    }

    // Image dropped into empty space becomes a textured plane; trace its alpha.
    {
        QImage cut(200, 100, QImage::Format_ARGB32);
        cut.fill(Qt::transparent);
        QPainter pt(&cut);
        pt.setBrush(Qt::red);
        pt.setPen(Qt::NoPen);
        pt.drawEllipse(QRectF(60, 10, 80, 80));
        pt.end();
        QString cutPng = tmp.filePath("cutout.png");
        cut.save(cutPng);
        vp->setStandardView(Vec3(0, 0, -1));
        size_t mats = doc.state.materials.size();
        bool placed = vp->addImagePlane(cutPng, 8, 8);
        d.pump();
        d.check(placed && doc.state.materials.size() == mats + 1 && !doc.selection.faces.empty(), "image drop into empty space adds a plane");
        if (!doc.selection.faces.empty()) {
            Id f = *doc.selection.faces.begin();
            const Material* mat = doc.material(d.mesh().faces[f].front.material);
            bool traced = false;
            if (mat) {
                ScalarImage si = scalarImage(decodeTexture(mat->texture), TraceSource::Alpha);
                doc.beginOp("Trace Image");
                traced = traceImageOnFace(d.mesh(), f, effectiveTexMap(d.mesh().faces[f], true, mat), si, TraceOptions());
                doc.commitOp();
            }
            d.check(traced, "trace image cuts out the alpha shape");
        }
        vp->zoomExtents();
    }

    // System clipboard: geometry (as GNUSketch data) and images.
    {
        auto action = [&](const char* name) { return w.findChild<QAction*>(QString::fromLatin1(name)); };
        Id top = d.topFace();
        if (top != kNone && action("copy") && action("paste")) {
            doc.selection.clear();
            doc.selection.faces.insert(top);
            doc.notifySelection();
            action("copy")->trigger();
            const QMimeData* md = QApplication::clipboard()->mimeData();
            ModelState clip;
            bool decoded = md && md->hasFormat(QStringLiteral("application/x-gnusketch")) &&
                           modelFromJson(md->data(QStringLiteral("application/x-gnusketch")), clip);
            d.check(decoded && clip.mesh.aliveFaceCount() == 1, "copy puts geometry on the system clipboard");
            int facesBefore = d.mesh().aliveFaceCount();
            Vec3 origin = d.mesh().faceInteriorPoint(top);
            action("paste")->trigger();
            d.pump();
            bool following = doc.inOp() && vp->tool() && vp->tool()->busy() && doc.selection.faces.size() == 1;
            Vec3 target(8, 6, 0);
            d.move(d.screen(target));
            Id pf = doc.selection.faces.empty() ? kNone : *doc.selection.faces.begin();
            bool moved = pf != kNone && (d.mesh().faceInteriorPoint(pf) - origin).length() > 1.0;
            d.check(following && moved, "pasted geometry follows the cursor right away");
            d.click(d.screen(target));
            d.check(!doc.inOp() && doc.undoName() == "Paste" && doc.selection.faces.size() == 1 &&
                        d.mesh().aliveFaceCount() == facesBefore + 1,
                    "a click places the paste as one undo step");
            doc.undo();
            action("paste")->trigger();
            d.pump();
            d.move(d.screen(target));
            d.key(Qt::Key_Escape);
            d.check(!doc.inOp() && d.mesh().aliveFaceCount() == facesBefore && doc.undoName() != "Paste",
                    "Esc cancels a paste that follows the cursor");
            std::string lastOp = doc.undoName();
            action("paste")->trigger();
            d.pump();
            action("undo")->trigger();
            d.pump();
            d.check(!doc.inOp() && d.mesh().aliveFaceCount() == facesBefore && doc.undoName() == lastOp,
                    "Ctrl+Z while placing cancels only the paste");
            QImage shot(120, 80, QImage::Format_RGB32);
            shot.fill(Qt::darkGreen);
            QApplication::clipboard()->setImage(shot);
            size_t mats = doc.state.materials.size();
            action("paste")->trigger();
            d.pump();
            d.check(doc.state.materials.size() == mats + 1 && doc.undoName() == "Add Image", "paste an image from the clipboard as a plane");
            doc.undo();
            vp->setTool(ToolId::Select);
        }
    }

    // Components: make, Ctrl+Move copy, scale one copy, edit in place, make unique, explode.
    {
        auto action = [&](const char* name) { return w.findChild<QAction*>(QString::fromLatin1(name)); };
        doc.selection.clear();
        doc.beginOp("Box");
        Mesh& m = d.mesh();
        m.drawPolyline({{20, 0, 0}, {21, 0, 0}, {21, 1, 0}, {20, 1, 0}}, true);
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.facePlane(f).contains(Vec3(20.5, 0.5, 0)) && m.pointInFace(f, Vec3(20.5, 0.5, 0))) {
                if (m.faces[f].normal.z < 0) m.reverseFace(f);
                m.pushPull(f, 1.0);
                break;
            }
        doc.commitOp();
        Selection s;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faceInteriorPoint(f).x > 19.9) s.faces.insert(f);
        for (Id e = 0; e < (Id)m.edges.size(); ++e)
            if (m.edges[e].alive && m.pos(m.edges[e].v[0]).x > 19.9 && m.pos(m.edges[e].v[1]).x > 19.9) s.edges.insert(e);
        doc.beginOp("Make Component");
        Id a = makeComponent(doc.state, s, "Crate");
        doc.selection.clear();
        if (a != kNone) doc.selection.instances.insert(a);
        doc.commitOp();
        doc.notifySelection();
        d.check(a != kNone && doc.state.defs.back().mesh.aliveFaceCount() == 6, "make component from the selection");
        const Id def = a != kNone ? doc.state.instances[a].def : kNone;
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(19.5, -0.5, 0), Vec3(25, 1.5, 2.5));
        d.pump();

        vp->setTool(ToolId::Move);
        d.key(Qt::Key_Control);
        d.click(d.screen(Vec3(20.5, 0.5, 1.0)));
        d.move(d.screen(Vec3(22.5, 0.5, 1.0)));
        d.type("<3000;0;0>");
        Id b = (Id)doc.state.instances.size() - 1;
        Vec3 mn, mx;
        bool copied = b != a && doc.state.instances[b].alive && doc.state.instances[b].def == def &&
                      instanceBounds(doc.state, b, mn, mx) && mn.nearlyEquals(Vec3(23, 0, 0), 1e-6);
        d.check(copied, "Ctrl+Move copies a component instance sharing its definition");

        vp->setTool(ToolId::Scale);
        doc.selection.clear();
        doc.selection.instances.insert(b);
        doc.notifySelection();
        d.click(d.screen(Vec3(24, 1, 1)));
        d.type("2");
        Vec3 dmn, dmx, amn, amx;
        bool any = false;
        doc.state.defs[def].mesh.bounds(dmn, dmx, any);
        instanceBounds(doc.state, b, mn, mx);
        instanceBounds(doc.state, a, amn, amx);
        d.check(std::fabs((mx - mn).z - 2) < 1e-6 && std::fabs((amx - amn).z - 1) < 1e-6 && dmx.nearlyEquals(Vec3(1, 1, 1), 1e-6),
                "scaling one instance leaves the others and the definition alone");

        // Face grip: one axis only.
        doc.selection.clear();
        doc.selection.instances.insert(a);
        doc.notifySelection();
        d.click(d.screen(Vec3(21, 0.5, 0.5)));
        d.type("2");
        instanceBounds(doc.state, a, amn, amx);
        d.check((amx - amn).nearlyEquals(Vec3(2, 1, 1), 1e-6) && amn.nearlyEquals(Vec3(20, 0, 0), 1e-6),
                "face grip scales a component along one axis about the opposite face");
        d.click(d.screen(Vec3(22, 0.5, 0.5)));
        d.type("0.5");
        instanceBounds(doc.state, a, amn, amx);
        d.check((amx - amn).nearlyEquals(Vec3(1, 1, 1), 1e-6), "scale back with the moved grip");

        vp->setTool(ToolId::Select);
        d.dblclick(d.screen((mn + mx) * 0.5 + Vec3(0, 0, (mx - mn).z * 0.5)));
        d.check(doc.state.editing == b, "double-click opens the component for editing");
        if (doc.state.editing == b) {
            vp->setTool(ToolId::PushPull);
            Id tf = d.topFace();
            QPointF tc = tf != kNone ? d.screen(d.mesh().faceInteriorPoint(tf)) : QPointF();
            d.click(tc);
            d.move(tc - QPointF(0, 40));
            d.type("500");
            instanceBounds(doc.state, a, amn, amx);
            d.check(std::fabs(amx.z - 1.25) < 1e-6, "editing one copy updates the other copies live");
            QImage fb = vp->grabFramebuffer();
            d.check(!fb.isNull(), "renders while a component is open");
            vp->setTool(ToolId::Select);
            d.key(Qt::Key_Escape);
            d.check(!doc.editingComponent() && doc.selection.instances.count(b), "Esc closes the component");
        }

        doc.selection.clear();
        doc.selection.instances.insert(b);
        doc.notifySelection();
        action("makeunique")->trigger();
        d.check(doc.state.instances[b].def != doc.state.instances[a].def && instanceCount(doc.state, def) == 1,
                "Make Unique gives the copy its own definition");

        int before = doc.state.mesh.aliveFaceCount();
        doc.selection.clear();
        doc.selection.instances.insert(a);
        doc.notifySelection();
        action("explode")->trigger();
        d.check(!doc.state.instances[a].alive && doc.state.mesh.aliveFaceCount() == before + 6 && doc.selection.faces.size() == 6,
                "Explode turns the component into loose geometry");
        // Edge grip on loose geometry: two axes, then adjust by typing again.
        vp->setTool(ToolId::Scale);
        Vec3 emn, emx;
        auto looseBounds = [&] {
            emn = Vec3(1e300, 1e300, 1e300);
            emx = -emn;
            for (Id v : selectionVertices(d.mesh(), doc.selection))
                for (int k = 0; k < 3; ++k) {
                    emn[k] = std::min(emn[k], d.mesh().pos(v)[k]);
                    emx[k] = std::max(emx[k], d.mesh().pos(v)[k]);
                }
        };
        looseBounds();
        d.click(d.screen(Vec3(emx.x, emx.y, (emn.z + emx.z) * 0.5)));
        d.type("2,3");
        Vec3 size0 = emx - emn;
        looseBounds();
        d.check((emx - emn).nearlyEquals(Vec3(size0.x * 2, size0.y * 3, size0.z), 1e-6), "edge grip scales two axes");
        d.type("3,3");
        looseBounds();
        d.check((emx - emn).nearlyEquals(Vec3(size0.x * 3, size0.y * 3, size0.z), 1e-6) && doc.undoName() == "Scale",
                "typing after a scale adjusts it");
        doc.selection.clear();
        doc.notifySelection();
        vp->zoomExtents();
    }

    // Magenta inference: along the edges at the start point (both ways) and
    // parallel to a hovered edge.
    {
        std::vector<Vec3> hex;
        for (int i = 0; i < 6; ++i) hex.push_back(Vec3(40 + std::cos(i * kPi / 3), std::sin(i * kPi / 3), 0));
        doc.beginOp("Hexagon");
        d.mesh().drawPolyline(hex, true);
        doc.commitOp();
        vp->setStandardView(Vec3(0, 0, -1));
        vp->camera().zoomExtents(Vec3(38, -2, 0), Vec3(43, 3, 0));
        InferenceEngine& eng = vp->inference();
        eng.resetMemory();
        Vec3 c0 = hex[0], dir = (hex[1] - hex[0]).normalized();
        InferenceEngine::Options o;
        o.ref = c0;
        auto at = [&](const Vec3& p) { return d.screen(p) + QPointF(2, 1); };
        QPointF q = at(c0 + dir * 1.6); // beyond the far corner, on the edge's line
        InferenceResult along = eng.infer(q.x(), q.y(), o);
        q = at(c0 - dir * 0.7); // the extension behind the corner
        InferenceResult ext = eng.infer(q.x(), q.y(), o);
        d.check(along.type == InferenceResult::AlongEdge && ext.type == InferenceResult::AlongEdge &&
                    std::fabs(std::fabs(ext.dir.dot(dir)) - 1) < 1e-9 && std::fabs((ext.p - c0).cross(dir).length()) < 1e-9,
                "line from a corner snaps along its edges and their extensions");
        // Hover the slanted edge hex[2]-hex[3], then draw from a free point parallel to it.
        Vec3 pd = (hex[3] - hex[2]).normalized();
        QPointF he = d.screen((hex[2] + hex[3]) * 0.5 + pd * 0.2);
        InferenceEngine::Options none;
        eng.infer(he.x(), he.y(), none);
        Vec3 start(42.5, 1.5, 0);
        o.ref = start;
        q = at(start + pd * 0.8);
        InferenceResult par = eng.infer(q.x(), q.y(), o);
        d.check(par.type == InferenceResult::Parallel && std::fabs(std::fabs(par.dir.dot(pd)) - 1) < 1e-9,
                "line snaps parallel to the last hovered edge");
        eng.resetMemory();
        vp->zoomExtents();
    }

    // Rectangles on vertical planes, front side toward the camera.
    {
        auto faceNear = [&](const Vec3& p) {
            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
                if (d.mesh().faces[f].alive && d.mesh().facePlane(f).contains(p, 1e-6) &&
                    (d.mesh().faceInteriorPoint(f) - p).length() < 1.5)
                    return f;
            return kNone;
        };
        vp->setTool(ToolId::Rectangle);
        // Front view: free-space rectangle lands on the vertical plane facing the viewer.
        vp->setStandardView(Vec3(0, 1, 0));
        vp->camera().zoomExtents(Vec3(60, 0, 0), Vec3(64, 0, 3));
        d.click(d.screen(Vec3(60, 0, 0.5)));
        d.click(d.screen(Vec3(62, 0, 2)));
        Id fv = faceNear(Vec3(61, 0, 1.25));
        d.check(fv != kNone && d.mesh().faces[fv].normal.y < -0.99, "front view rectangle is vertical and faces the camera");
        // Rotate in the same view about a point in empty space: the protractor
        // lies in the plane facing the camera, so the face stays in that plane.
        if (fv != kNone) {
            vp->setTool(ToolId::Select);
            d.click(d.screen(Vec3(61, 0, 1.25)));
            vp->setTool(ToolId::Rotate);
            d.move(d.screen(Vec3(63.5, 0, 1)));
            d.click(d.screen(Vec3(63.5, 0, 1)));
            d.click(d.screen(Vec3(64, 0, 1)));
            d.move(d.screen(Vec3(63.5, 0, 1.5)));
            d.type("90");
            bool inPlane = true;
            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
                if (d.mesh().faces[f].alive && d.mesh().faceInteriorPoint(f).x > 59 && d.mesh().faceInteriorPoint(f).x < 66)
                    inPlane = inPlane && std::fabs(d.mesh().faces[f].normal.y) > 0.99;
            d.check(doc.undoName() == "Rotate" && inPlane, "rotate in front view turns about the axis facing the camera");
            vp->setTool(ToolId::Rectangle);
        }
        // Iso view: drag up the blue axis, then sideways.
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(70, 0, 0), Vec3(73, 0, 3));
        d.click(d.screen(Vec3(70, 0, 0)));
        d.move(d.screen(Vec3(70, 0, 1.5)));
        d.move(d.screen(Vec3(71, 0, 1.5)));
        d.click(d.screen(Vec3(72, 0, 1.5)));
        Id fb = faceNear(Vec3(71, 0, 0.75));
        d.check(fb != kNone && d.mesh().faces[fb].normal.y < -0.99, "rectangle dragged up blue is vertical and faces the camera");
        vp->zoomExtents();
    }

    // An axis line can still end exactly on an edge it crosses or a face it hits.
    {
        doc.beginOp("Targets");
        d.mesh().drawLine(Vec3(50, -1, 0), Vec3(50, 2, 0)); // midpoint away from the crossing
        d.mesh().drawPolyline({{53, -1, 0}, {53, 1, 0}, {53, 1, 2}, {53, -1, 2}}, true);
        doc.commitOp();
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(47, -2, 0), Vec3(54, 2, 2));
        InferenceEngine& eng = vp->inference();
        eng.resetMemory();
        InferenceEngine::Options o;
        o.ref = Vec3(48, 0, 0);
        QPointF q = d.screen(Vec3(50, 0.03, 0));
        InferenceResult r1 = eng.infer(q.x(), q.y(), o);
        d.check(r1.type == InferenceResult::OnEdge && r1.p.nearlyEquals(Vec3(50, 0, 0), 1e-9),
                "red axis line ends exactly on the edge it crosses");
        o.ref = Vec3(51, 0, 1);
        q = d.screen(Vec3(53, 0.02, 1.01));
        InferenceResult r2 = eng.infer(q.x(), q.y(), o);
        d.check(r2.type == InferenceResult::OnFace && r2.p.nearlyEquals(Vec3(53, 0, 1), 1e-9),
                "red axis line ends exactly on the face it hits");
        eng.resetMemory();
        vp->zoomExtents();
    }

    // Terraform: press-and-hold brushes on a ground plane away from the rest.
    {
        const TerrainBrush userBrush = TerrainSettings::instance().brush();
        const Camera savedCam = vp->camera();
        doc.beginOp("Terrain plane");
        d.mesh().drawPolyline({{300, 0, 0}, {310, 0, 0}, {310, 10, 0}, {300, 10, 0}}, true);
        doc.commitOp();
        vp->camera().setStandardView(Vec3(0.3, 0.4, -1), Vec3(0, 0, 1));
        vp->camera().zoomExtents(Vec3(300, 0, 0), Vec3(310, 10, 1));
        d.pump();
        auto hold = [&](const Vec3& at, int ms, bool escape = false, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QPointF p = d.screen(at);
            d.move(p, Qt::NoButton, mods);
            d.send(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
            if (escape) d.key(Qt::Key_Escape);
            d.send(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
            d.pump();
        };
        auto zRange = [&](double& lo, double& hi) {
            lo = 1e300, hi = -1e300;
            for (const Vertex& v : d.mesh().vertices)
                if (v.alive && v.p.x >= 299.99) { lo = std::min(lo, v.p.z); hi = std::max(hi, v.p.z); }
        };
        TerrainBrush b;
        b.radius = 2;
        b.force = 1; // 100 cm/s
        b.hardness = 0.3;
        TerrainSettings::instance().setBrush(b);
        double lo, hi;

        vp->setTool(ToolId::Mountain);
        int facesBefore = d.mesh().aliveFaceCount();
        hold(Vec3(305, 5, 0), 300);
        zRange(lo, hi);
        d.check(hi > 0.15 && hi < 1.5 && doc.undoName() == "Mountain", "holding Mountain raises the plane over time");
        d.check(d.mesh().aliveFaceCount() > facesBefore + 100, "mountain subdivides the plane into a fine triangle mesh");
        bool corners = true;
        for (const Vertex& v : d.mesh().vertices)
            if (v.alive && v.p.x >= 299.99 && (std::fabs(v.p.x - 305) > 4.99 && std::fabs(v.p.y - 5) > 4.99))
                corners = corners && std::fabs(v.p.z) < 1e-9;
        d.check(corners, "mountain leaves the plane corners in place");

        hold(Vec3(305, 5, 0), 150, true);
        double lo2, hi2;
        zRange(lo2, hi2);
        d.check(std::fabs(hi2 - hi) < 1e-9 && doc.undoName() == "Mountain", "Esc cancels a Mountain stroke");

        vp->setTool(ToolId::Pit);
        hold(Vec3(302, 8, 0), 200);
        zRange(lo, hi);
        d.check(lo < -0.1 && doc.undoName() == "Pit", "holding Pit sinks the plane");

        b.radius = 15; // reaches the far corner from where the stroke starts
        b.force = 20;
        TerrainSettings::instance().setBrush(b);
        vp->setTool(ToolId::Flatten);
        hold(Vec3(300.3, 0.3, 0), 400);
        zRange(lo, hi);
        d.check(std::fabs(lo) < 1e-6 && std::fabs(hi) < 1e-6 && doc.undoName() == "Flatten",
                "Flatten levels the terrain to the height where the stroke started");

        // Shift keeps the brush vertical on a slope: nothing moves sideways.
        doc.beginOp("Terrain slope");
        d.mesh().drawPolyline({{320, 0, 0}, {330, 0, 0}, {330, 10, 5}, {320, 10, 5}}, true);
        doc.commitOp();
        vp->camera().zoomExtents(Vec3(320, 0, 0), Vec3(330, 10, 5));
        d.pump();
        b.radius = 2;
        b.force = 1;
        TerrainSettings::instance().setBrush(b);
        std::vector<Vec3> before;
        for (const Vertex& v : d.mesh().vertices) before.push_back(v.p);
        vp->setTool(ToolId::Mountain);
        hold(Vec3(325, 5, 2.5), 250, false, Qt::ShiftModifier);
        bool sideways = false, rose = false;
        for (size_t i = 0; i < d.mesh().vertices.size(); ++i) {
            const Vertex& v = d.mesh().vertices[i];
            if (!v.alive || v.p.x < 319.99) continue;
            const Vec3 o = i < before.size() ? before[i] : Vec3(v.p.x, v.p.y, v.p.y * 0.5);
            sideways = sideways || std::fabs(v.p.y - o.y) > 1e-9 || std::fabs(v.p.x - o.x) > 1e-9;
            rose = rose || v.p.z - v.p.y * 0.5 > 0.1;
        }
        d.check(rose && !sideways, "Shift keeps Mountain on the vertical axis on a slope");

        // Ctrl+wheel scales the radius, Alt+wheel steps the hardness; the camera stays put.
        {
            const Camera camBefore = vp->camera();
            QPointF p = d.screen(Vec3(325, 5, 2.5));
            auto wheel = [&](int dy, Qt::KeyboardModifiers m) {
                QWheelEvent we(p, vp->mapToGlobal(p.toPoint()), QPoint(), QPoint(0, dy), Qt::NoButton, m, Qt::NoScrollPhase, false);
                QApplication::sendEvent(vp, &we);
                d.pump();
            };
            wheel(120, Qt::ControlModifier);
            d.check(std::fabs(TerrainSettings::instance().brush().radius - 2.2) < 1e-9 && vp->camera().eye.nearlyEquals(camBefore.eye),
                    "Ctrl+wheel grows the terrain brush radius without zooming");
            const double h0 = TerrainSettings::instance().brush().hardness;
            wheel(-240, Qt::AltModifier);
            d.check(std::fabs(TerrainSettings::instance().brush().hardness - (h0 - 0.1)) < 1e-9, "Alt+wheel lowers the hardness");
        }

        // Texture Brush: paint a red material over the (sculpted) plane.
        {
            vp->camera().setStandardView(Vec3(0.3, 0.4, -1), Vec3(0, 0, 1));
            vp->camera().zoomExtents(Vec3(300, 0, 0), Vec3(310, 10, 1));
            d.pump();
            Material red;
            red.name = "Brush Red";
            red.color = Color::rgb8(230, 0, 0);
            doc.currentMaterial = doc.addMaterial(red);
            b.radius = 1.5;
            b.hardness = 0.6;
            TerrainSettings::instance().setBrush(b);
            vp->setTool(ToolId::TextureBrush);
            const Vec3 spot(307, 2.5, 0);
            double z = 0;
            for (const Vertex& v : d.mesh().vertices)
                if (v.alive && (v.p - spot).length() < 0.3) z = v.p.z;
            const Vec3 at(spot.x, spot.y, z);
            QPointF px = d.screen(at);
            vp->update();
            QColor before = vp->grabFramebuffer().pixelColor(px.toPoint());
            std::vector<Vec3> pos;
            for (const Vertex& v : d.mesh().vertices) pos.push_back(v.p);
            d.move(QPointF(5, 5)); // no brush ring over the sampled pixel
            hold(at, 400);
            d.move(QPointF(5, 5));
            bool still = true;
            for (size_t i = 0; i < pos.size(); ++i) still = still && d.mesh().vertices[i].p.nearlyEquals(pos[i], 1e-12);
            float w = 0;
            for (const Vertex& v : d.mesh().vertices)
                if (v.alive && (v.p - at).length() < 0.3)
                    for (const PaintWeight& pw : v.paint)
                        if (pw.material == doc.currentMaterial) w = std::max(w, pw.weight);
            d.check(still && w > 0.6 && doc.undoName() == "Texture Brush", "Texture Brush paints the chosen material without moving geometry");
            vp->update();
            QColor after = vp->grabFramebuffer().pixelColor(px.toPoint());
            d.check(after.red() - after.green() > before.red() - before.green() + 60, "painted material shows in the viewport");
            doc.currentMaterial = kNone;
        }

        TerrainSettings::instance().setBrush(userBrush);
        vp->camera() = savedCam;
        vp->setTool(ToolId::Select);
        d.pump();
    }

    // Undo everything, redo everything.
    int steps = 0;
    while (doc.canUndo() && steps < 500) { doc.undo(); ++steps; d.pump(); }
    d.check(d.mesh().aliveFaceCount() == 0, "undo back to empty model");
    while (doc.canRedo()) { doc.redo(); d.pump(); }
    d.check(d.mesh().aliveFaceCount() > 10, "redo restores the model");

    // Save / load roundtrip.
    QString file = tmp.filePath("selftest.gsketch");
    int facesNow = d.mesh().aliveFaceCount();
    QString err;
    bool saved = saveNative(doc, file, {}, &err);
    Document re;
    bool loaded = loadNative(re, file, nullptr, &err);
    d.check(saved && loaded && re.mesh().aliveFaceCount() == facesNow, "save/load roundtrip");

    std::printf("selftest: %d faces, %d edges, %zu materials, %d failures\n", d.mesh().aliveFaceCount(),
                d.mesh().aliveEdgeCount(), doc.state.materials.size(), d.failures);
    doc.modified = false;
    return d.failures ? 1 : 0;
}

int runGlProbe() {
    std::printf("gl-probe: platform %s\n", qPrintable(QGuiApplication::platformName()));
    QOpenGLContext ctx;
    ctx.setFormat(QSurfaceFormat::defaultFormat());
    if (!ctx.create()) {
        std::printf("gl-probe: FAILED to create an OpenGL 3.3 core context\n");
        return 1;
    }
    QOffscreenSurface surface;
    surface.setFormat(ctx.format());
    surface.create();
    if (!ctx.makeCurrent(&surface)) {
        std::printf("gl-probe: FAILED to make the context current\n");
        return 1;
    }
    auto* f = ctx.functions();
    std::printf("gl-probe: %s | %s | %s\n", (const char*)f->glGetString(GL_VENDOR), (const char*)f->glGetString(GL_RENDERER),
                (const char*)f->glGetString(GL_VERSION));
    QOpenGLFramebufferObjectFormat ff;
    ff.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    ff.setSamples(4);
    QOpenGLFramebufferObject fbo(800, 600, ff);
    fbo.bind();
    Renderer r;
    QString log;
    if (!r.initialize(&log)) {
        std::printf("gl-probe: renderer FAILED:\n%s\n", qPrintable(log));
        return 1;
    }
    Document doc;
    doc.mesh().drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}, true);
    for (Id i = 0; i < (Id)doc.mesh().faces.size(); ++i)
        if (doc.mesh().faces[i].alive) doc.mesh().pushPull(i, -1.0);
    Camera cam;
    cam.setViewport(800, 600);
    Preview pv;
    for (int s = 0; s <= 4; ++s) {
        doc.style.faceStyle = (RenderStyle::FaceStyle)s;
        r.render(doc, cam, pv, 1.0);
    }
    GLenum err = f->glGetError();
    QImage img = fbo.toImage();
    QSet<QRgb> colors;
    for (int y = 0; y < img.height(); y += 5)
        for (int x = 0; x < img.width(); x += 5) colors.insert(img.pixel(x, y));
    r.cleanup();
    fbo.release();
    std::printf("gl-probe: glError=0x%x, %d distinct colours -> %s\n", err, int(colors.size()),
                err == GL_NO_ERROR && colors.size() > 10 ? "OK" : "FAILED");
    return err == GL_NO_ERROR && colors.size() > 10 ? 0 : 1;
}

} // namespace gs
