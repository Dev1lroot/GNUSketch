// SPDX-License-Identifier: GPL-3.0-or-later
#include "DrawTools.h"

#include "Viewport.h"

#include <QKeyEvent>
#include <QRegularExpression>

#include <cmath>

namespace gs {

namespace {

const Vec3 kAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};

// Parses "<dx,dy,dz>" (relative) or "[x,y,z]" (absolute) coordinates.
bool parseCoords(const QString& text, const Units& units, Vec3& out, bool& relative) {
    QString t = text.trimmed();
    if (t.size() < 2) return false;
    if (t.startsWith('<') && t.endsWith('>')) relative = true;
    else if (t.startsWith('[') && t.endsWith(']')) relative = false;
    else return false;
    QStringList parts = t.mid(1, t.size() - 2).split(QRegularExpression("[;,]"));
    if (parts.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        double v;
        if (!units.parseLength(parts[i].toStdString(), v)) return false;
        out[i] = v;
    }
    return true;
}

bool parseSides(const QString& text, int& sides) {
    QString t = text.trimmed().toLower();
    if (!t.endsWith('s')) return false;
    bool ok = false;
    int n = t.left(t.size() - 1).toInt(&ok);
    if (!ok || n < 3 || n > 999) return false;
    sides = n;
    return true;
}

std::vector<Vec3> arcPoints(const Vec3& A, const Vec3& B, const Vec3& n, double h, int segments) {
    Vec3 AB = B - A;
    double c = AB.length() * 0.5;
    if (c < kTol) return {A};
    Vec3 M = (A + B) * 0.5;
    Vec3 w = n.cross(AB / (2 * c)).normalized();
    if (std::fabs(h) < kTol) return {A, B};
    double ah = std::fabs(h), sg = h > 0 ? 1.0 : -1.0;
    double R = (c * c + ah * ah) / (2 * ah);
    Vec3 apexDir = w * sg;
    Vec3 C = M + apexDir * (ah - R);
    Vec3 vA = A - C;
    double half = std::atan2(c, R - ah);
    double sweep = 2 * half;
    Vec3 axis = vA.cross(apexDir).normalized();
    if (axis.length2() < 0.5) axis = n;
    std::vector<Vec3> pts;
    for (int i = 0; i <= segments; ++i) {
        Mat4 rot = Mat4::rotation(axis, sweep * i / segments);
        pts.push_back(C + rot.transformDir(vA));
    }
    pts.front() = A;
    pts.back() = B;
    return pts;
}

} // namespace

// ===========================================================================
// Line
// ===========================================================================

QString LineTool::status() const {
    if (!hasStart_) return QStringLiteral("Select start point.");
    return QStringLiteral("Select end point or enter length. Arrow keys lock axes, Shift locks the inference. Esc to finish.");
}

void LineTool::reset() {
    hasStart_ = false;
    dragging_ = false;
    clearLocks();
    setVcb(QString());
    refreshStatus();
    update();
}

void LineTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    InferenceResult r = inferPoint(e, hasStart_ ? std::optional<Vec3>(start_) : std::nullopt);
    if (!hasStart_) {
        start_ = r.p;
        hasStart_ = true;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        refreshStatus();
    } else {
        commit(start_, r.p);
    }
    update();
}

void LineTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    InferenceResult r = inferPoint(e, hasStart_ ? std::optional<Vec3>(start_) : std::nullopt);
    if (hasStart_) setVcb(fmtLen((r.p - start_).length()));
    update();
}

void LineTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (dragging_ && hasStart_ && std::hypot(e.x - pressX_, e.y - pressY_) > 6) {
        InferenceResult r = inferPoint(e, start_);
        commit(start_, r.p);
    }
    dragging_ = false;
}

void LineTool::mouseDoubleClick(const MouseEvent&) {
    hasStart_ = false;
    clearLocks();
    refreshStatus();
    update();
}

