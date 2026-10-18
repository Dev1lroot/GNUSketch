// SPDX-License-Identifier: GPL-3.0-or-later
#include "SelfTest.h"

#include "MainWindow.h"
#include "Viewport.h"
#include "io/NativeFormat.h"
#include "render/Renderer.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSet>
#include <QTemporaryDir>

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
    QPointF sp = d.screen(Vec3(-3, 0, 0.8));
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
