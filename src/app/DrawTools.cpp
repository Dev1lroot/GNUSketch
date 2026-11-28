// SPDX-License-Identifier: GPL-3.0-or-later
#include "DrawTools.h"

#include "Viewport.h"

#include <QKeyEvent>
#include <QRegularExpression>

#include <cmath>

namespace forma {

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

// Lines from a start point square (90 degrees) to the edges meeting there:
// in the plane of each face along such an edge and along that face's normal,
// square to two of the edges at once, or (an edge without faces) the level
// and the upright line square to it. `edges` are the edges at the start.
std::vector<Vec3> squareDirections(const Mesh& m, const std::vector<Id>& edges, std::optional<Vec3> primary) {
    std::vector<Vec3> bases, out;
    std::vector<std::vector<Vec3>> normals;
    auto addBase = [&](const Vec3& d, std::vector<Vec3> ns) {
        if (d.length() < kTol) return;
        const Vec3 e = d.normalized();
        for (size_t i = 0; i < bases.size(); ++i)
            if (std::fabs(bases[i].dot(e)) > 1 - 1e-9) {
                normals[i].insert(normals[i].end(), ns.begin(), ns.end());
                return;
            }
        bases.push_back(e);
        normals.push_back(std::move(ns));
    };
    if (primary) addBase(*primary, {});
    for (Id e : edges) {
        const Edge& E = m.edges[e];
        std::vector<Vec3> ns;
        for (Id f : m.edgeFaces(e))
            if (m.faces[f].alive) ns.push_back(m.facePlane(f).n);
        addBase(m.pos(E.v[1]) - m.pos(E.v[0]), std::move(ns));
    }
    for (size_t i = 0; i < bases.size(); ++i) {
        const Vec3& e = bases[i];
        for (const Vec3& n : normals[i]) {
            out.push_back(n.cross(e));
            out.push_back(n);
        }
        if (normals[i].empty()) {
            const Vec3 h = e.cross(Vec3(0, 0, 1));
            if (h.length() > 1e-6) {
                out.push_back(h);
                out.push_back(h.cross(e));
            }
        }
        for (size_t j = i + 1; j < bases.size(); ++j) out.push_back(e.cross(bases[j]));
    }
    std::vector<Vec3> uniq;
    for (const Vec3& d : out) {
        if (d.length() < 1e-9) continue;
        const Vec3 u = d.normalized();
        bool dup = false;
        for (const Vec3& w : uniq) dup = dup || std::fabs(w.dot(u)) > 1 - 1e-9;
        if (!dup) uniq.push_back(u);
    }
    return uniq;
}

} // namespace

// ===========================================================================
// Line
// ===========================================================================

QString LineTool::status() const {
    if (!hasStart_) return QStringLiteral("Select start point.");
    if (baseDir_)
        return QStringLiteral("Select end point or enter length. Square to the start edge is inferred; hold Shift to step the angle by 5\u00b0. "
                              "Arrow keys lock axes. Esc to finish.");
    return QStringLiteral("Select end point or enter length. Arrow keys lock axes, Shift locks the inference. Esc to finish.");
}

std::vector<Id> LineTool::edgesAt(Id vertex) {
    std::vector<Id> es;
    if (vertex == kNone) return es;
    const Mesh& m = mesh();
    for (Id e : m.vertexEdges(vertex))
        if (m.edges[e].alive && !m.edges[e].soft && !m.edges[e].hidden) es.push_back(e);
    return es;
}

void LineTool::setBase(const InferenceResult& r) {
    base_.reset();
    baseDir_.reset();
    squareDirs_.clear();
    const bool onEdge = r.type == InferenceResult::Midpoint || r.type == InferenceResult::OnEdge ||
                        r.type == InferenceResult::OnIntersection || r.type == InferenceResult::Intersection;
    const Mesh& m = mesh();
    if (onEdge && r.hasEdge && (r.edgeB - r.edgeA).length() > kTol) {
        base_ = std::make_pair(r.edgeA, r.edgeB);
        baseDir_ = (r.edgeB - r.edgeA).normalized();
        std::vector<Id> es;
        if (r.edge != kNone && r.edge < (Id)m.edges.size() && m.edges[r.edge].alive) es.push_back(r.edge);
        squareDirs_ = squareDirections(m, es, baseDir_);
        return;
    }
    // Only a line drawn from an edge is inferred square to it.
    if (r.type != InferenceResult::Endpoint || r.vertex == kNone) return;
    const std::vector<Id> es = edgesAt(r.vertex);
    squareDirs_ = squareDirections(m, es, std::nullopt);
    // The angle reference: a vertex where a line was split (two edges in
    // line), or the end of a lone edge.
    auto seg = [&](Id e) { return std::make_pair(m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1])); };
    for (size_t i = 0; i < es.size(); ++i)
        for (size_t j = i + 1; j < es.size(); ++j) {
            const Vec3 a = m.pos(m.edges[es[i]].other(r.vertex)), b = m.pos(m.edges[es[j]].other(r.vertex));
            const Vec3 da = (a - r.p).normalized(), db = (b - r.p).normalized();
            if (da.dot(db) < -1 + 1e-9) {
                base_ = std::make_pair(a, b);
                baseDir_ = (b - a).normalized();
                return;
            }
        }
    if (es.size() == 1) {
        base_ = seg(es[0]);
        baseDir_ = (base_->second - base_->first).normalized();
    }
}

