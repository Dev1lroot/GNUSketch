// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraTools.h"

#include "Viewport.h"

#include <QPainter>

#include <cmath>
#include <limits>

namespace gs {

Vec3 focusPointAt(InferenceEngine& eng, const Camera& cam, double x, double y) {
    double t = std::numeric_limits<double>::infinity();
    Ray r = cam.ray(x, y);
    eng.pickFace(x, y, &t); // also hits components
    if (std::isfinite(t)) return r.at(t);
    Plane ground(Vec3(0, 0, 1), Vec3());
    if (intersectRayPlane(r, ground, t) && t < cam.distance() * 50) return r.at(t);
    Plane pv(cam.forward(), cam.target);
    if (intersectRayPlane(r, pv, t)) return r.at(t);
    return cam.target;
}

// ---- Orbit -------------------------------------------------------------------

void OrbitTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    dragging_ = true;
    last_ = QPointF(e.x, e.y);
    grab_ = focusPointAt(engine(), cam(), e.x, e.y);
    pivot_ = vp_->orbitPivotAt(e.x, e.y);
}

void OrbitTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!dragging_) return;
    QPointF p(e.x, e.y);
    QPointF d = p - last_;
    if (e.shift()) cam().panTo(grab_, e.x, e.y);
    else cam().orbitAround(pivot_, d.x(), d.y());
    last_ = p;
    vp_->cameraChanged();
}

// ---- Pan ---------------------------------------------------------------------

void PanTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    dragging_ = true;
    grab_ = focusPointAt(engine(), cam(), e.x, e.y);
}

void PanTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!dragging_) return;
    cam().panTo(grab_, e.x, e.y);
    vp_->cameraChanged();
}

// ---- Zoom --------------------------------------------------------------------

void ZoomTool::activate() { setVcb(QString::number(cam().fovY, 'f', 1) + QStringLiteral("°")); }

void ZoomTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    dragging_ = true;
    last_ = QPointF(e.x, e.y);
    focus_ = focusPointAt(engine(), cam(), e.x, e.y);
}

void ZoomTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!dragging_) return;
    double dy = e.y - last_.y();
    last_ = QPointF(e.x, e.y);
    if (e.shift()) {
        cam().fovY = std::clamp(cam().fovY + dy * 0.2, 1.0, 120.0);
        setVcb(QString::number(cam().fovY, 'f', 1) + QStringLiteral("°"));
    } else {
        cam().zoom(std::pow(1.01, dy), &focus_);
    }
    vp_->cameraChanged();
}

bool ZoomTool::vcbEnter(const QString& text) {
    QString t = text.trimmed();
    t.remove(QStringLiteral("°"));
    t.remove(QStringLiteral("deg"));
    double v;
    if (!parseNumber(t.toStdString(), v) || v < 1 || v > 120) return false;
    cam().fovY = v;
    setVcb(QString::number(v, 'f', 1) + QStringLiteral("°"));
    vp_->cameraChanged();
    return true;
}

// ---- Zoom window ---------------------------------------------------------------

void ZoomWindowTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    dragging_ = true;
    a_ = b_ = QPointF(e.x, e.y);
}

void ZoomWindowTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!dragging_) return;
    b_ = QPointF(e.x, e.y);
    update();
}

void ZoomWindowTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    b_ = QPointF(e.x, e.y);
    if ((b_ - a_).manhattanLength() > 8) {
        cam().zoomWindow(a_.x(), a_.y(), b_.x(), b_.y());
        vp_->cameraChanged();
    }
    update();
}

void ZoomWindowTool::paintOverlay(QPainter& p) {
    if (!dragging_) return;
    p.setPen(QPen(Qt::black, 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(a_, b_).normalized());
}

} // namespace gs