void LineTool::commit(const Vec3& a, const Vec3& b) {
    dragging_ = false;
    if (a.nearlyEquals(b)) return;
    int facesBefore = mesh().aliveFaceCount();
    doc().beginOp("Line");
    mesh().drawLine(a, b);
    doc().commitOp();
    bool closed = mesh().aliveFaceCount() > facesBefore;
    clearLocks();
    if (closed) {
        hasStart_ = false;
    } else {
        start_ = b;
        hasStart_ = true;
    }
    setVcb(fmtLen((b - a).length()));
    refreshStatus();
}

bool LineTool::vcbEnter(const QString& text) {
    Vec3 c;
    bool rel = false;
    if (parseCoords(text, doc().units, c, rel)) {
        if (!hasStart_ && rel) return false;
        if (!hasStart_) {
            start_ = c;
            hasStart_ = true;
            refreshStatus();
            update();
            return true;
        }
        commit(start_, rel ? start_ + c : c);
        update();
        return true;
    }
    if (!hasStart_) return false;
    double len;
    if (!parseLen(text, len) || std::fabs(len) < kTol) return false;
    Vec3 dir = inf_.p - start_;
    if (dir.length() < kTol) dir = lockDir_ ? *lockDir_ : Vec3(1, 0, 0);
    commit(start_, start_ + dir.normalized() * len);
    update();
    return true;
}

void LineTool::buildPreview(Preview& pv) {
    if (hasStart_) {
        Color c = hasLock() ? lockColor() : lineColor(start_, inf_.p);
        pv.line(start_, inf_.p, c, hasLock() ? 2.5f : 1.6f);
    }
    engine().addPreview(inf_, pv);
}

// ===========================================================================
// Freehand
// ===========================================================================

void FreehandTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    InferenceResult r = inferPoint(e, std::nullopt, std::nullopt, nullptr, false);
    plane_ = r.face != kNone ? mesh().facePlane(r.face) : Plane(Vec3(0, 0, 1), r.p);
    pts_ = {r.p};
    drawing_ = true;
    lastX_ = e.x;
    lastY_ = e.y;
}

void FreehandTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!drawing_) {
        inferPoint(e, std::nullopt, std::nullopt, nullptr, false);
        update();
        return;
    }
    if (std::hypot(e.x - lastX_, e.y - lastY_) < 5) return;
    Ray r = cam().ray(e.x, e.y);
    double t;
    if (intersectRayPlane(r, plane_, t)) {
        pts_.push_back(r.at(t));
        lastX_ = e.x;
        lastY_ = e.y;
    }
    update();
}

void FreehandTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !drawing_) return;
    drawing_ = false;
    if (pts_.size() < 2) { pts_.clear(); return; }
    bool closed = false;
    double sx, sy;
    if (pts_.size() > 3 && cam().project(pts_.front(), sx, sy) && std::hypot(sx - e.x, sy - e.y) < 12) {
        closed = true;
        pts_.pop_back();
    }
    doc().beginOp("Freehand");
    mesh().drawPolyline(pts_, closed, mesh().newCurveId());
    doc().commitOp();
    pts_.clear();
    update();
}

void FreehandTool::buildPreview(Preview& pv) {
    for (size_t i = 0; i + 1 < pts_.size(); ++i) pv.line(pts_[i], pts_[i + 1], Color::rgb8(0, 0, 0), 1.6f);
    if (!drawing_) engine().addPreview(inf_, pv);
}

// ===========================================================================
// Planar shape helpers
// ===========================================================================

bool PlanarShapeTool::keyPress(QKeyEvent* e) {
    int axis = -1;
    switch (e->key()) {
    case Qt::Key_Right: axis = 0; break;
    case Qt::Key_Left: axis = 1; break;
    case Qt::Key_Up: axis = 2; break;
    case Qt::Key_Down: forcedAxis_ = -1; mouseMove(lastMouse_); update(); return true;
    default: return Tool::keyPress(e);
    }
    forcedAxis_ = forcedAxis_ == axis ? -1 : axis;
    mouseMove(lastMouse_);
    update();
    return true;
}