bool LineTool::keyPress(QKeyEvent* e) {
    // From an edge, Shift steps the angle to it instead of locking the inference.
    if (e->key() == Qt::Key_Shift && hasStart_ && baseDir_ && !lockDir_ && heldAxis_ < 0) {
        if (!e->isAutoRepeat() && !angleStep_) {
            angleStep_ = true;
            lastMouse_.mods |= Qt::ShiftModifier;
            mouseMove(lastMouse_);
        }
        return false;
    }
    return Tool::keyPress(e);
}

bool LineTool::keyRelease(QKeyEvent* e) {
    if (e->key() == Qt::Key_Shift && !e->isAutoRepeat() && angleStep_) {
        angleStep_ = false;
        lastMouse_.mods &= ~Qt::ShiftModifier;
        mouseMove(lastMouse_);
        return false;
    }
    return Tool::keyRelease(e);
}

void LineTool::reset() {
    base_.reset();
    baseDir_.reset();
    squareDirs_.clear();
    angleStep_ = false;
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
    angleStep_ = hasStart_ && baseDir_ && e.shift();
    InferenceResult r = inferPoint(e, hasStart_ ? std::optional<Vec3>(start_) : std::nullopt);
    if (!hasStart_) {
        setBase(r);
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
    angleStep_ = hasStart_ && baseDir_ && e.shift() && !lockDir_;
    InferenceResult r = inferPoint(e, hasStart_ ? std::optional<Vec3>(start_) : std::nullopt);
    if (hasStart_) setVcb(fmtLen((r.p - start_).length()));
    update();
}

void LineTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (dragging_ && hasStart_ && std::hypot(e.x - pressX_, e.y - pressY_) > 6) {
        angleStep_ = baseDir_ && e.shift() && !lockDir_;
        InferenceResult r = inferPoint(e, start_);
        commit(start_, r.p);
    }
    dragging_ = false;
}

void LineTool::mouseDoubleClick(const MouseEvent&) {
    base_.reset();
    baseDir_.reset();
    squareDirs_.clear();
    hasStart_ = false;
    clearLocks();
    refreshStatus();
    update();
}

