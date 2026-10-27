// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainTools.h"

#include "Viewport.h"

#include <QSettings>
#include <QTimer>

#include <cmath>

namespace gs {

namespace {

constexpr int kTickMs = 33;
// A click without holding still applies this much brushing.
constexpr double kMinStroke = 1.0 / 30.0;
constexpr int kCircleSegments = 64;

} // namespace

TerrainSettings& TerrainSettings::instance() {
    static TerrainSettings s;
    return s;
}

TerrainSettings::TerrainSettings() {
    QSettings s;
    brush_.radius = std::max(s.value("terrain/radius", brush_.radius).toDouble(), 1e-3);
    brush_.force = std::max(s.value("terrain/force", brush_.force).toDouble(), 1e-4);
    brush_.hardness = std::clamp(s.value("terrain/hardness", brush_.hardness).toDouble(), 0.0, 1.0);
}

void TerrainSettings::setBrush(const TerrainBrush& b) {
    brush_ = b;
    brush_.radius = std::max(brush_.radius, 1e-3);
    brush_.force = std::max(brush_.force, 1e-4);
    brush_.hardness = std::clamp(brush_.hardness, 0.0, 1.0);
    QSettings s;
    s.setValue("terrain/radius", brush_.radius);
    s.setValue("terrain/force", brush_.force);
    s.setValue("terrain/hardness", brush_.hardness);
    emit changed();
}

bool isTerrainTool(ToolId id) { return id == ToolId::Mountain || id == ToolId::Pit || id == ToolId::Flatten; }

TerrainTool::TerrainTool(Viewport* vp, ToolId id) : Tool(vp), id_(id), timer_(std::make_unique<QTimer>()) {
    timer_->setInterval(kTickMs);
    QObject::connect(timer_.get(), &QTimer::timeout, [this] { tick(); });
}

TerrainTool::~TerrainTool() = default;

TerrainMode TerrainTool::mode() const {
    switch (id_) {
    case ToolId::Pit: return TerrainMode::Lower;
    case ToolId::Flatten: return TerrainMode::Flatten;
    default: return TerrainMode::Raise;
    }
}

QString TerrainTool::status() const {
    const TerrainBrush& b = TerrainSettings::instance().brush();
    QString what = mode() == TerrainMode::Raise   ? QStringLiteral("raise a mountain")
                   : mode() == TerrainMode::Lower ? QStringLiteral("sink a pit")
                                                  : QStringLiteral("flatten to the height under the cursor");
    return QStringLiteral("Press and hold on a surface to %1. Radius %2, force %3 cm/s, hardness %4%. Type a radius.")
        .arg(what, fmtLen(b.radius))
        .arg(b.force * 100, 0, 'g', 4)
        .arg(std::lround(b.hardness * 100));
}

bool TerrainTool::surfaceAt(double x, double y, Vec3& p, Id& face, Vec3& toViewer) {
    double t;
    face = engine().pickFace(x, y, &t);
    if (face == kNone) return false;
    Ray r = cam().ray(x, y);
    p = r.at(t);
    toViewer = -r.dir;
    return true;
}

void TerrainTool::reset() {
    timer_->stop();
    if (stroke_) {
        stroke_.reset();
        doc().cancelOp();
    }
    refreshStatus();
    update();
}

void TerrainTool::mousePress(const MouseEvent& e) {
    lastMouse_ = e;
    if (e.button != Qt::LeftButton || stroke_) return;
    Vec3 p, toViewer;
    Id f;
    if (!surfaceAt(e.x, e.y, p, f, toViewer)) return;
    static const char* names[] = {"Mountain", "Pit", "Flatten"};
    doc().beginOp(names[(int)mode()]);
    stroke_.emplace(mode(), TerrainSettings::instance().brush());
    if (!stroke_->begin(mesh(), p, f, toViewer)) {
        stroke_.reset();
        doc().cancelOp();
        return;
    }
    center_ = p;
    normal_ = stroke_->direction();
    onSurface_ = true;
    applied_ = 0;
    clock_.start();
    timer_->start();
    update();
}

void TerrainTool::applyElapsed() {
    double dt = std::min(clock_.restart() / 1000.0, 0.1);
    if (!onSurface_ || dt <= 0) return;
    stroke_->apply(mesh(), center_, dt);
    applied_ += dt;
}

void TerrainTool::tick() {
    if (!stroke_) {
        timer_->stop();
        return;
    }
    // Follow the surface under the (possibly still) cursor as it changes.
    Vec3 p, toViewer;
    Id f;
    onSurface_ = surfaceAt(lastMouse_.x, lastMouse_.y, p, f, toViewer);
    if (onSurface_) center_ = p;
    applyElapsed();
    update();
}

void TerrainTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    Vec3 p, toViewer;
    Id f;
    onSurface_ = surfaceAt(e.x, e.y, p, f, toViewer);
    if (onSurface_) {
        center_ = p;
        if (!stroke_) normal_ = terrainNormal(mesh(), p, f, toViewer, TerrainSettings::instance().brush().radius);
    }
    update();
}

void TerrainTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !stroke_) return;
    timer_->stop();
    applyElapsed();
    if (applied_ < kMinStroke && onSurface_) stroke_->apply(mesh(), center_, kMinStroke - applied_);
    stroke_->finish(mesh(), center_);
    stroke_.reset();
    doc().commitOp();
    update();
}

bool TerrainTool::vcbEnter(const QString& text) {
    double r;
    if (!parseLen(text, r) || r <= 0) return false;
    TerrainBrush b = TerrainSettings::instance().brush();
    b.radius = r;
    TerrainSettings::instance().setBrush(b);
    setVcb(fmtLen(r));
    refreshStatus();
    return true;
}

void TerrainTool::buildPreview(Preview& pv) {
    if (!onSurface_) return;
    const TerrainBrush& b = TerrainSettings::instance().brush();
    const Color color = mode() == TerrainMode::Raise   ? Color::rgb8(200, 110, 20)
                        : mode() == TerrainMode::Lower ? Color::rgb8(30, 110, 220)
                                                       : Color::rgb8(30, 160, 60);
    Vec3 u, v;
    planeBasis(normal_, u, v);
    // Lift the rings a little off the surface so they read on flat ground.
    const Vec3 c = center_ + normal_ * (b.radius * 0.01);
    auto ring = [&](double r, Dash dash, float width) {
        for (int i = 0; i < kCircleSegments; ++i) {
            double a0 = 2 * M_PI * i / kCircleSegments, a1 = 2 * M_PI * (i + 1) / kCircleSegments;
            pv.line(c + (u * std::cos(a0) + v * std::sin(a0)) * r, c + (u * std::cos(a1) + v * std::sin(a1)) * r, color,
                    width, dash);
        }
    };
    ring(b.radius, Dash::None, 2.0f);
    if (b.hardness > 0.02) ring(b.radius * std::min(b.hardness, 0.95), Dash::Dashed, 1.0f);
    // Direction tick: up for Mountain, down for Pit, a short level cross for Flatten.
    const double len = b.radius * 0.35;
    switch (mode()) {
    case TerrainMode::Raise: pv.line(c, c + normal_ * len, color, 2.0f); break;
    case TerrainMode::Lower: pv.line(c, c - normal_ * len, color, 2.0f); break;
    case TerrainMode::Flatten:
        pv.line(c - u * (len * 0.5), c + u * (len * 0.5), color, 2.0f);
        pv.line(c - v * (len * 0.5), c + v * (len * 0.5), color, 2.0f);
        break;
    }
    pv.markers.push_back({center_, Preview::MarkerKind::Plain});
}

} // namespace gs