Plane PlanarShapeTool::resolvePlane(const MouseEvent& e, const Vec3& p0, Id startFace) {
    if (forcedAxis_ >= 0) return Plane(kAxes[forcedAxis_], p0);
    if (startFace != kNone && mesh().faces[startFace].alive) return mesh().facePlane(startFace);
    // Hovering a face that contains the start point: draw on it.
    Id f = engine().pickFace(e.x, e.y);
    if (f != kNone) {
        Plane fp = mesh().facePlane(f);
        if (fp.contains(p0, kTol * 10)) return fp;
    }
    return Plane(Vec3(0, 0, 1), p0);
}

void PlanarShapeTool::shapeBasis(const Vec3& n, Vec3& u, Vec3& v) const {
    if (std::fabs(n.z) > 0.999) {
        u = Vec3(1, 0, 0);
        v = Vec3(0, 1, 0);
        return;
    }
    if (std::fabs(n.x) > 0.999) {
        u = Vec3(0, 1, 0);
        v = Vec3(0, 0, 1);
        return;
    }
    if (std::fabs(n.y) > 0.999) {
        u = Vec3(1, 0, 0);
        v = Vec3(0, 0, 1);
        return;
    }
    planeBasis(n, u, v);
}

// ===========================================================================
// Rectangle
// ===========================================================================

QString RectangleTool::status() const {
    if (!hasStart_) return QStringLiteral("Select first corner. Arrow keys choose the drawing plane (Right=red, Left=green, Up=blue).");
    return QStringLiteral("Select opposite corner or enter dimensions (width,height).");
}

void RectangleTool::reset() {
    hasStart_ = dragging_ = false;
    w_ = h_ = 0;
    hint_.clear();
    clearLocks();
    setVcb(QString());
    refreshStatus();
    update();
}

std::vector<Vec3> RectangleTool::corners() const {
    return {p0_, p0_ + u_ * w_, p0_ + u_ * w_ + v_ * h_, p0_ + v_ * h_};
}

void RectangleTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!hasStart_) {
        InferenceResult r = inferPoint(e, std::nullopt);
        p0_ = r.p;
        startFace_ = r.face;
        hasStart_ = true;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        plane_ = Plane(Vec3(0, 0, 1), p0_);
        refreshStatus();
        update();
        return;
    }
    mouseMove(e);
    commit();
}

void RectangleTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!hasStart_) {
        inferPoint(e, std::nullopt);
        update();
        return;
    }
    plane_ = resolvePlane(e, p0_, startFace_);
    shapeBasis(plane_.n, u_, v_);
    InferenceResult r = inferPoint(e, p0_, plane_);
    Vec3 d = r.p - p0_;
    w_ = d.dot(u_);
    h_ = d.dot(v_);
    hint_.clear();
    double px = cam().pixelSize(r.p);
    if (!r.isSnap() && std::fabs(w_) > px * 10) {
        if (std::fabs(std::fabs(h_) - std::fabs(w_)) < px * 6) {
            h_ = (h_ < 0 ? -1 : 1) * std::fabs(w_);
            hint_ = "Square";
        } else if (std::fabs(std::fabs(h_) - std::fabs(w_) * 0.618034) < px * 6) {
            h_ = (h_ < 0 ? -1 : 1) * std::fabs(w_) * 0.618034;
            hint_ = "Golden Section";
        } else if (std::fabs(std::fabs(w_) - std::fabs(h_) * 0.618034) < px * 6 && std::fabs(h_) > px * 10) {
            w_ = (w_ < 0 ? -1 : 1) * std::fabs(h_) * 0.618034;
            hint_ = "Golden Section";
        }
    }
    setVcb(fmtLen(std::fabs(w_)) + ", " + fmtLen(std::fabs(h_)));
    update();
}

void RectangleTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (dragging_ && hasStart_ && std::hypot(e.x - pressX_, e.y - pressY_) > 6) {
        mouseMove(e);
        commit();
    }
    dragging_ = false;
}