void LineTool::commit(Vec3 a, Vec3 b) { // by value: callers pass start_, reassigned below
    dragging_ = false;
    if (a.nearlyEquals(b)) return;
    int facesBefore = mesh().aliveFaceCount();
    doc().beginOp("Line");
    // Drawn over existing edges in line with it, the new line replaces them.
    mesh().overwriteLine(mesh().drawLine(a, b));
    doc().commitOp();
    bool closed = mesh().aliveFaceCount() > facesBefore;
    clearLocks();
    if (closed) {
        hasStart_ = false;
        base_.reset();
        baseDir_.reset();
        squareDirs_.clear();
    } else {
        start_ = b;
        hasStart_ = true;
        // The next segment measures its angles from this one, and is square
        // to it or to any other edge already at its end.
        base_ = std::make_pair(a, b);
        baseDir_ = (b - a).normalized();
        squareDirs_ = squareDirections(mesh(), edgesAt(mesh().findVertex(b)), baseDir_);
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
            base_.reset();
            baseDir_.reset();
            squareDirs_.clear();
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
        const Vec3 d = inf_.p - start_;
        // Square to the edge it starts from (a forced X/Y/Z keeps its colour).
        const bool square = forcedAxis() < 0 && !hasLock() &&
                            ((baseDir_ && d.length() > kTol && std::fabs(d.normalized().dot(*baseDir_)) < 1e-6) ||
                             (inf_.type == InferenceResult::Perpendicular && inf_.square));
        Color c = hasLock() ? lockColor() : square ? InferenceEngine::squareColor() : lineColor(start_, inf_.p);
        pv.line(start_, inf_.p, c, hasLock() || square ? 2.5f : 1.6f);
        // The edge the angle is taken from.
        if (base_ && (square || inf_.type == InferenceResult::Angle))
            pv.line(base_->first, base_->second, square ? InferenceEngine::squareColor() : Color::rgb8(220, 0, 220), 2.0f, Dash::Dashed);
        if (square && inf_.type != InferenceResult::Angle && inf_.label().empty()) pv.labels.push_back({inf_.p, "Perpendicular"});
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

void PlanarShapeTool::holdAxis(int axis) {
    if (axis >= 0 && heldAxis_ < 0) heldSaved_ = forcedAxis_;
    forcedAxis_ = axis >= 0 ? axis : heldSaved_;
    heldAxis_ = axis;
    mouseMove(lastMouse_);
    update();
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
    return Plane(viewPlaneNormal(), p0);
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

std::vector<InferenceEngine::SurfaceAt> PlanarShapeTool::surfacesOf(const InferenceResult& r, const MouseEvent& e) {
    if (r.type == InferenceResult::None) return {};
    return engine().surfacesAt(r.p, e.x, e.y);
}

void PlanarShapeTool::beginSurface(const InferenceResult& r, const MouseEvent& e) {
    startSurfaces_ = surfacesOf(r, e);
    lastSurface_.reset();
}

int PlanarShapeTool::chooseSurface(const std::vector<InferenceEngine::SurfaceAt>& all, const MouseEvent& e) {
    // A face seen almost edge-on (as a wall from above) is not drawn on: you
    // look past it, at the ground or an axis plane.
    const Vec3 ray = cam().ray(e.x, e.y).dir.normalized();
    std::vector<int> idx;
    for (size_t i = 0; i < all.size(); ++i)
        if (std::fabs(all[i].plane.n.dot(ray)) >= kMinFaceView) idx.push_back((int)i);
    std::vector<InferenceEngine::SurfaceAt> c;
    for (int i : idx) c.push_back(all[i]);
    auto pick = [&](int i) { return i < 0 ? -1 : idx[i]; };
    if (c.empty()) return -1;
    if (c.size() == 1) return pick(0);
    const double tol = kTol * 100;
    Vec3 pt, n;
    if (engine().faceUnder(e.x, e.y, nullptr, pt, n)) {
        const Plane under(n, pt);
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i].plane.coincident(under, tol)) return pick((int)i);
    }
    if (lastSurface_)
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i].plane.coincident(*lastSurface_, tol)) return pick((int)i);
    return -1; // none of them under the cursor yet: the usual axis planes
}

PlanarShapeTool::Surface PlanarShapeTool::currentSurface(const MouseEvent& e,
                                                         const std::vector<InferenceEngine::SurfaceAt>& hover,
                                                         const Vec3& p0, const std::optional<Vec3>& p1) {
    Surface out;
    const double tol = std::max(kTol * 10, cam().pixelSize(p0) * 0.05);
    auto holds = [&](const Plane& P) { return P.contains(p0, tol) && (!p1 || P.contains(*p1, tol)); };
    std::vector<InferenceEngine::SurfaceAt> cands;
    for (const auto& s : startSurfaces_)
        if (holds(s.plane)) cands.push_back(s);
    int i = chooseSurface(cands, e);
    const bool fromStart = i >= 0;
    if (!fromStart) {
        // Nothing usable at the start (or only faces seen edge-on): the face
        // of what the cursor is on, if it holds the start.
        cands.clear();
        for (const auto& s : hover)
            if (holds(s.plane)) cands.push_back(s);
        i = chooseSurface(cands, e);
    }
    if (i < 0) return out;
    const InferenceEngine::SurfaceAt& c = cands[i];
    lastSurface_ = c.plane;
    out.ok = true;
    out.plane = c.plane;
    // Line up with the edge the shape starts from, else the edge of this face
    // the cursor is on, else the face's edge nearest the start.
    if (fromStart && c.onEdge) {
        out.hasDir = true;
        out.dir = c.edgeDir;
        return out;
    }
    for (const auto& h : hover)
        if (h.onEdge && h.plane.coincident(c.plane, tol)) {
            out.hasDir = true;
            out.dir = h.edgeDir;
            return out;
        }
    if (c.onEdge) {
        out.hasDir = true;
        out.dir = c.edgeDir;
    } else if (c.hasNearest) {
        out.hasDir = true;
        out.dir = c.nearestDir;
    }
    return out;
}

