// SPDX-License-Identifier: GPL-3.0-or-later
#include "OffscreenRenderer.h"

#include "Renderer.h"
#include "core/Components.h"
#include "io/ImageUtil.h"

#include <QCoreApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

namespace gs {

namespace {

// A smooth sphere whose texture is projected along the viewing direction, so a
// tile shows on it without seams.
Mesh ballMesh(const Vec3& towardsCamera) {
    constexpr int kRings = 24, kSegs = 40;
    Mesh m;
    std::vector<std::vector<Id>> vs(kRings + 1, std::vector<Id>(kSegs));
    for (int r = 0; r <= kRings; ++r)
        for (int s = 0; s < kSegs; ++s) {
            const double th = kPi * r / kRings, ph = 2 * kPi * s / kSegs;
            if ((r == 0 || r == kRings) && s > 0) {
                vs[r][s] = vs[r][0];
                continue;
            }
            vs[r][s] = m.addVertexRaw(Vec3(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)));
        }
    for (int r = 0; r < kRings; ++r)
        for (int s = 0; s < kSegs; ++s) {
            const int s2 = (s + 1) % kSegs;
            std::vector<Id> loop;
            if (r == 0) loop = {vs[0][0], vs[1][s], vs[1][s2]};
            else if (r == kRings - 1) loop = {vs[r][s], vs[kRings][0], vs[r][s2]};
            else loop = {vs[r][s], vs[r + 1][s], vs[r + 1][s2], vs[r][s2]};
            for (size_t i = 0; i < loop.size(); ++i) {
                const Id e = m.addEdgeRaw(loop[i], loop[(i + 1) % loop.size()]);
                m.edges[e].soft = m.edges[e].smooth = true;
            }
            Vec3 c;
            for (Id v : loop) c += m.pos(v);
            const Id f = m.addFaceRaw(loop, {}, (c / (double)loop.size()).normalized());
            if (f != kNone) {
                m.faces[f].front.material = 0;
                m.faces[f].back.material = 0;
                m.faces[f].front.axis = towardsCamera;
                m.faces[f].back.axis = towardsCamera;
            }
        }
    m.touch();
    return m;
}

void cleanStyle(RenderStyle& s) {
    s.showAxes = s.showGuides = s.showObjects = s.showHidden = false;
    s.edges = false;
    s.profiles = false;
    s.sky = s.ground = false;
    s.shadows = false;
    s.background = Color::rgb8(236, 236, 236);
}

} // namespace

OffscreenRenderer& OffscreenRenderer::instance() {
    static OffscreenRenderer* self = new OffscreenRenderer; // lives as long as the application
    return *self;
}

OffscreenRenderer::OffscreenRenderer() {
    // Earlier than ~QApplication: GL objects need the platform plugin alive.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { shutdown(); });
}

OffscreenRenderer::~OffscreenRenderer() = default;

bool OffscreenRenderer::ensure() {
    if (failed_) return false;
    if (renderer_) return context_->makeCurrent(surface_.get());
    context_ = std::make_unique<QOpenGLContext>();
    context_->setFormat(QSurfaceFormat::defaultFormat());
    surface_ = std::make_unique<QOffscreenSurface>();
    if (!context_->create()) {
        failed_ = true;
        return false;
    }
    surface_->setFormat(context_->format());
    surface_->create();
    if (!context_->makeCurrent(surface_.get())) {
        failed_ = true;
        return false;
    }
    renderer_ = std::make_unique<Renderer>();
    QString log;
    if (!renderer_->initialize(&log)) {
        qWarning("Offscreen renderer: %s", qPrintable(log));
        renderer_.reset();
        failed_ = true;
        return false;
    }
    return true;
}

void OffscreenRenderer::shutdown() {
    if (renderer_ && context_->makeCurrent(surface_.get())) {
        renderer_->cleanup();
        context_->doneCurrent();
    }
    renderer_.reset();
    context_.reset();
    surface_.reset();
    failed_ = true;
}