void RectangleTool::commit() {
    dragging_ = false;
    if (std::fabs(w_) < kTol * 10 || std::fabs(h_) < kTol * 10) return;
    doc().beginOp("Rectangle");
    mesh().drawPolyline(corners(), true);
    doc().commitOp();
    hasStart_ = false;
    hint_.clear();
    clearLocks();
    refreshStatus();
    update();
}

bool RectangleTool::vcbEnter(const QString& text) {
    if (!hasStart_) return false;
    std::optional<double> a, b;
    if (!parseLen2(text, a, b)) return false;
    if (a) w_ = (w_ < 0 ? -1 : 1) * std::fabs(*a);
    if (b) h_ = (h_ < 0 ? -1 : 1) * std::fabs(*b);
    if (a && !b && std::fabs(h_) < kTol) h_ = w_;
    commit();
    return true;
}

void RectangleTool::buildPreview(Preview& pv) {
    if (hasStart_) {
        auto c = corners();
        pv.polys.push_back({c, Color::rgb8(80, 80, 200, 50)});
        for (int i = 0; i < 4; ++i) pv.line(c[i], c[(i + 1) % 4], Color::rgb8(0, 0, 0), 1.6f);
        if (!hint_.empty()) {
            pv.line(c[0], c[2], Color::rgb8(220, 0, 220), 1.0f, Dash::Dashed);
            pv.labels.push_back({c[2], hint_});
        }
    }
    engine().addPreview(inf_, pv);
}

// ===========================================================================
// Circle / Polygon
// ===========================================================================

QString CircleTool::status() const {
    QString what = polygon_ ? QStringLiteral("polygon") : QStringLiteral("circle");
    if (!hasCenter_)
        return QStringLiteral("Select center point. Type e.g. 24s to set the number of sides. Arrow keys choose the plane.");
    return QStringLiteral("Select edge of %1 or enter radius.").arg(what);
}

QString CircleTool::vcbLabel() const { return hasCenter_ ? QStringLiteral("Radius") : QStringLiteral("Sides"); }

void CircleTool::reset() {
    hasCenter_ = dragging_ = false;
    radius_ = 0;
    clearLocks();
    setVcb(hasCenter_ ? QString() : QString::number(sides_));
    refreshStatus();
    update();
}

std::vector<Vec3> CircleTool::points() const {
    std::vector<Vec3> pts;
    Vec3 a = radiusDir_.normalized();
    Vec3 b = plane_.n.cross(a).normalized();
    for (int i = 0; i < sides_; ++i) {
        double t = 2 * kPi * i / sides_;
        pts.push_back(center_ + (a * std::cos(t) + b * std::sin(t)) * radius_);
    }
    return pts;
}

void CircleTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!hasCenter_) {
        InferenceResult r = inferPoint(e, std::nullopt);
        center_ = r.p;
        startFace_ = r.face;
        hasCenter_ = true;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        plane_ = resolvePlane(e, center_, startFace_);
        refreshStatus();
        vp_->refreshToolStatus();
        update();
        return;
    }
    mouseMove(e);
    commit();
}

void CircleTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!hasCenter_) {
        InferenceResult r = inferPoint(e, std::nullopt);
        plane_ = forcedAxis_ >= 0 ? Plane(kAxes[forcedAxis_], r.p)
               : r.face != kNone ? mesh().facePlane(r.face) : Plane(Vec3(0, 0, 1), r.p);
        update();
        return;
    }
    if (forcedAxis_ >= 0) plane_ = Plane(kAxes[forcedAxis_], center_);
    else if (startFace_ == kNone) plane_ = resolvePlane(e, center_, kNone);
    InferenceResult r = inferPoint(e, center_, plane_);
    Vec3 d = r.p - center_;
    d = d - plane_.n * d.dot(plane_.n);
    radius_ = d.length();
    if (radius_ > kTol) radiusDir_ = d / radius_;
    setVcb(fmtLen(radius_));
    update();
}

void CircleTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (dragging_ && hasCenter_ && std::hypot(e.x - pressX_, e.y - pressY_) > 6) {
        mouseMove(e);
        commit();
    }
    dragging_ = false;
}