void PlanarShapeTool::surfaceBasis(const Surface& s, Vec3& u, Vec3& v) const {
    const Vec3 n = s.plane.n;
    if (s.hasDir) {
        Vec3 d = s.dir - n * s.dir.dot(n);
        if (d.length() > 1e-9) {
            u = d.normalized();
            v = n.cross(u).normalized();
            // Width runs along the more level of the two sides.
            if (std::fabs(v.z) < std::fabs(u.z) - 1e-9) {
                u = v;
                v = n.cross(u).normalized();
            }
            return;
        }
    }
    shapeBasis(n, u, v);
}

// ===========================================================================
// Rectangle
// ===========================================================================

QString RectangleTool::status() const {
    if (!hasStart_) return QStringLiteral("Select first corner. Drag up along blue for a vertical plane; arrow keys force a plane (Right=red, Left=green, Up=blue).");
    return QStringLiteral("Select opposite corner or enter dimensions (width,height).");
}

void RectangleTool::rememberEdge(const InferenceResult& r) {
    if (!r.hasEdge || (r.edgeB - r.edgeA).length() < kTol * 10) return;
    for (size_t i = 0; i < hoveredEdges_.size(); ++i) {
        const LenRef& o = hoveredEdges_[i];
        if ((o.a.nearlyEquals(r.edgeA) && o.b.nearlyEquals(r.edgeB)) || (o.a.nearlyEquals(r.edgeB) && o.b.nearlyEquals(r.edgeA))) {
            hoveredEdges_.erase(hoveredEdges_.begin() + i);
            break;
        }
    }
    hoveredEdges_.push_back({r.edgeA, r.edgeB});
    if (hoveredEdges_.size() > 8) hoveredEdges_.erase(hoveredEdges_.begin());
}

bool RectangleTool::snapToEdgeLength(double& len, double tol, std::optional<LenRef>& ref) const {
    const double a = std::fabs(len);
    double best = tol;
    auto consider = [&](const std::vector<LenRef>& refs) {
        // Most recently hovered first: on a tie it wins.
        for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
            const double l = (it->b - it->a).length();
            if (std::fabs(l - a) < best) {
                best = std::fabs(l - a);
                ref = *it;
            }
        }
    };
    consider(faceEdges_);
    consider(hoveredEdges_);
    if (!ref) return false;
    len = (len < 0 ? -1 : 1) * (ref->b - ref->a).length();
    return true;
}

void RectangleTool::reset() {
    hasStart_ = dragging_ = false;
    w_ = h_ = 0;
    hint_.clear();
    hoveredEdges_.clear();
    faceEdges_.clear();
    refW_.reset();
    refH_.reset();
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
        beginSurface(r, e);
        faceEdges_.clear();
        for (const auto& sf : startSurfaces_)
            if (sf.outline.size() <= 32) // not the many small sides of a circle or terrain
                for (const auto& [a, b] : sf.outline) faceEdges_.push_back({a, b});
        refW_.reset();
        refH_.reset();
        hasStart_ = true;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        plane_ = Plane(viewPlaneNormal(), p0_);
        autoAxis_ = -1;
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
        rememberEdge(inferPoint(e, std::nullopt));
        update();
        return;
    }
    // On a face the shape follows the face (and its edges); X/Y/Z or the
    // arrow keys still force an axis plane.
    Surface surf;
    if (forcedAxis_ < 0) {
        snapDistance_ = false;
        const InferenceResult hover = inferPoint(e, p0_);
        snapDistance_ = true;
        rememberEdge(hover);
        surf = currentSurface(e, surfacesOf(hover, e), p0_);
    }
    if (surf.ok) {
        plane_ = surf.plane;
        surfaceBasis(surf, u_, v_);
    } else {
        int axis = forcedAxis_ < 0 ? cursorAxis(e) : -1;
        plane_ = axis >= 0 ? Plane(kAxes[axis], p0_) : resolvePlane(e, p0_, startFace_);
        shapeBasis(plane_.n, u_, v_);
    }
    snapDistance_ = false; // each side snaps on its own
    InferenceResult r = inferPoint(e, p0_, plane_);
    snapDistance_ = true;
    rememberEdge(r);
    Vec3 d = r.p - p0_;
    w_ = d.dot(u_);
    h_ = d.dot(v_);
    if (lengthSnappable(r)) {
        w_ = snapLen(w_);
        h_ = snapLen(h_);
    }
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
    // A side as long as an existing edge (hovered lately or of the face started on).
    refW_.reset();
    refH_.reset();
    if (hint_.empty() && lengthSnappable(r)) {
        snapToEdgeLength(w_, px * 6, refW_);
        snapToEdgeLength(h_, px * 6, refH_);
    }
    setVcb(fmtLen(std::fabs(w_)) + ", " + fmtLen(std::fabs(h_)));
    update();
}

