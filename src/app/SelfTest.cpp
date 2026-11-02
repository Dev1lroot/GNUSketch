// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelfTest.h"

#include "GameMode.h"
#include "TextureTool.h"
#include "core/TextureMapping.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QJsonObject>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

#include "MainWindow.h"
#include "ModifyTools.h"
#include "TerrainTools.h"
#include "Viewport.h"
#include "core/Components.h"
#include "core/Solids.h"
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

int diffFrames(const QImage& a, const QImage& b) {
    int n = 0;
    for (int y = 0; y < std::min(a.height(), b.height()); ++y)
        for (int x = 0; x < std::min(a.width(), b.width()); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) ++n;
    return n;
}

void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void solidBoxAt(Mesh& m, const Vec3& mn, const Vec3& mx) {
    m.drawPolyline({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}}, true);
    const Vec3 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, mn.z);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && m.facePlane(f).contains(c) && m.pointInFace(f, c)) {
            m.pushPull(f, (mx.z - mn.z) * m.faces[f].normal.z);
            return;
        }
}

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

    // Solid tools: click the cutter, then the solid to cut; preselection; undo.
    {
        auto boxComponent = [&](Vec3 mn, Vec3 mx, const char* name) {
            doc.selection.clear();
            doc.beginOp("Box");
            Mesh& m = d.mesh();
            m.drawPolyline({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}}, true);
            Vec3 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, mn.z);
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && m.facePlane(f).contains(c) && m.pointInFace(f, c)) {
                    m.pushPull(f, (mx.z - mn.z) * m.faces[f].normal.z);
                    break;
                }
            Selection s;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && m.faceInteriorPoint(f).x > mn.x - 1e-6 && m.faceInteriorPoint(f).x < mx.x + 1e-6 &&
                    m.faceInteriorPoint(f).y > mn.y - 1e-6 && m.faceInteriorPoint(f).y < mx.y + 1e-6)
                    s.faces.insert(f);
            Id inst = makeComponent(doc.state, s, name);
            doc.commitOp();
            return inst;
        };
        auto volume = [&](Id inst) { return solidVolume(doc.state.defs[doc.state.instances[inst].def].mesh); };
        Id plate = boxComponent(Vec3(100, 0, 0), Vec3(102, 2, 1), "Plate");
        Id peg = boxComponent(Vec3(100.5, 0.5, 0), Vec3(101.5, 1.5, 2), "Peg");
        d.check(plate != kNone && peg != kNone && instanceIsSolid(doc.state, plate) && instanceIsSolid(doc.state, peg),
                "two solid components to cut");
        doc.selection.clear();
        doc.notifySelection();
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(99.5, -0.5, 0), Vec3(102.5, 2.5, 2));
        d.pump();

        const QPointF onPeg = d.screen(Vec3(101, 1, 2)), onPlate = d.screen(Vec3(101.8, 0.2, 1));
        vp->setTool(ToolId::SolidSubtract);
        d.click(onPeg);
        d.check(doc.selection.instances == std::set<Id>{peg}, "Subtract: first click picks the cutter");
        d.click(onPlate);
        d.check(!doc.state.instances[peg].alive && std::fabs(volume(plate) - 3) < 1e-6 && doc.undoName() == "Subtract" &&
                    doc.selection.instances == std::set<Id>{plate},
                "Subtract cuts a hole through the plate and removes the cutter");
        d.check(!vp->grabFramebuffer().isNull(), "renders the subtracted solid");
        doc.undo();
        d.check(doc.state.instances[peg].alive && std::fabs(volume(plate) - 4) < 1e-6, "undo restores both solids");

        doc.selection.clear();
        doc.selection.instances.insert(peg);
        doc.notifySelection();
        vp->setTool(ToolId::SolidTrim);
        d.click(onPlate);
        d.check(doc.state.instances[peg].alive && std::fabs(volume(plate) - 3) < 1e-6 && doc.undoName() == "Trim",
                "Trim with the cutter preselected keeps the cutter");
        doc.undo();

        Id lid = boxComponent(Vec3(100, 0, 1), Vec3(102, 2, 1.25), "Lid");
        doc.selection.clear();
        for (Id i : {plate, peg, lid}) doc.selection.instances.insert(i);
        doc.notifySelection();
        vp->setTool(ToolId::SolidUnion);
        d.pump();
        int alive = 0;
        for (Id i : {plate, peg, lid}) alive += doc.state.instances[i].alive;
        d.check(alive == 1 && doc.selection.instances.size() == 1 &&
                    std::fabs(volume(*doc.selection.instances.begin()) - (4 + 1 + 1 - 0.25)) < 1e-6,
                "Union of three preselected solids makes one");
        doc.undo();

        // A lone face is no solid: clicking it does nothing.
        doc.beginOp("Card");
        d.mesh().drawPolyline({{100, 3, 0}, {101, 3, 0}, {101, 3, 1}, {100, 3, 1}}, true);
        Selection card;
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive && d.mesh().faceInteriorPoint(f).y > 2.9) card.faces.insert(f);
        Id cardInst = makeComponent(doc.state, card, "Card");
        doc.commitOp();
        vp->setTool(ToolId::SolidUnion);
        std::string before = doc.undoName();
        d.click(d.screen(Vec3(100.5, 3, 0.5)));
        d.check(cardInst != kNone && doc.undoName() == before && doc.selection.instances.empty(),
                "a component that isn't closed can't be picked");
        vp->setTool(ToolId::Select);
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

    // Components offer the same snaps as loose geometry: midpoints, edges,
    // parallel/perpendicular to their edges and perpendicular to their faces.
    {
        auto boxInst = [&](Vec3 mn, Vec3 mx, const char* name) {
            doc.beginOp("Box");
            Mesh& m = d.mesh();
            m.drawPolyline({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}}, true);
            Vec3 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, mn.z);
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && m.facePlane(f).contains(c) && m.pointInFace(f, c)) {
                    m.pushPull(f, (mx.z - mn.z) * m.faces[f].normal.z);
                    break;
                }
            Selection s;
            for (Id f = 0; f < (Id)m.faces.size(); ++f) {
                Vec3 q = m.faceInteriorPoint(f);
                if (m.faces[f].alive && q.x > mn.x - 1e-6 && q.x < mx.x + 1e-6 && q.y > mn.y - 1e-6 && q.y < mx.y + 1e-6)
                    s.faces.insert(f);
            }
            Id inst = makeComponent(doc.state, s, name);
            doc.commitOp();
            return inst;
        };
        InferenceEngine& eng = vp->inference();
        InferenceEngine::Options none;
        Id base = boxInst(Vec3(70, 0, 0), Vec3(74, 2, 1), "Base");
        Id block = boxInst(Vec3(76, 0, 0), Vec3(77, 1, 0.5), "Block");
        doc.selection.clear();
        doc.notifySelection();
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(69.5, -0.5, 0), Vec3(77.5, 2.5, 1));
        d.pump();
        eng.resetMemory();
        const Vec3 topMid(72, 0, 1); // middle of the base's top front edge
        QPointF q = d.screen(topMid) + QPointF(1, 1);
        InferenceResult mid = eng.infer(q.x(), q.y(), none);
        d.check(mid.type == InferenceResult::Midpoint && mid.p.nearlyEquals(topMid, 1e-9) && mid.hasEdge,
                "midpoint of a component's edge");
        q = d.screen(Vec3(70.6, 0, 1)) + QPointF(0, 2);
        InferenceResult on = eng.infer(q.x(), q.y(), none);
        d.check(on.type == InferenceResult::OnEdge && on.edge == kNone && on.hasEdge && std::fabs(on.p.y) < 1e-6 &&
                    std::fabs(on.p.z - 1) < 1e-6,
                "on a component's edge");

        // Centre the block on the base: move its bottom front midpoint to the
        // base's top front midpoint.
        vp->setTool(ToolId::Move);
        doc.selection.clear();
        doc.selection.instances.insert(block);
        doc.notifySelection();
        d.click(d.screen(Vec3(76.5, 0, 0)) + QPointF(1, 1));
        d.click(d.screen(topMid) + QPointF(1, 1));
        Vec3 bmn, bmx;
        instanceBounds(doc.state, block, bmn, bmx);
        d.check(bmn.nearlyEquals(Vec3(71.5, 0, 1), 1e-9) && bmx.nearlyEquals(Vec3(72.5, 1, 1.5), 1e-9),
                "Move snaps a component midpoint to another component's midpoint (centred)");
        doc.undo();
        vp->setTool(ToolId::Select);

        // Rotated component: parallel / perpendicular to its edge, from a free point on the ground.
        const double ang = 0.5;
        doc.beginOp("Rotate");
        doc.state.instances[block].xf = Mat4::translation(Vec3(76, 0, 0)) * Mat4::rotation(Vec3(0, 0, 1), ang) *
                                        Mat4::translation(Vec3(-76, 0, 0)) * doc.state.instances[block].xf;
        doc.commitOp();
        vp->setStandardView(Vec3(0, 0, -1));
        vp->camera().zoomExtents(Vec3(69, -3, 0), Vec3(79, 4, 0));
        d.pump();
        eng.resetMemory();
        const Vec3 ex(std::cos(ang), std::sin(ang), 0), ey(-std::sin(ang), std::cos(ang), 0);
        q = d.screen(Vec3(76, 0, 0.5) + ex * 0.3);
        eng.infer(q.x(), q.y(), none); // hover the block's front edge
        InferenceEngine::Options o;
        o.ref = Vec3(76, -2.5, 0);
        o.plane = Plane(Vec3(0, 0, 1), Vec3());
        q = d.screen(*o.ref + ex * 1.7) + QPointF(1, 1);
        InferenceResult par = eng.infer(q.x(), q.y(), o);
        d.check(par.type == InferenceResult::Parallel && std::fabs(std::fabs(par.dir.dot(ex)) - 1) < 1e-9,
                "parallel to a component's edge");
        q = d.screen(*o.ref + ey * 1.3) + QPointF(1, 1);
        InferenceResult perp = eng.infer(q.x(), q.y(), o);
        d.check(perp.type == InferenceResult::Perpendicular && std::fabs(perp.dir.dot(ex)) < 1e-9,
                "perpendicular to a component's edge");

        // Tilted component: perpendicular to its sloped top face.
        doc.beginOp("Tilt");
        doc.state.instances[base].xf = Mat4::translation(Vec3(70, 0, 0)) * Mat4::rotation(Vec3(1, 0, 0), 0.4) *
                                       Mat4::translation(Vec3(-70, 0, 0)) * doc.state.instances[base].xf;
        doc.commitOp();
        const Vec3 n = Mat4::rotation(Vec3(1, 0, 0), 0.4).transformDir(Vec3(0, 0, 1));
        Mat4 tilt = Mat4::translation(Vec3(70, 0, 0)) * Mat4::rotation(Vec3(1, 0, 0), 0.4) * Mat4::translation(Vec3(-70, 0, 0));
        const Vec3 topPt = tilt.transformPoint(Vec3(71.3, 1.2, 1));
        vp->setStandardView(Vec3(1, 1, -0.8));
        vp->camera().zoomExtents(Vec3(69, -1, 0), Vec3(75, 3, 3));
        d.pump();
        eng.resetMemory();
        q = d.screen(topPt);
        eng.infer(q.x(), q.y(), none); // hover the sloped top
        InferenceEngine::Options from;
        from.ref = topPt;
        q = d.screen(topPt + n * 1.2) + QPointF(1, 1);
        InferenceResult fn = eng.infer(q.x(), q.y(), from);
        d.check(fn.type == InferenceResult::FaceNormal && std::fabs(fn.dir.dot(n) - 1) < 1e-9 &&
                    ((fn.p - topPt).cross(n)).length() < 1e-9,
                "perpendicular to a component's face");

        // Tape Measure: double-click a component edge for an infinite guide.
        vp->setTool(ToolId::Tape);
        size_t guides = doc.state.guides.size();
        const Vec3 edgePt = tilt.transformPoint(Vec3(70.7, 0, 1));
        d.dblclick(d.screen(edgePt));
        d.check(doc.state.guides.size() == guides + 1 && std::fabs(std::fabs(doc.state.guides.back().dir.x) - 1) < 1e-9,
                "Tape double-click on a component edge makes a guide along it");
        vp->setTool(ToolId::Select);
        eng.resetMemory();
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

        // View > Render > Ambient Occlusion darkens the inside corner where a
        // floor meets a wall.
        {
            doc.beginOp("AO box");
            d.mesh().drawPolyline({{302, 4, 0.5}, {306, 4, 0.5}, {306, 8, 0.5}, {302, 8, 0.5}}, true);
            d.mesh().drawPolyline({{302, 8, 0.5}, {306, 8, 0.5}, {306, 8, 2.5}, {302, 8, 2.5}}, true);
            doc.commitOp();
            QAction* ao = w.findChild<QAction*>(QStringLiteral("ambientocclusion"));
            d.check(ao && !ao->isChecked(), "View > Render has an Ambient Occlusion toggle");
            if (ao) {
                d.move(QPointF(5, 5));
                vp->update();
                QImage base = vp->grabFramebuffer();
                ao->trigger();
                QImage shaded = vp->grabFramebuffer();
                int darker = 0, lighter = 0;
                for (int y = 0; y < base.height(); ++y)
                    for (int x = 0; x < base.width(); ++x) {
                        int g0 = qGray(base.pixel(x, y)), g1 = qGray(shaded.pixel(x, y));
                        if (g1 + 6 < g0) ++darker;
                        if (g1 > g0 + 2) ++lighter;
                    }
                d.check(doc.style.ambientOcclusion && darker > base.width() * base.height() / 500 && lighter == 0,
                        "ambient occlusion darkens creases and never brightens");
                ao->trigger();
                d.check(vp->grabFramebuffer() == base, "switching ambient occlusion off restores the frame");
            }
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

    // Effects menu: settings dialog with live preview, on selected faces.
    {
        // The user's remembered effect settings are not the test's to overwrite.
        QSettings userSettings;
        userSettings.beginGroup(QStringLiteral("effects"));
        QVariantMap savedEffects;
        for (const QString& k : userSettings.childKeys()) savedEffects[k] = userSettings.value(k);
        userSettings.remove(QString());
        vp->setTool(ToolId::Select);
        vp->cancelTool();
        doc.reset();
        d.pump();
        d.mesh().viewHint = Vec3(0, 0, 10);
        d.mesh().groundFacesDown = false;
        d.mesh().drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}}, true);
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive) { d.mesh().pushPull(f, 2.0 * d.mesh().faces[f].normal.z); break; }
        doc.commitOp();
        vp->zoomExtents();
        d.pump();
        auto action = [&](const char* name) { return w.findChild<QAction*>(QString::fromLatin1(name)); };
        QAction* noise = action("effect_noise");
        QAction* hex = action("effect_remesh_to_hexagons");
        QAction* decimate = action("effect_decimate");
        QAction* blur = action("effect_blur");
        QAction* repeat = action("repeateffect");
        d.check(noise && hex && decimate && blur && repeat, "Effects menu has Noise, Blur, Decimate, Remesh and Repeat");
        if (noise && hex && decimate && blur && repeat) {
            doc.selection.clear();
            doc.notifySelection();
            d.check(!noise->isEnabled(), "effects are disabled without a selection");
            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
                if (d.mesh().faces[f].alive) doc.selection.faces.insert(f);
            doc.notifySelection();
            d.check(noise->isEnabled() && !repeat->isEnabled(), "effects enabled by the selection; nothing to repeat yet");

            // Drives the real dialog: changes a setting, sees the preview, accepts or cancels.
            auto throughDialog = [&](QAction* a, bool accept, const std::function<void(QDialog*)>& inDialog = {}) {
                bool shown = false;
                QTimer::singleShot(0, &w, [&] {
                    auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dlg) return;
                    shown = true;
                    if (inDialog) inDialog(dlg);
                    QDialogButtonBox* bb = dlg->findChild<QDialogButtonBox*>();
                    if (accept) bb->button(QDialogButtonBox::Ok)->click();
                    else bb->button(QDialogButtonBox::Cancel)->click();
                });
                a->trigger();
                d.pump();
                return shown;
            };

            const int faces = d.mesh().aliveFaceCount();
            const Selection selBefore = doc.selection;
            int during = 0;
            const bool shownCancel = throughDialog(noise, false, [&](QDialog*) { during = d.mesh().aliveFaceCount(); });
            d.check(shownCancel && during > faces, "the Noise dialog previews on the model");
            d.check(d.mesh().aliveFaceCount() == faces && doc.selection == selBefore, "cancelling an effect restores model and selection");

            const std::string undoBefore = doc.undoName();
            const bool shownOk = throughDialog(noise, true, [&](QDialog* dlg) {
                // Distance (first spin box) to 10 cm.
                if (auto* spin = dlg->findChild<QDoubleSpinBox*>()) spin->setValue(0.1);
                waitMs(250); // the preview follows the setting
            });
            d.check(shownOk && doc.undoName() == "Noise", "accepting keeps the effect as one undo step");
            d.check(isSolid(d.mesh()), "noise on a closed box stays closed");
            d.check(!doc.selection.faces.empty() && (int)doc.selection.faces.size() == d.mesh().aliveFaceCount(), "the result is selected");
            d.check(repeat->isEnabled() && repeat->text().contains("Noise"), "Repeat names the last effect");
            const int noisy = d.mesh().aliveFaceCount();
            repeat->trigger();
            d.pump();
            d.check(doc.undoName() == "Noise" && d.mesh().aliveFaceCount() >= noisy, "Repeat runs without a dialog");
            w.findChild<QAction*>(QStringLiteral("undo"))->trigger();
            w.findChild<QAction*>(QStringLiteral("undo"))->trigger();
            d.pump();
            d.check(d.mesh().aliveFaceCount() == faces, "undo brings the box back");

            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
                if (d.mesh().faces[f].alive) doc.selection.faces.insert(f);
            doc.notifySelection();
            d.check(throughDialog(hex, true), "Hexagons runs through its dialog");
            int sixes = 0;
            for (const Face& F : d.mesh().faces)
                if (F.alive && F.outer.size() == 6) ++sixes;
            d.check(sixes > 20 && isSolid(d.mesh()), "Hexagons turns the box into hexagon cells");
            QImage cells = vp->grabFramebuffer();
            d.check(!cells.isNull(), "the hexagon box renders");
            d.check(throughDialog(decimate, true), "Decimate runs through its dialog");
            d.check(isSolid(d.mesh()), "decimate leaves the box closed");
        }
        userSettings.remove(QString());
        for (auto it = savedEffects.cbegin(); it != savedEffects.cend(); ++it) userSettings.setValue(it.key(), it.value());
    }

    // Sun: lighting, the disc in the sky, the panel; culling; both together.
    {
        auto diffCount = [](const QImage& a, const QImage& b, int*) {
            int n = 0;
            if (a.size() != b.size()) return a.width() * a.height();
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x)
                    if (a.pixel(x, y) != b.pixel(x, y)) ++n;
            return n;
        };
        doc.reset();
        vp->setTool(ToolId::Select);
        d.mesh().viewHint = Vec3(0, 0, 10);
        d.mesh().groundFacesDown = false;
        // A single-sided square seen from above, and a closed box next to it.
        d.mesh().drawPolyline({{0, 0, 0.5}, {4, 0, 0.5}, {4, 4, 0.5}, {0, 4, 0.5}}, true);
        Camera& cam = vp->camera();
        cam.eye = Vec3(2, -6, 8);
        cam.target = Vec3(2, 2, 0.5);
        d.pump();
        const QImage top = vp->grabFramebuffer();
        QAction* sun = w.findChild<QAction*>(QStringLiteral("sun"));
        QAction* cull = w.findChild<QAction*>(QStringLiteral("backfaceculling"));
        d.check(sun && cull && sun->isChecked() && !cull->isChecked(), "View > Render has Sun (on) and Backface Culling (off)");
        if (sun && cull) {
            sun->trigger();
            d.pump();
            d.check(diffCount(top, vp->grabFramebuffer(), nullptr) > 500, "switching the sun off changes the lighting");
            sun->trigger();
            d.pump();
            d.check(diffCount(top, vp->grabFramebuffer(), nullptr) == 0, "switching it back restores the frame");

            // Evening: the same scene gets darker and warmer.
            doc.style.sunAltitude = 6;
            d.pump();
            const QImage evening = vp->grabFramebuffer();
            doc.style.sunAltitude = -20;
            d.pump();
            const QImage night = vp->grabFramebuffer();
            auto brightness = [](const QImage& im) {
                double sum = 0;
                for (int y = 0; y < im.height(); y += 4)
                    for (int x = 0; x < im.width(); x += 4) sum += qGray(im.pixel(x, y));
                return sum;
            };
            d.check(brightness(night) < brightness(evening) && brightness(evening) < brightness(top), "the frame darkens as the sun sets");
            // Looking towards the sun shows its disc in the sky.
            doc.style.sunAzimuth = 0;
            doc.style.sunAltitude = 20;
            cam.eye = Vec3(2, -6, 2);
            const Vec3 sd = doc.style.sunDirection();
            cam.target = cam.eye + sd;
            d.pump();
            const QImage toSun = vp->grabFramebuffer();
            const QRgb mid = toSun.pixel(toSun.width() / 2, toSun.height() / 2);
            d.check(qRed(mid) > 240 && qGreen(mid) > 235 && qBlue(mid) > 200, "the sun's disc is in the sky, in its direction");
            doc.style = RenderStyle();

            // Culling: the square faces up; from above it is there, from below gone.
            cam.eye = Vec3(2, -6, 8);
            cam.target = Vec3(2, 2, 0.5);
            d.pump();
            const QImage above = vp->grabFramebuffer();
            cull->trigger();
            d.pump();
            d.check(doc.style.backfaceCulling && diffCount(above, vp->grabFramebuffer(), nullptr) == 0, "culling keeps a face seen from the front");
            const QPointF centreOfSquare = d.screen(Vec3(2, 2, 0.5));
            Id pickedFront = vp->inference().pickFace(centreOfSquare.x(), centreOfSquare.y());
            d.check(pickedFront != kNone, "a face seen from the front is picked");
            cam.eye = Vec3(2, -4, -6);
            cam.target = Vec3(2, 2, 0.5);
            d.pump();
            const QImage culled = vp->grabFramebuffer();
            const QPointF below = d.screen(Vec3(2, 2, 0.5));
            d.check(vp->inference().pickFace(below.x(), below.y()) == kNone, "a culled face is not picked");
            cull->trigger();
            d.pump();
            const QImage shown = vp->grabFramebuffer();
            d.check(vp->inference().pickFace(below.x(), below.y()) != kNone, "without culling the back is picked again");
            d.check(diffCount(culled, shown, nullptr) > 1000, "culling removes the back of a face");
            // Mirrored component: winding flips, front stays front.
            doc.style.backfaceCulling = true;
            Selection all;
            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
                if (d.mesh().faces[f].alive) all.faces.insert(f);
            Id inst = makeComponent(doc.state, all, "Mirrored");
            doc.state.instances[inst].xf = Mat4::translation(Vec3(8, 0, 0)) * Mat4::scale(Vec3(-1, 1, 1));
            // The square now spans x = 4..8.
            cam.eye = Vec3(6, -6, 8);
            cam.target = Vec3(6, 2, 0.5);
            d.pump();
            const QPointF q = d.screen(Vec3(6, 2, 0.5));
            d.check(vp->inference().pickInstance(q.x(), q.y()) == inst, "a mirrored component is still picked from its front");
            const QImage mirroredFront = vp->grabFramebuffer();
            doc.style.backfaceCulling = false;
            d.pump();
            d.check(diffCount(mirroredFront, vp->grabFramebuffer(), nullptr) == 0, "a mirrored component is still drawn from its front");
        }
        doc.style = RenderStyle();
    }

    // The style survives saving, which is also how the game window gets it.
    {
        doc.style.sun = true;
        doc.style.sunAzimuth = 123;
        doc.style.sunAltitude = 17;
        doc.style.sunIntensity = 1.5;
        doc.style.backfaceCulling = true;
        doc.style.shadows = true;
        RenderStyle back;
        styleFromJson(styleToJson(doc.style), back);
        d.check(back.sunAzimuth == 123 && back.sunAltitude == 17 && back.sunIntensity == 1.5 && back.backfaceCulling && back.shadows,
                "sun, culling and shadows are saved with the view");
        doc.style = RenderStyle();
    }

    // Game Mode: the placement tool, and the window driven with real key events.
    {
        doc.reset();
        vp->setTool(ToolId::Select);
        d.mesh().viewHint = Vec3(0, 0, 10);
        d.mesh().groundFacesDown = false;
        d.mesh().drawPolyline({{-20, -20, 1}, {20, -20, 1}, {20, 20, 1}, {-20, 20, 1}}, true);
        Camera& cam = vp->camera();
        cam.eye = Vec3(0, -15, 15);
        cam.target = Vec3(0, 0, 1);
        d.pump();

        Vec3 placed;
        double placedYaw = 99;
        int spawns = 0;
        auto conn = QObject::connect(vp, &Viewport::characterPlaced, vp, [&](const Vec3& p, double y) {
            placed = p;
            placedYaw = y;
            ++spawns;
        }, Qt::DirectConnection);
        // The real slot starts the separate process: tested from outside instead.
        QObject::disconnect(vp, &Viewport::characterPlaced, &w, nullptr);
        QAction* gameAct = w.findChild<QAction*>(QStringLiteral("gamemode"));
        d.check(gameAct != nullptr, "Tools has Game Mode");
        vp->setTool(ToolId::PlaceCharacter);
        d.check(vp->toolId() == ToolId::PlaceCharacter, "Game Mode starts the placement tool");
        const QPointF spot = d.screen(Vec3(3, 2, 1));
        d.move(spot);
        const QImage ghost = vp->grabFramebuffer();
        d.check(!ghost.isNull(), "the character's capsule previews at the cursor");
        d.click(spot);
        d.pump();
        d.check(spawns == 1 && placed.nearlyEquals(Vec3(3, 2, 1), 1e-3), "clicking places the character on the surface");
        d.check(vp->toolId() == ToolId::Select, "the tool ends after placing");
        const Vec3 f = cam.forward();
        d.check(std::fabs(placedYaw - std::atan2(f.y, f.x)) < 1e-9, "the character looks the way the camera does");
        QObject::disconnect(conn);

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("game.gsketch"));
        solidBoxAt(d.mesh(), Vec3(8, -5, 1), Vec3(10, 5, 4)); // a wall ahead
        doc.style.shadows = true;
        QJsonObject view;
        view["style"] = styleToJson(doc.style);
        QString err;
        d.check(saveNative(doc, path, view, &err), "the model is saved for the game");
        doc.style = RenderStyle();

        GameWindow game;
        d.check(game.load(path, Vec3(3, 2, 1), 0.0, &err), "the game window loads the model");
        game.show();
        d.pump();
        waitMs(100);
        d.check(game.renderOk(), "the game window renders with its own OpenGL context");
        // Several frames, with the HUD's text between them. Text leaves GL state
        // behind; the world must survive it. The HUD's corner is left out.
        QImage frame;
        for (int i = 0; i < 3; ++i) frame = game.grabFramebuffer();
        QSet<QRgb> shades;
        for (int y = 60; y < frame.height(); y += 9)
            for (int x = 0; x < frame.width(); x += 9) shades.insert(frame.pixel(x, y));
        const QRgb overWall = frame.pixel(frame.width() / 2, 70);
        d.check(shades.size() >= 4 && qBlue(overWall) > qRed(overWall), "the game window shows the world (the sky, over the wall)");
        d.check(game.world().triangles().size() >= 12, "the collision world holds the model's triangles");

        auto keys = [&](QEvent::Type t, int k, const QString& text = {}) {
            QKeyEvent e(t, k, Qt::NoModifier, text);
            QApplication::sendEvent(&game, &e);
        };
        game.simulate(1.0);
        d.check(game.walker().grounded() && std::fabs(game.walker().feet().z - 1) < 0.01, "the character stands on the surface");
        const double eyeZ = game.walker().eye().z - game.walker().feet().z;
        d.check(eyeZ > 1.5 && eyeZ < 1.8, "the eye is below the top of the 1.80 m character");

        Vec3 start = game.walker().feet();
        keys(QEvent::KeyPress, Qt::Key_W, QStringLiteral("w"));
        game.simulate(0.5);
        keys(QEvent::KeyRelease, Qt::Key_W, QStringLiteral("w"));
        d.check(game.walker().feet().x > start.x + 0.8, "W walks forward");
        d.check(game.grabFramebuffer() != frame, "the view moves with the character");
        start = game.walker().feet();
        game.simulate(0.4);
        start = game.walker().feet();
        keys(QEvent::KeyPress, Qt::Key_A, QStringLiteral("a"));
        game.simulate(0.5);
        keys(QEvent::KeyRelease, Qt::Key_A, QStringLiteral("a"));
        d.check(game.walker().feet().y > start.y + 0.5, "A walks left");
        keys(QEvent::KeyPress, Qt::Key_W, QStringLiteral("w"));
        game.simulate(5.0);
        keys(QEvent::KeyRelease, Qt::Key_W, QStringLiteral("w"));
        d.check(game.walker().feet().x < 8 && game.walker().feet().x > 7.5, "the wall stops the character");
        keys(QEvent::KeyPress, Qt::Key_Space, QStringLiteral(" "));
        game.simulate(0.1);
        keys(QEvent::KeyRelease, Qt::Key_Space, QStringLiteral(" "));
        d.check(game.walker().feet().z > 1.1, "Space jumps");
        game.simulate(1.5);

        // Mouse look: a click captures, motion turns, Esc frees it.
        const double yaw0 = game.walker().yaw(), pitch0 = game.walker().pitch();
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100), QCursor::pos(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&game, &click);
        d.check(game.mouseCaptured(), "a click captures the mouse");
        auto motion = [&](QPointF at) {
            QMouseEvent m(QEvent::MouseMove, at, at, game.mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&game, &m);
        };
        const QPointF c = QPointF(game.rect().center());
        motion(c); // first report is skipped
        motion(c + QPointF(40, 0));
        d.check(game.walker().yaw() < yaw0, "moving the mouse right turns right");
        motion(c + QPointF(0, -40));
        d.check(game.walker().pitch() > pitch0, "moving the mouse up looks up");
        keys(QEvent::KeyPress, Qt::Key_Escape);
        d.check(!game.mouseCaptured() && game.isVisible(), "Esc frees the mouse first");
        keys(QEvent::KeyPress, Qt::Key_R, QStringLiteral("r"));
        d.check(game.walker().feet().nearlyEquals(Vec3(3, 2, 1), 0.01), "R returns to the start");
        keys(QEvent::KeyPress, Qt::Key_Escape);
        d.check(!game.isVisible(), "Esc again closes the window");
    }

    // Bend tool: straight stairs, the real clicks and typed angle.
    {
        doc.reset();
        vp->setTool(ToolId::Select);
        d.pump();
        Mesh& m = d.mesh();
        m.viewHint = Vec3(0, -10, 2);
        m.groundFacesDown = false;
        const int steps = 12;
        std::vector<Vec3> profile{{0, 0, 0}};
        for (int i = 0; i < steps; ++i) {
            profile.push_back({i * 0.3, 0, (i + 1) * 0.18});
            profile.push_back({(i + 1) * 0.3, 0, (i + 1) * 0.18});
        }
        profile.push_back({steps * 0.3, 0, 0});
        m.drawPolyline(profile, true);
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) { m.pushPull(f, m.faces[f].normal.y > 0 ? 1.2 : -1.2); break; }
        vp->zoomExtents();
        vp->setStandardView(Vec3(0, 0, -1)); // from above: every corner is in view
        d.pump();
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) doc.selection.faces.insert(f);
        doc.notifySelection();
        const double volume = solidVolume(m);
        const int facesBefore = m.aliveFaceCount();
        vp->setTool(ToolId::Bend);
        d.check(vp->toolId() == ToolId::Bend, "the Bend tool starts");
        // The stairs' nosing at each end, in view from above.
        const QPointF a = d.screen(Vec3(0, 0, 0.18)), b = d.screen(Vec3(steps * 0.3, 0, steps * 0.18));
        d.move(a);
        d.click(a);
        d.pump();
        d.move(b);
        d.click(b);
        d.pump();
        // The cursor to one side: the ghost and the typed value follow.
        d.move(b + QPointF(0, -80));
        const QImage ghost = vp->grabFramebuffer();
        d.check(!ghost.isNull() && m.aliveFaceCount() == facesBefore, "bending previews without touching the model");
        d.type(QStringLiteral("90"));
        d.pump();
        d.check(doc.undoName() == "Bend", "typing the angle bends the stairs, as one undo step");
        d.check(isSolid(d.mesh()) && std::fabs(solidVolume(d.mesh()) - volume * (1 - 0.6 / (steps * 0.3 / (kPi / 2)))) / volume < 0.02, "the bent stairs are closed, with the volume of their curved path");
        int treads = 0;
        for (const Face& F : d.mesh().faces)
            if (F.alive && F.normal.z > 0.999) ++treads;
        d.check(treads == steps, "each tread is still one flat face");
        const double R = steps * 0.3 / (kPi / 2);
        d.check(d.mesh().findVertex(Vec3(R, R, 0), 1e-3) != kNone && d.mesh().findVertex(Vec3(R, R, steps * 0.18), 1e-3) != kNone, "the stairs' run lies on a quarter circle");
        d.check(!doc.selection.faces.empty(), "the bent stairs stay selected");
        d.check(vp->toolId() == ToolId::Bend, "the tool is ready for another bend");
        w.findChild<QAction*>(QStringLiteral("undo"))->trigger();
        d.pump();
        d.check(d.mesh().aliveFaceCount() == facesBefore, "undo brings the straight stairs back");
        vp->setTool(ToolId::Select);
    }

    // Position Texture: real mouse drags on a textured face.
    {
        doc.reset();
        vp->setTool(ToolId::Select);
        Mesh& m = d.mesh();
        m.viewHint = Vec3(0, 0, 10);
        m.groundFacesDown = false;
        m.drawPolyline({{0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}}, true);
        Id face = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) face = f;
        QImage img(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) img.setPixel(x, y, (x < 32) == (y < 32) ? qRgb(220, 40, 40) : qRgb(40, 40, 220));
        Material mat;
        mat.name = "Checker";
        mat.texture = textureFromImage(img);
        mat.texture.worldWidth = mat.texture.worldHeight = 2;
        const Id matId = doc.addMaterial(mat);
        m.faces[face].front.material = matId;
        ++doc.materialsRevision;
        m.touch();
        vp->setStandardView(Vec3(0, 0, -1));
        vp->zoomExtents();
        d.pump();
        doc.style.showAxes = false;
        vp->setTool(ToolId::TexturePosition);
        d.check(vp->toolId() == ToolId::TexturePosition, "the Position Texture tool starts");
        const QPointF centre = d.screen(Vec3(2, 2, 0));
        d.move(centre);
        d.click(centre);
        d.pump();
        d.check(doc.selection.faces.count(face) == 1, "clicking a face selects it for the tool");
        auto* tool = dynamic_cast<TextureTool*>(vp->tool());
        d.check(tool != nullptr, "the tool is active");
        const Face& F = m.faces[face];
        auto uvNow = [&](const Vec3& p) { return faceTexCoord(d.mesh().faces[face], true, p, doc.material(matId)); };
        const Vec3 probe(1.3, 2.1, 0);

        // Moving: the image follows the cursor exactly, also while dragging.
        {
            const Vec2 before = uvNow(probe);
            const QImage still = vp->grabFramebuffer();
            const QPointF from = d.screen(Vec3(1.7, 1.2, 0)), to = d.screen(Vec3(2.2, 1.5, 0));
            d.move(from);
            d.send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            d.move(to, Qt::LeftButton);
            d.pump();
            const QImage live = vp->grabFramebuffer();
            d.check(diffFrames(still, live) > 500, "the texture changes on screen during the drag");
            d.send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
            d.pump();
            const Vec2 after = uvNow(probe + Vec3(0.5, 0.3, 0));
            d.check(std::fabs(after.x - before.x) < 2e-3 && std::fabs(after.y - before.y) < 2e-3, "moving: the image stays under the cursor");
            d.check(doc.undoName() == "Position Texture" && !tool->busy(), "a drag is one undo step");
        }
        // Esc during a drag restores it.
        {
            const Vec2 before = uvNow(probe);
            const QPointF from = d.screen(Vec3(2, 2, 0));
            d.move(from);
            d.send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            d.move(from + QPointF(40, 25), Qt::LeftButton);
            d.key(Qt::Key_Escape);
            d.send(QEvent::MouseButtonRelease, from + QPointF(40, 25), Qt::LeftButton, Qt::NoButton);
            d.pump();
            const Vec2 after = uvNow(probe);
            d.check(after.x == before.x && after.y == before.y && !tool->busy(), "Esc cancels a drag");
        }
        // Every handle, by dragging it, changes the image in its own way.
        auto dragHandle = [&](TextureTool::Handle h, QPointF by, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const auto at = tool->handleAt(h);
            if (!at) return false;
            d.move(*at);
            d.send(QEvent::MouseButtonPress, *at, Qt::LeftButton, Qt::LeftButton, mods);
            d.move(*at + by, Qt::LeftButton, mods);
            d.send(QEvent::MouseButtonRelease, *at + by, Qt::LeftButton, Qt::NoButton, mods);
            d.pump();
            return true;
        };
        auto axes = [&] { return explicitTexMap(d.mesh().faces[face], true, doc.material(matId)); };
        {
            d.check(F.front.tex.valid, "the face has its own mapping now");
            const TexMap a = axes();
            d.check(dragHandle(TextureTool::Handle::Corner2, QPointF(60, -60)), "a corner can be dragged");
            const TexMap b = axes();
            d.check(std::fabs(a.u.length() / b.u.length() - a.v.length() / b.v.length()) < 1e-6 && b.u.length() < a.u.length(), "a corner resizes in proportion");
            d.check(dragHandle(TextureTool::Handle::Corner2, QPointF(50, -10), Qt::ShiftModifier), "Shift+corner can be dragged");
            const TexMap c = axes();
            d.check(std::fabs(b.u.length() / c.u.length() - b.v.length() / c.v.length()) > 1e-3, "Shift+corner resizes freely");
            d.check(dragHandle(TextureTool::Handle::Edge1, QPointF(40, 0)), "a side square can be dragged");
            const TexMap e = axes();
            d.check(std::fabs(e.v.length() - c.v.length()) < 1e-6 && e.u.length() < c.u.length(), "a side square stretches one way only");
            d.check(std::fabs(e.u.dot(e.v)) < 1e-9, "stretching does not skew");
            d.check(dragHandle(TextureTool::Handle::Skew2, QPointF(40, 0)), "a diamond can be dragged");
            const TexMap sk = axes();
            d.check(std::fabs(sk.u.dot(sk.v)) > 1e-3, "a diamond skews");
            d.check(dragHandle(TextureTool::Handle::Rotate, QPointF(-30, 60)), "the circle can be dragged");
            const TexMap ro = axes();
            d.check(!ro.u.nearlyEquals(sk.u, 1e-6), "the circle rotates");
            d.check(std::fabs(ro.u.dot(ro.u) * ro.v.dot(ro.v) - ro.u.dot(ro.v) * ro.u.dot(ro.v) - (sk.u.dot(sk.u) * sk.v.dot(sk.v) - sk.u.dot(sk.v) * sk.u.dot(sk.v))) < 1e-6 * sk.u.dot(sk.u) * sk.v.dot(sk.v),
                    "rotating keeps the image's size");
        }
        // Click a handle, type a value; mirror by typing; Ctrl+Z.
        {
            // From a clean default mapping.
            d.mesh().faces[face].front.tex.valid = false;
            d.mesh().touch();
            d.pump();
            const TexMap a = axes();
            const auto rot = tool->handleAt(TextureTool::Handle::Rotate);
            if (rot) {
                d.move(*rot);
                d.click(*rot);
                d.pump();
                d.check(tool->busy(), "clicking a handle keeps it grabbed");
                d.type(QStringLiteral("90"));
                d.pump();
                d.check(!tool->busy(), "typing the value finishes");
            }
            const TexMap b = axes();
            d.check(std::fabs(a.u.dot(b.u)) < 1e-6 * a.u.dot(a.u) && std::fabs(b.u.length() - a.u.length()) < 1e-6, "typing 90 rotates exactly a quarter turn");
            const auto corner = tool->handleAt(TextureTool::Handle::Corner0);
            if (corner) {
                d.move(*corner);
                d.click(*corner);
                d.pump();
                d.type(QStringLiteral("-1,1"));
                d.pump();
            }
            const TexMap c = axes();
            d.check(c.u.dot(b.u) < 0 && c.v.nearlyEquals(b.v, 1e-9), "a negative factor mirrors");
            const QImage textured = vp->grabFramebuffer();
            d.check(!textured.isNull(), "the mirrored texture renders");
        }
        // Several faces move together.
        {
            d.mesh().drawPolyline({{4, 0, 0}, {6, 0, 0}, {6, 4, 0}, {4, 4, 0}}, true);
            // Drawing in the same plane rebuilds its faces: found again by position.
            Id second = kNone;
            for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f) {
                if (!d.mesh().faces[f].alive) continue;
                if (d.mesh().pointInFace(f, Vec3(2, 2, 0))) face = f;
                if (d.mesh().pointInFace(f, Vec3(5, 2, 0))) second = f;
            }
            d.mesh().faces[second].front.material = matId;
            d.mesh().touch();
            doc.selection.clear();
            doc.selection.faces = {face, second};
            doc.notifySelection();
            d.pump();
            const TexMap a0 = explicitTexMap(d.mesh().faces[second], true, doc.material(matId));
            const QPointF at = d.screen(Vec3(2.6, 1.3, 0)); // clear of the handles
            d.move(at);
            d.drag(at, at + QPointF(30, 0));
            d.pump();
            const TexMap a1 = explicitTexMap(d.mesh().faces[second], true, doc.material(matId));
            d.check(!a1.origin.nearlyEquals(a0.origin, 1e-6) && a1.u.nearlyEquals(a0.u, 1e-9), "all selected faces move together");
        }
        // Without a texture the tool says so and does nothing.
        {
            d.mesh().drawPolyline({{-3, 0, 0}, {-1, 0, 0}, {-1, 2, 0}, {-3, 2, 0}}, true);
            vp->setStandardView(Vec3(0, 0, -1));
            vp->zoomExtents();
            d.pump();
            const QPointF plain = d.screen(Vec3(-2, 1, 0));
            d.move(plain);
            d.click(plain);
            d.pump();
            d.check(tool->status().contains(QStringLiteral("no texture")), "a face without a texture is reported");
        }
        // Skewed maps survive saving.
        {
            d.mesh().faces[face].front.tex.v = d.mesh().faces[face].front.tex.v + d.mesh().faces[face].front.tex.u * 0.5;
            QByteArray json = modelToJson(doc.state);
            ModelState back;
            d.check(modelFromJson(json, back), "the model saves");
            const TexMap& t0 = d.mesh().faces[face].front.tex;
            const TexMap& t1 = back.mesh.faces[face].front.tex;
            d.check(t1.valid && t1.u.nearlyEquals(t0.u, 1e-9) && t1.v.nearlyEquals(t0.v, 1e-9) && t1.origin.nearlyEquals(t0.origin, 1e-9), "a skewed mapping is saved exactly");
        }
        doc.style = RenderStyle();
        vp->setTool(ToolId::Select);
    }

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