void CircleTool::commit() {
    dragging_ = false;
    if (radius_ < kTol * 10) return;
    doc().beginOp(polygon_ ? "Polygon" : "Circle");
    mesh().drawPolyline(points(), true, polygon_ ? kNone : mesh().newCurveId());
    doc().commitOp();
    hasCenter_ = false;
    clearLocks();
    setVcb(QString::number(sides_));
    vp_->refreshToolStatus();
    update();
}

bool CircleTool::vcbEnter(const QString& text) {
    int s;
    if (parseSides(text, s) || (!hasCenter_ && (s = text.trimmed().toInt()) >= 3 && s <= 999)) {
        sides_ = s;
        if (!hasCenter_) setVcb(QString::number(sides_));
        update();
        return true;
    }
    if (!hasCenter_) return false;
    double r;
    if (!parseLen(text, r) || r <= kTol) return false;
    radius_ = r;
    commit();
    return true;
}

void CircleTool::buildPreview(Preview& pv) {
    if (hasCenter_ && radius_ > kTol) {
        auto pts = points();
        for (size_t i = 0; i < pts.size(); ++i) pv.line(pts[i], pts[(i + 1) % pts.size()], Color::rgb8(0, 0, 0), 1.6f);
        pv.line(center_, center_ + radiusDir_ * radius_, Color::rgb8(80, 80, 80), 1.0f, Dash::Dashed);
        pv.polys.push_back({pts, Color::rgb8(80, 80, 200, 40)});
    } else if (!hasCenter_ && inf_.type != InferenceResult::None) {
        // Small orientation hint circle at the cursor in the target plane.
        double r = cam().pixelSize(inf_.p) * 18;
        Vec3 a, b;
        planeBasis(plane_.n, a, b);
        Color c = forcedAxis_ >= 0 ? Renderer::axisColor(forcedAxis_) : Color::rgb8(0, 0, 200);
        for (int i = 0; i < 24; ++i) {
            double t0 = 2 * kPi * i / 24, t1 = 2 * kPi * (i + 1) / 24;
            pv.line(inf_.p + (a * std::cos(t0) + b * std::sin(t0)) * r, inf_.p + (a * std::cos(t1) + b * std::sin(t1)) * r, c, 1.2f);
        }
    }
    engine().addPreview(inf_, pv);
}

// ===========================================================================
// 2-point arc
// ===========================================================================

QString ArcTool::status() const {
    switch (stage_) {
    case 0: return QStringLiteral("Select start point of arc.");
    case 1: return QStringLiteral("Select end point of arc or enter chord length.");
    default: return QStringLiteral("Select bulge, or enter bulge distance (type e.g. 24s for segments).");
    }
}

QString ArcTool::vcbLabel() const {
    if (stage_ == 0) return QStringLiteral("Segments");
    return stage_ == 1 ? QStringLiteral("Length") : QStringLiteral("Bulge");
}

void ArcTool::reset() {
    stage_ = 0;
    bulge_ = 0;
    hint_.clear();
    clearLocks();
    setVcb(QString::number(segments_));
    refreshStatus();
    update();
}

Plane ArcTool::arcPlane(const MouseEvent& e) {
    Vec3 d = b_ - a_;
    if (forcedAxis_ >= 0) {
        Vec3 n = kAxes[forcedAxis_];
        if (std::fabs(n.dot(d.normalized())) < 1e-6) return Plane(n, a_);
    }
    if (startFace_ != kNone && mesh().faces[startFace_].alive) {
        Plane fp = mesh().facePlane(startFace_);
        if (fp.contains(b_, kTol * 10)) return fp;
    }
    Id f = engine().pickFace(e.x, e.y);
    if (f != kNone) {
        Plane fp = mesh().facePlane(f);
        if (fp.contains(a_, kTol * 10) && fp.contains(b_, kTol * 10)) return fp;
    }
    Vec3 dn = d.normalized();
    if (std::fabs(dn.z) < 1e-9) return Plane(Vec3(0, 0, 1), a_);
    if (std::fabs(dn.z) > 1 - 1e-9) {
        Vec3 f2 = cam().forward();
        return Plane(std::fabs(f2.x) > std::fabs(f2.y) ? Vec3(1, 0, 0) : Vec3(0, 1, 0), a_);
    }
    return Plane(dn.cross(Vec3(0, 0, 1)).normalized(), a_);
}