int RectangleTool::cursorAxis(const MouseEvent& e) {
    double sx, sy;
    if (cam().project(p0_, sx, sy) && std::hypot(sx - e.x, sy - e.y) < 8) autoAxis_ = -1;
    InferenceResult r = inferPoint(e, p0_);
    Vec3 d = r.p - p0_;
    double eps = kTol * 10;
    // Moving straight up from the start: switch to the vertical plane facing the camera.
    if (r.type == InferenceResult::AxisZ && std::fabs(d.z) > eps) {
        Vec3 f = cam().forward();
        autoAxis_ = std::fabs(f.x) > std::fabs(f.y) ? 0 : 1;
    }
    // Snapped to geometry: use the axis plane through both points, if there is exactly one.
    bool onGeometry = r.isSnap() || r.type == InferenceResult::OnEdge || r.type == InferenceResult::OnIntersection ||
                      r.type == InferenceResult::OnFace ||
                      r.type == InferenceResult::OnGuide;
    if (onGeometry) {
        int zero = -1, n = 0;
        for (int i = 0; i < 3; ++i)
            if (std::fabs(d[i]) < eps) { zero = i; ++n; }
        if (n == 1) return zero;
    }
    return autoAxis_;
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
    hoveredEdges_.clear();
    faceEdges_.clear();
    refW_.reset();
    refH_.reset();
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
        // Sides snapped to an edge's length: that edge and the side in magenta.
        const Color mag = Color::rgb8(220, 0, 220);
        if (refW_) {
            pv.line(refW_->a, refW_->b, mag, 3.0f);
            pv.line(c[0], c[1], mag, 2.4f);
            pv.labels.push_back({(c[0] + c[1]) * 0.5, "Edge Length"});
        }
        if (refH_) {
            pv.line(refH_->a, refH_->b, mag, 3.0f);
            pv.line(c[1], c[2], mag, 2.4f);
            pv.labels.push_back({(c[1] + c[2]) * 0.5, "Edge Length"});
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
        beginSurface(r, e);
        hasCenter_ = true;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        if (forcedAxis_ >= 0) plane_ = Plane(kAxes[forcedAxis_], center_);
        else if (Surface s = currentSurface(e, {}, center_); s.ok) plane_ = s.plane;
        else plane_ = resolvePlane(e, center_, startFace_);
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
        if (forcedAxis_ >= 0) {
            plane_ = Plane(kAxes[forcedAxis_], r.p);
        } else {
            // Show the plane of the face (or the faces around the edge) under the cursor.
            auto c = surfacesOf(r, e);
            lastSurface_.reset();
            const int i = chooseSurface(c, e);
            plane_ = i >= 0 ? Plane(c[i].plane.n, r.p) : r.face != kNone ? mesh().facePlane(r.face) : Plane(viewPlaneNormal(), r.p);
        }
        update();
        return;
    }
    if (forcedAxis_ >= 0) {
        plane_ = Plane(kAxes[forcedAxis_], center_);
    } else {
        const InferenceResult hover = inferPoint(e, center_);
        if (Surface s = currentSurface(e, surfacesOf(hover, e), center_); s.ok) plane_ = s.plane;
        else plane_ = resolvePlane(e, center_, kNone);
    }
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
    // A face holding both ends (the start's faces first, then the end's).
    if (Surface s = currentSurface(e, endSurfaces_, a_, b_); s.ok) return s.plane;
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
        beginSurface(r, e);
        endSurfaces_.clear();
        stage_ = 1;
    } else if (stage_ == 1) {
        InferenceResult r = inferPoint(e, a_);
        if (r.p.nearlyEquals(a_)) return;
        b_ = r.p;
        endSurfaces_ = surfacesOf(r, e);
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
        endSurfaces_.clear();
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

// ===========================================================================
// 3D Curve
// ===========================================================================

namespace {

// Centripetal Catmull-Rom between p1 (u = 0) and p2 (u = 1): no loops or
// cusps, however unevenly the anchors are spaced.
Vec3 catmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, double u) {
    auto knot = [](double t, const Vec3& a, const Vec3& b) { return t + std::max(std::sqrt((b - a).length()), 1e-6); };
    const double t0 = 0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const Vec3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const Vec3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const Vec3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const Vec3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const Vec3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

const Color kCurveColor = Color::rgb8(40, 40, 40);
const Color kHullColor = Color::rgb8(130, 130, 130);

} // namespace

QString Curve3DTool::status() const {
    switch (stage_) {
    case 0: return QStringLiteral("Click the start point of the line to bend. Type a number for the anchor points (now %1), e.g. 32s for segments.").arg(anchorCount_);
    case 1: return QStringLiteral("Click the end point or enter a length.");
    default:
        if (active_ >= 0)
            return QStringLiteral("Click where the anchor goes, or type a distance or <x,y,z>. Arrow keys or X/Y/Z lock an axis, Shift the inference. Esc puts it back.");
        return QStringLiteral("Drag an anchor (or click it, move, click) to bend the curve in 3D. Click the curve to add an anchor, "
                              "Ctrl+click one to remove it. Enter or double-click to finish, Esc to cancel.");
    }
}

QString Curve3DTool::vcbLabel() const {
    if (stage_ == 1) return QStringLiteral("Length");
    if (stage_ == 2 && active_ >= 0) return QStringLiteral("Distance");
    return QStringLiteral("Anchors");
}

void Curve3DTool::reset() {
    if (stage_ == 2 && active_ >= 0) { // Esc while moving an anchor puts it back
        anchors_[active_] = orig_;
        active_ = -1;
        dragging_ = false;
        clearLocks();
        refreshStatus();
        update();
        return;
    }
    stage_ = 0;
    anchors_.clear();
    active_ = hot_ = last_ = -1;
    dragging_ = false;
    clearLocks();
    setVcb(QString::number(anchorCount_));
    refreshStatus();
    update();
}

void Curve3DTool::deactivate() {
    if (stage_ == 2) {
        if (active_ >= 0) anchors_[active_] = orig_;
        active_ = -1;
        commit();
    }
    reset();
}

std::vector<Vec3> Curve3DTool::curvePoints(std::vector<int>* span, int segments) const {
    std::vector<Vec3> pts;
    if (span) span->clear();
    const int n = (int)anchors_.size();
    if (n < 2) return pts;
    if (segments <= 0) segments = segments_;
    double total = 0;
    for (int i = 0; i + 1 < n; ++i) total += (anchors_[i + 1] - anchors_[i]).length();
    pts.push_back(anchors_[0]);
    if (span) span->push_back(0);
    for (int i = 0; i + 1 < n; ++i) {
        const Vec3& p1 = anchors_[i];
        const Vec3& p2 = anchors_[i + 1];
        // Mirrored neighbours at the open ends.
        const Vec3 p0 = i > 0 ? anchors_[i - 1] : p1 * 2.0 - p2;
        const Vec3 p3 = i + 2 < n ? anchors_[i + 2] : p2 * 2.0 - p1;
        const double len = (p2 - p1).length();
        const int k = total > kTol ? std::max(1, (int)std::lround(segments * len / total)) : 1;
        for (int j = 1; j <= k; ++j) {
            const Vec3 q = j == k ? p2 : catmullRom(p0, p1, p2, p3, (double)j / k);
            if (q.nearlyEquals(pts.back())) continue;
            pts.push_back(q);
            if (span) span->push_back(i);
        }
    }
    return pts;
}

void Curve3DTool::finishLine(const Vec3& end) {
    if (end.nearlyEquals(start_)) return;
    anchors_.clear();
    const int n = std::max(2, anchorCount_);
    for (int i = 0; i < n; ++i) anchors_.push_back(lerp(start_, end, (double)i / (n - 1)));
    stage_ = 2;
    active_ = hot_ = last_ = -1;
    dragging_ = false;
    clearLocks();
    setVcb(QString::number(n));
    refreshStatus();
    update();
}

void Curve3DTool::commit() {
    const std::vector<Vec3> pts = curvePoints();
    if (pts.size() >= 2) {
        doc().beginOp("3D Curve");
        mesh().drawPolyline(pts, false, pts.size() > 2 ? mesh().newCurveId() : kNone);
        doc().commitOp();
    }
    stage_ = 0;
    anchors_.clear();
    active_ = hot_ = last_ = -1;
    dragging_ = false;
    clearLocks();
    setVcb(QString::number(anchorCount_));
    refreshStatus();
    update();
}

void Curve3DTool::setAnchorCount(int n) {
    n = std::clamp(n, 2, 64);
    anchorCount_ = n;
    if (stage_ != 2) return;
    // The same shape, resampled evenly along its length.
    const std::vector<Vec3> dense = curvePoints(nullptr, 64 * (int)anchors_.size());
    std::vector<double> at(dense.size(), 0.0);
    for (size_t i = 1; i < dense.size(); ++i) at[i] = at[i - 1] + (dense[i] - dense[i - 1]).length();
    const double total = at.back();
    std::vector<Vec3> out{dense.front()};
    size_t j = 1;
    for (int i = 1; i + 1 < n; ++i) {
        const double s = total * i / (n - 1);
        while (j + 1 < dense.size() && at[j] < s) ++j;
        const double seg = at[j] - at[j - 1];
        out.push_back(seg > 1e-12 ? lerp(dense[j - 1], dense[j], (s - at[j - 1]) / seg) : dense[j]);
    }
    out.push_back(dense.back());
    anchors_ = out;
    active_ = hot_ = last_ = -1;
    update();
}

int Curve3DTool::anchorAt(double x, double y) const {
    int best = -1;
    double bestD = 10.0;
    for (int i = 0; i < (int)anchors_.size(); ++i) {
        double sx, sy;
        if (!vp_->camera().project(anchors_[i], sx, sy)) continue;
        const double d = std::hypot(sx - x, sy - y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

bool Curve3DTool::curveAt(double x, double y, Vec3& p, int& insertAt) const {
    std::vector<int> span;
    const std::vector<Vec3> pts = curvePoints(&span, 32 * (int)anchors_.size());
    double bestD = 6.0;
    bool hit = false;
    for (size_t i = 1; i < pts.size(); ++i) {
        double ax, ay, bx, by;
        if (!vp_->camera().project(pts[i - 1], ax, ay) || !vp_->camera().project(pts[i], bx, by)) continue;
        const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        const double t = l2 > 1e-12 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(ax + dx * t - x, ay + dy * t - y);
        if (d < bestD) {
            bestD = d;
            p = lerp(pts[i - 1], pts[i], t);
            insertAt = span[i] + 1;
            hit = true;
        }
    }
    return hit;
}

void Curve3DTool::pick(int anchor, const MouseEvent& e) {
    active_ = last_ = hot_ = anchor;
    orig_ = anchors_[anchor];
    dragging_ = true;
    pressX_ = e.x;
    pressY_ = e.y;
    travelled_ = false;
    clearLocks();
    setVcb(QString());
    refreshStatus();
}

void Curve3DTool::placeAnchor(const MouseEvent& e) {
    if (active_ < 0) return;
    // From where the anchor was: the axes, geometry and the locks infer.
    const InferenceResult r = inferPoint(e, orig_);
    anchors_[active_] = r.p;
    setVcb(fmtLen((r.p - orig_).length()));
}

void Curve3DTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (stage_ == 0) {
        start_ = inferPoint(e, std::nullopt).p;
        stage_ = 1;
        dragging_ = true;
        pressX_ = e.x;
        pressY_ = e.y;
        travelled_ = false;
        refreshStatus();
    } else if (stage_ == 1) {
        finishLine(inferPoint(e, start_).p);
    } else if (active_ >= 0) { // click-move-click: the second click places it
        placeAnchor(e);
        active_ = -1;
        clearLocks();
        refreshStatus();
    } else if (const int a = anchorAt(e.x, e.y); a >= 0) {
        if (e.ctrl()) {
            if (anchors_.size() > 2) {
                anchors_.erase(anchors_.begin() + a);
                anchorCount_ = (int)anchors_.size();
                hot_ = last_ = -1;
            }
        } else {
            pick(a, e);
        }
    } else {
        Vec3 p;
        int at = 0;
        if (curveAt(e.x, e.y, p, at)) { // a new anchor where the curve is clicked
            anchors_.insert(anchors_.begin() + at, p);
            anchorCount_ = (int)anchors_.size();
            pick(at, e);
        } else {
            commit(); // a click away from the curve finishes it
        }
    }
    update();
}

void Curve3DTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (dragging_) travelled_ = travelled_ || std::hypot(e.x - pressX_, e.y - pressY_) > 6;
    if (stage_ == 0) {
        inferPoint(e, std::nullopt);
    } else if (stage_ == 1) {
        const InferenceResult r = inferPoint(e, start_);
        setVcb(fmtLen((r.p - start_).length()));
    } else if (active_ >= 0) {
        placeAnchor(e);
    } else {
        hot_ = anchorAt(e.x, e.y);
    }
    update();
}

void Curve3DTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    travelled_ = travelled_ || std::hypot(e.x - pressX_, e.y - pressY_) > 6;
    if (dragging_ && travelled_) {
        if (stage_ == 1) {
            finishLine(inferPoint(e, start_).p);
        } else if (stage_ == 2 && active_ >= 0) {
            placeAnchor(e);
            active_ = -1;
            clearLocks();
            refreshStatus();
        }
    }
    // Released where it was pressed: the anchor follows the mouse until the next click.
    dragging_ = false;
    update();
}

void Curve3DTool::mouseDoubleClick(const MouseEvent&) {
    if (stage_ != 2) return;
    if (active_ >= 0) anchors_[active_] = orig_;
    active_ = -1;
    commit();
}

bool Curve3DTool::keyPress(QKeyEvent* e) {
    if (stage_ == 2 && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        if (active_ >= 0) anchors_[active_] = orig_;
        active_ = -1;
        commit();
        return true;
    }
    const bool r = Tool::keyPress(e);
    if (active_ >= 0) placeAnchor(lastMouse_); // an axis lock applies at once
    return r;
}

bool Curve3DTool::vcbEnter(const QString& text) {
    int s;
    if (parseSides(text, s)) {
        segments_ = std::clamp(s, 2, 2000);
        update();
        return true;
    }
    bool isInt = false;
    const int n = text.trimmed().toInt(&isInt);
    if (stage_ == 0) {
        if (!isInt || n < 2 || n > 64) return false;
        setAnchorCount(n);
        setVcb(QString::number(anchorCount_));
        refreshStatus();
        return true;
    }
    Vec3 c;
    bool rel = false;
    if (stage_ == 1) {
        if (parseCoords(text, doc().units, c, rel)) {
            finishLine(rel ? start_ + c : c);
            return true;
        }
        double len;
        if (!parseLen(text, len) || std::fabs(len) < kTol) return false;
        Vec3 dir = inf_.p - start_;
        if (dir.length() < kTol) dir = lockDir_ ? *lockDir_ : Vec3(1, 0, 0);
        finishLine(start_ + dir.normalized() * len);
        return true;
    }
    // Moving anchors: coordinates for the anchor being moved (or moved last),
    // a distance along the way it is moving, or else a new anchor count.
    const int idx = active_ >= 0 ? active_ : last_;
    if (idx >= 0 && idx < (int)anchors_.size() && parseCoords(text, doc().units, c, rel)) {
        anchors_[idx] = rel ? (active_ >= 0 ? orig_ : anchors_[idx]) + c : c;
        active_ = -1;
        last_ = idx;
        clearLocks();
        refreshStatus();
        update();
        return true;
    }
    if (active_ >= 0) {
        double len;
        const Vec3 dir = anchors_[active_] - orig_;
        if (!parseLen(text, len) || dir.length() < kTol) return false;
        anchors_[active_] = orig_ + dir.normalized() * len;
        active_ = -1;
        clearLocks();
        refreshStatus();
        update();
        return true;
    }
    if (isInt && n >= 2 && n <= 64) {
        setAnchorCount(n);
        setVcb(QString::number(anchorCount_));
        return true;
    }
    return false;
}

void Curve3DTool::buildPreview(Preview& pv) {
    using MK = Preview::MarkerKind;
    if (stage_ == 1) {
        pv.line(start_, inf_.p, hasLock() ? lockColor() : lineColor(start_, inf_.p), hasLock() ? 2.5f : 1.6f);
        // Where the anchors will be.
        const int n = std::max(2, anchorCount_);
        for (int i = 0; i < n; ++i) pv.markers.push_back({lerp(start_, inf_.p, (double)i / (n - 1)), MK::Grip});
    } else if (stage_ == 2) {
        for (size_t i = 1; i < anchors_.size(); ++i) pv.line(anchors_[i - 1], anchors_[i], kHullColor, 1.0f, Dash::Dashed);
        const std::vector<Vec3> pts = curvePoints();
        for (size_t i = 1; i < pts.size(); ++i) pv.line(pts[i - 1], pts[i], kCurveColor, 2.2f);
        if (active_ >= 0) {
            const Vec3& a = anchors_[active_];
            // How far it moved, in the colour of the axis it follows.
            if (!a.nearlyEquals(orig_)) pv.line(orig_, a, hasLock() ? lockColor() : lineColor(orig_, a), 2.0f, Dash::Dashed);
            // Its height over the level it started from.
            const Vec3 foot(a.x, a.y, orig_.z);
            if (std::fabs(a.z - orig_.z) > kTol && !foot.nearlyEquals(orig_)) pv.line(a, foot, kHullColor, 1.0f, Dash::Dotted);
        }
        for (int i = 0; i < (int)anchors_.size(); ++i) pv.markers.push_back({anchors_[i], i == active_ || i == hot_ ? MK::GripHot : MK::Grip});
    }
    if (stage_ < 2 || active_ >= 0) engine().addPreview(inf_, pv);
}

} // namespace forma