QImage OffscreenRenderer::renderDocument(Document& doc, const Camera& cam, int size) {
    if (!ensure()) return {};
    QImage out;
    {
        QOpenGLFramebufferObjectFormat ff;
        ff.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        ff.setSamples(4);
        QOpenGLFramebufferObject fbo(size, size, ff);
        if (fbo.isValid() && fbo.bind()) {
            renderer_->invalidate();
            renderer_->render(doc, cam, Preview(), 1.0);
            out = fbo.toImage();
            fbo.release();
        }
    }
    context_->doneCurrent();
    return out;
}

QImage OffscreenRenderer::materialBall(const Material& m, int size) {
    const Vec3 toCam = Vec3(-0.45, -1, 0.35).normalized();
    Document doc;
    cleanStyle(doc.style);
    doc.style.sunAzimuth = 250;
    doc.style.sunAltitude = 40;
    Material prev = m;
    prev.setTileSize(1.2, 1.2); // about two tiles across the ball, whatever its real size
    doc.state.materials.push_back(prev);
    doc.state.mesh = ballMesh(toCam);
    if (prev.opacity < 0.999f || prev.blended()) {
        // A checker behind it, or glass would be invisible.
        QImage check(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) check.setPixel(x, y, ((x / 32 + y / 32) & 1) ? qRgb(255, 255, 255) : qRgb(120, 120, 120));
        Material backdrop;
        backdrop.name = "backdrop";
        backdrop.texture = textureFromImage(check);
        backdrop.setTileSize(1.0, 1.0);
        doc.state.materials.push_back(backdrop);
        Vec3 u, v;
        planeBasis(toCam, u, v);
        const Vec3 c = toCam * -3.0;
        Mesh& mm = doc.state.mesh;
        const Id q[4] = {mm.addVertexRaw(c - u * 5 - v * 5), mm.addVertexRaw(c + u * 5 - v * 5), mm.addVertexRaw(c + u * 5 + v * 5), mm.addVertexRaw(c - u * 5 + v * 5)};
        for (int i = 0; i < 4; ++i) mm.addEdgeRaw(q[i], q[(i + 1) % 4]);
        const Id f = mm.addFaceRaw({q[0], q[1], q[2], q[3]}, {}, toCam);
        if (f != kNone) mm.faces[f].front.material = 1;
        mm.touch();
    }
    Camera cam;
    cam.target = Vec3();
    cam.fovY = 26;
    cam.eye = toCam * (1.08 / std::sin(cam.fovY * 0.5 * kPi / 180));
    cam.setViewport(size, size);
    cam.setSceneRadius(Vec3(), 1);
    return renderDocument(doc, cam, size);
}

QImage OffscreenRenderer::modelThumbnail(const ModelState& state, int size) {
    Document doc;
    doc.state = closedState(state);
    doc.state.objects.clear();
    doc.state.mesh.touch();
    for (ComponentDef& d : doc.state.defs) d.mesh.touch();
    cleanStyle(doc.style);
    doc.style.edges = true;
    doc.style.profiles = true;
    Vec3 mn, mx;
    bool any = false;
    modelBounds(doc.state, mn, mx, any);
    if (!any) mn = Vec3(-1, -1, -1), mx = Vec3(1, 1, 1);
    Camera cam;
    cam.setViewport(size, size);
    cam.setSceneRadius((mn + mx) * 0.5, std::max((mx - mn).length() * 0.5, 0.01));
    cam.zoomExtents(mn, mx);
    return renderDocument(doc, cam, size);
}

bool OffscreenRenderer::compileShader(const std::string& source, QString* log) {
    if (!ensure()) {
        if (log) *log = QStringLiteral("OpenGL is unavailable.");
        return false;
    }
    const bool ok = renderer_->compileSurfaceShader(source, log);
    context_->doneCurrent();
    return ok;
}

} // namespace gs