std::vector<Vec3> ArcTool::points() const { return arcPoints(a_, b_, plane_.n, bulge_, segments_); }

void ArcTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (stage_ == 0) {
        InferenceResult r = inferPoint(e, std::nullopt);
        a_ = r.p;
        startFace_ = r.face;
        stage_ = 1;
    } else if (stage_ == 1) {
        InferenceResult r = inferPoint(e, a_);
        if (r.p.nearlyEquals(a_)) return;
        b_ = r.p;
        stage_ = 2;
        clearLocks();
        plane_ = arcPlane(e);
    } else {
        mouseMove(e);
        commit();
        return;
    }
    refreshStatus();
    update();
}

void ArcTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (stage_ == 0) {
        inferPoint(e, std::nullopt);
    } else if (stage_ == 1) {
        InferenceResult r = inferPoint(e, a_);
        cur_ = r.p;
        setVcb(fmtLen((cur_ - a_).length()));
    } else {
        plane_ = arcPlane(e);
        InferenceResult r = inferPoint(e, std::nullopt, plane_, nullptr, false);
        Vec3 AB = b_ - a_;
        Vec3 M = (a_ + b_) * 0.5;
        Vec3 w = plane_.n.cross(AB.normalized()).normalized();
        bulge_ = (r.p - M).dot(w);
        hint_.clear();
        double c = AB.length() * 0.5;
        double px = cam().pixelSize(M);
        if (std::fabs(std::fabs(bulge_) - c) < px * 6) {
            bulge_ = (bulge_ < 0 ? -1 : 1) * c;
            hint_ = "Half Circle";
        }
        setVcb(fmtLen(std::fabs(bulge_)));
    }
    update();
}

void ArcTool::commit() {
    if (stage_ != 2) return;
    auto pts = points();
    if (pts.size() < 2) return;
    doc().beginOp("Arc");
    mesh().drawPolyline(pts, false, std::fabs(bulge_) > kTol ? mesh().newCurveId() : kNone);
    doc().commitOp();
    stage_ = 0;
    hint_.clear();
    setVcb(QString::number(segments_));
    refreshStatus();
    update();
}

bool ArcTool::vcbEnter(const QString& text) {
    int s;
    if (parseSides(text, s) || (stage_ == 0 && (s = text.trimmed().toInt()) >= 2 && s <= 999)) {
        segments_ = std::max(2, s);
        if (stage_ == 0) setVcb(QString::number(segments_));
        update();
        return true;
    }
    double v;
    if (!parseLen(text, v)) return false;
    if (stage_ == 1) {
        Vec3 d = cur_ - a_;
        if (d.length() < kTol) d = Vec3(1, 0, 0);
        b_ = a_ + d.normalized() * v;
        stage_ = 2;
        plane_ = arcPlane(lastMouse_);
        refreshStatus();
        update();
        return true;
    }
    if (stage_ == 2) {
        bulge_ = (bulge_ < 0 ? -1 : 1) * std::fabs(v);
        commit();
        return true;
    }
    return false;
}

void ArcTool::buildPreview(Preview& pv) {
    if (stage_ == 1) {
        pv.line(a_, cur_, lineColor(a_, cur_), 1.4f, Dash::Dashed);
    } else if (stage_ == 2) {
        pv.line(a_, b_, Color::rgb8(120, 120, 120), 1.0f, Dash::Dashed);
        auto pts = points();
        for (size_t i = 0; i + 1 < pts.size(); ++i) pv.line(pts[i], pts[i + 1], Color::rgb8(0, 0, 0), 1.6f);
        if (!hint_.empty()) pv.labels.push_back({(a_ + b_) * 0.5, hint_});
    }
    if (stage_ != 2) engine().addPreview(inf_, pv);
}

} // namespace gs
