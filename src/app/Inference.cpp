// SPDX-License-Identifier: GPL-3.0-or-later
#include "Inference.h"

#include "core/Components.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gs {

static const Vec3 kAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};

std::string InferenceResult::label() const {
    switch (type) {
    case Endpoint: return "Endpoint";
    case Midpoint: return "Midpoint";
    case OnEdge: return "On Edge";
    case OnFace: return "On Face";
    case Origin: return "Origin";
    case AxisX: return "On Red Axis";
    case AxisY: return "On Green Axis";
    case AxisZ: return "On Blue Axis";
    case Locked: return "Locked";
    case FromPoint: return "From Point";
    case OnGuide: return "On Guide";
    case GuidePoint: return "Guide Point";
    case Intersection: return "Intersection";
    case AlongEdge: return "Along Edge";
    case Parallel: return "Parallel to Edge";
    case Perpendicular: return "Perpendicular to Edge";
    case FaceNormal: return "Perpendicular to Face";
    case OnIntersection: return "On Intersection";
    case Angle: return std::to_string((int)std::lround(angleDeg)) + "\u00b0";
    default: return "";
    }
}

double InferenceEngine::pixelDistance(const Vec3& p, double px, double py) const {
    double sx, sy;
    if (!cam_.project(p, sx, sy)) return 1e300;
    return std::hypot(sx - px, sy - py);
}

const InferenceEngine::MeshBoxes& InferenceEngine::boxesOf(const Mesh& m) const {
    if (boxes_.size() > 4096) boxes_.clear(); // meshes come and go (undo, deleted components)
    MeshBoxes& b = boxes_[&m];
    if (b.revision == m.revision && b.faces.size() == m.faces.size()) return b;
    b.revision = m.revision;
    b.any = false;
    b.faces.assign(m.faces.size(), Box());
    const double big = std::numeric_limits<double>::infinity();
    b.all = {Vec3(big, big, big), Vec3(-big, -big, -big)};
    auto grow = [](Box& x, const Vec3& p) {
        x.mn = Vec3(std::min(x.mn.x, p.x), std::min(x.mn.y, p.y), std::min(x.mn.z, p.z));
        x.mx = Vec3(std::max(x.mx.x, p.x), std::max(x.mx.y, p.y), std::max(x.mx.z, p.z));
    };
    for (const Vertex& v : m.vertices)
        if (v.alive) {
            grow(b.all, v.p);
            b.any = true;
        }
    if (!b.any) b.all = {Vec3(), Vec3()};
    // A little slack: flat faces have no thickness, and kTol counts as touching.
    const double pad = std::max(kTol, 1e-7 * (b.all.mx - b.all.mn).length());
    const Vec3 slack(pad, pad, pad);
    b.all.mn = b.all.mn - slack;
    b.all.mx = b.all.mx + slack;
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive) continue;
        Box& fb = b.faces[f];
        fb = {Vec3(big, big, big), Vec3(-big, -big, -big)};
        for (Id v : F.outer) grow(fb, m.pos(v)); // holes lie inside the outline
        if (F.outer.empty()) fb = {Vec3(), Vec3()};
        fb.mn = fb.mn - slack;
        fb.mx = fb.mx + slack;
    }
    return b;
}

bool InferenceEngine::boxNearCursor(const Box& b, const Mat4& xf, double px, double py, double tolPx) const {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (int i = 0; i < 8; ++i) {
        const Vec3 c((i & 1) ? b.mx.x : b.mn.x, (i & 2) ? b.mx.y : b.mn.y, (i & 4) ? b.mx.z : b.mn.z);
        double sx, sy;
        if (!cam_.project(xf.transformPoint(c), sx, sy)) return true;
        x0 = std::min(x0, sx);
        x1 = std::max(x1, sx);
        y0 = std::min(y0, sy);
        y1 = std::max(y1, sy);
    }
    return px >= x0 - tolPx && px <= x1 + tolPx && py >= y0 - tolPx && py <= y1 + tolPx;
}

bool InferenceEngine::rayHitsBox(const Box& b, const Ray& r) {
    double t0 = -std::numeric_limits<double>::infinity(), t1 = std::numeric_limits<double>::infinity();
    for (int a = 0; a < 3; ++a) {
        const double o = r.origin[a], d = r.dir[a], lo = b.mn[a], hi = b.mx[a];
        if (std::fabs(d) < 1e-300) {
            if (o < lo || o > hi) return false;
            continue;
        }
        double ta = (lo - o) / d, tb = (hi - o) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true; // a parallel camera's ray runs both ways: t < 0 counts too
}

bool InferenceEngine::boxContains(const Box& b, const Mat4& xf, const Vec3& p, double tol) const {
    const Vec3 q = xf.inverted().transformPoint(p);
    // tol is in world units; a scaled placement shrinks or stretches it.
    const double s = std::max({xf.transformDir(Vec3(1, 0, 0)).length(), xf.transformDir(Vec3(0, 1, 0)).length(),
                               xf.transformDir(Vec3(0, 0, 1)).length(), 1e-12});
    const double t = tol / std::min(s, 1.0) + tol;
    for (int a = 0; a < 3; ++a)
        if (q[a] < b.mn[a] - t || q[a] > b.mx[a] + t) return false;
    return true;
}

Id InferenceEngine::pickFace(double px, double py, double* tOut, const std::set<Id>* ex) const {
    const Mesh& m = doc_.mesh();
    Ray r = cam_.ray(px, py);
    Id best = kNone;
    double bestT = std::numeric_limits<double>::infinity();
    if (tOut) *tOut = bestT;
    if (doc_.style.faceStyle == RenderStyle::Wireframe) return kNone;
    const MeshBoxes& boxes = boxesOf(m);
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden || !rayHitsBox(boxes.faces[f], r)) continue;
        if (ex) {
            bool skip = false;
            for (Id v : F.outer)
                if (ex->count(v)) { skip = true; break; }
            if (skip) continue;
        }
        double t;
        if (doc_.style.backfaceCulling && !doc_.style.xray && F.normal.dot(r.dir) >= 0) continue; // not drawn
        if (m.rayFace(f, r, t) && t < bestT) {
            bestT = t;
            best = f;
        }
    }
    // Components in front hide the loose geometry behind them.
    double tInst;
    if (pickInstance(px, py, &tInst) != kNone && tInst < bestT - 1e-9) {
        bestT = tInst;
        best = kNone;
    }
    if (tOut) *tOut = bestT;
    return best;
}

Id InferenceEngine::pickInstance(double px, double py, double* tOut, Vec3* normalOut) const {
    const ModelState& st = doc_.state;
    Id best = kNone;
    double bestT = std::numeric_limits<double>::infinity();
    if (normalOut) *normalOut = Vec3();
    Ray r = cam_.ray(px, py);
    const bool wire = doc_.style.faceStyle == RenderStyle::Wireframe;
    // Everything inside a component counts as that component.
    for (const PlacedInstance& placed : placedInstances(st)) {
        if (!placed.active || placed.root == kNone || placed.inst == st.editing || ignoredInstances.count(placed.root)) continue;
        const Id i = placed.root;
        const Mesh& m = st.defs[placed.def].mesh;
        const Mat4& xf = placed.xf;
        const MeshBoxes& boxes = boxesOf(m);
        if (!boxes.any || !boxNearCursor(boxes.all, xf, px, py, 6.0)) continue; // nothing of it near the cursor
        const Mat4 inv = xf.inverted();
        // Ray in definition coordinates; same parameter t as in world space.
        Ray lr{inv.transformPoint(r.origin), inv.transformDir(r.dir)};
        if (!wire)
            for (Id f = 0; f < (Id)m.faces.size(); ++f) {
                if (!m.faces[f].alive || m.faces[f].hidden || !rayHitsBox(boxes.faces[f], lr)) continue;
                double t;
                if (doc_.style.backfaceCulling && !doc_.style.xray &&
                    transformCovector(xf, m.faces[f].normal).dot(r.dir) >= 0) continue; // not drawn
                if (m.rayFace(f, lr, t) && t < bestT) {
                    bestT = t;
                    best = i;
                    if (normalOut) *normalOut = transformCovector(xf, m.faces[f].normal).normalized();
                }
            }
        // Edges near the cursor (lines and wireframe have no faces to hit).
        for (const Edge& E : m.edges) {
            if (!E.alive || E.hidden || E.soft) continue;
            Vec3 a = xf.transformPoint(m.pos(E.v[0])), b = xf.transformPoint(m.pos(E.v[1]));
            double s, t;
            Vec3 q = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
            if (pixelDistance(q, px, py) > 5.0) continue;
            double depth = (q - r.origin).dot(r.dir);
            if (cam_.perspective && depth < 0) continue;
            if (depth < bestT) {
                bestT = depth;
                best = i;
                if (normalOut) *normalOut = Vec3();
            }
        }
    }
    if (tOut) *tOut = bestT;
    return best;
}

bool InferenceEngine::snapsTo(const PlacedInstance& p) const {
    // The open component is the active mesh itself.
    return p.inst != doc_.state.editing && (p.root == kNone || !ignoredInstances.count(p.root));
}

std::vector<PlacedInstance> InferenceEngine::snapSources() const {
    std::vector<PlacedInstance> out = placedInstances(doc_.state);
    if (doc_.state.editing != kNone) {
        PlacedInstance outer; // the model's loose geometry, already in world coordinates
        outer.inst = kNone;
        outer.def = kNone;
        outer.active = false;
        out.push_back(outer);
    }
    return out;
}

const std::vector<InferenceEngine::Segment>& InferenceEngine::instanceEdges(int index, const PlacedInstance& p) const {
    const Mesh& m = sourceMesh(p);
    CachedEdges& c = instEdges_[index];
    if (c.revision != m.revision || !std::equal(std::begin(c.xf.m), std::end(c.xf.m), std::begin(p.xf.m))) {
        c.revision = m.revision;
        c.xf = p.xf;
        c.edges.clear();
        for (const Edge& E : m.edges)
            if (E.alive && !E.soft && !E.hidden)
                c.edges.push_back({p.xf.transformPoint(m.pos(E.v[0])), p.xf.transformPoint(m.pos(E.v[1]))});
    }
    return c.edges;
}

bool InferenceEngine::pickAnyEdge(double px, double py, double tolPx, const std::set<Id>* ex, double tFace,
                                  EdgeHit& hit) const {
    const Mesh& m = doc_.mesh();
    Ray r = cam_.ray(px, py);
    auto closest = [&](const Vec3& a, const Vec3& b) {
        double s, t;
        return closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
    };
    double bestD = tolPx;
    bool found = false;
    Id e = pickEdgeT(px, py, tolPx, ex, tFace);
    if (e != kNone) {
        hit.seg = {m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1])};
        hit.edge = e;
        hit.p = closest(hit.seg.a, hit.seg.b);
        bestD = pixelDistance(hit.p, px, py);
        found = true;
    }
    const std::vector<PlacedInstance> placed = snapSources();
    for (int i = 0; i < (int)placed.size(); ++i) {
        if (!snapsTo(placed[i]) || !nearCursor(placed[i], px, py, tolPx)) continue;
        for (const Segment& s : instanceEdges(i, placed[i])) {
            Vec3 q = closest(s.a, s.b);
            double d = pixelDistance(q, px, py);
            if (d >= bestD) continue;
            double depth = (q - r.origin).dot(r.dir);
            if (!seeThrough() && depth > tFace + std::max(cam_.pixelSize(q) * 3.0, 1e-4)) continue;
            if (cam_.perspective && depth < 0) continue;
            bestD = d;
            hit.seg = s;
            hit.edge = kNone;
            hit.p = q;
            found = true;
        }
    }
    // No edge: the line where two faces cut through each other.
    if (!found && ix_ && pixelDistance(ix_->p, px, py) < tolPx) {
        hit = *ix_;
        found = true;
    }
    return found;
}

std::vector<InferenceEngine::FaceRef> InferenceEngine::worldFaces(const std::set<Id>* ex, double px, double py, double tolPx) const {
    std::vector<FaceRef> out;
    if (doc_.style.faceStyle == RenderStyle::Wireframe) return out;
    const Mesh& m = doc_.mesh();
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        if (ex) {
            bool skip = false;
            for (Id v : F.outer)
                if (ex->count(v)) { skip = true; break; }
            if (skip) continue;
        }
        FaceRef r;
        r.mesh = &m;
        r.face = f;
        r.plane = m.facePlane(f);
        r.active = true;
        out.push_back(r);
    }
    const ModelState& st = doc_.state;
    // Components, and while one is open everything around it.
    for (const PlacedInstance& placed : snapSources()) {
        if (!snapsTo(placed) || placed.inst == st.editing || !nearCursor(placed, px, py, tolPx)) continue;
        const Mesh& dm = sourceMesh(placed);
        const Mat4 inv = placed.xf.inverted();
        for (Id f = 0; f < (Id)dm.faces.size(); ++f) {
            const Face& F = dm.faces[f];
            if (!F.alive || F.hidden || F.outer.empty()) continue;
            const Vec3& c = F.normal;
            const Vec3 n(inv.at(0, 0) * c.x + inv.at(1, 0) * c.y + inv.at(2, 0) * c.z,
                         inv.at(0, 1) * c.x + inv.at(1, 1) * c.y + inv.at(2, 1) * c.z,
                         inv.at(0, 2) * c.x + inv.at(1, 2) * c.y + inv.at(2, 2) * c.z);
            if (n.length2() < 1e-24) continue;
            FaceRef r;
            r.mesh = &dm;
            r.face = f;
            r.xf = placed.xf;
            r.inv = inv;
            r.plane = Plane(n, placed.xf.transformPoint(dm.pos(F.outer[0])));
            out.push_back(r);
        }
    }
    return out;
}

bool InferenceEngine::rayHitsFace(const FaceRef& f, const Ray& r, double& t) const {
    if (doc_.style.backfaceCulling && !doc_.style.xray && f.plane.n.dot(r.dir) >= 0) return false; // not drawn
    // Same parameter t in world and definition coordinates.
    const Ray lr{f.inv.transformPoint(r.origin), f.inv.transformDir(r.dir)};
    if (!rayHitsBox(boxesOf(*f.mesh).faces[f.face], lr)) return false;
    return f.mesh->rayFace(f.face, lr, t);
}

bool InferenceEngine::faceContains(const FaceRef& f, const Vec3& p) const {
    return f.mesh->pointInFace(f.face, f.inv.transformPoint(p), true);
}

int InferenceEngine::frontFace(const std::vector<FaceRef>& faces, const Ray& r, double& tFront) const {
    int front = -1;
    tFront = std::numeric_limits<double>::infinity();
    for (int i = 0; i < (int)faces.size(); ++i) {
        double t;
        if (rayHitsFace(faces[i], r, t) && t < tFront) {
            tFront = t;
            front = i;
        }
    }
    return front;
}

Vec3 InferenceEngine::faceNormalAt(double px, double py, const std::set<Id>* ex) const {
    const std::vector<FaceRef> faces = worldFaces(ex, px, py, 1.0);
    double t;
    const int f = frontFace(faces, cam_.ray(px, py), t);
    return f >= 0 ? faces[f].plane.n : Vec3();
}

bool InferenceEngine::faceUnder(double px, double py, const std::set<Id>* ex, Vec3& point, Vec3& normal) const {
    const std::vector<FaceRef> faces = worldFaces(ex, px, py, 1.0);
    const Ray r = cam_.ray(px, py);
    double t;
    const int f = frontFace(faces, r, t);
    if (f < 0) return false;
    point = r.at(t);
    normal = faces[f].plane.n;
    return true;
}

bool InferenceEngine::outlineNearCursor(const FaceRef& F, double px, double py, double tolPx) const {
    const std::vector<Id>& L = F.mesh->faces[F.face].outer;
    std::vector<std::pair<double, double>> s(L.size());
    for (size_t i = 0; i < L.size(); ++i)
        if (!cam_.project(F.xf.transformPoint(F.mesh->pos(L[i])), s[i].first, s[i].second)) return true; // behind the camera: keep
    bool inside = false;
    const double tol2 = tolPx * tolPx;
    for (size_t i = 0, j = L.size() - 1; i < L.size(); j = i++) {
        const auto [xi, yi] = s[i];
        const auto [xj, yj] = s[j];
        if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
        // Distance from the cursor to this side.
        const double dx = xj - xi, dy = yj - yi, l2 = dx * dx + dy * dy;
        const double t = l2 > 0 ? std::clamp(((px - xi) * dx + (py - yi) * dy) / l2, 0.0, 1.0) : 0.0;
        const double ex = xi + dx * t - px, ey = yi + dy * t - py;
        if (ex * ex + ey * ey <= tol2) return true;
    }
    return inside;
}

bool InferenceEngine::pickCorner(double px, double py, double tolPx, const std::vector<FaceRef>& faces, double tFront, Vec3& out) const {
    // Only faces that reach near the cursor on screen.
    std::vector<int> near;
    for (int i = 0; i < (int)faces.size(); ++i) {
        const FaceRef& F = faces[i];
        const MeshBoxes& b = boxesOf(*F.mesh);
        if (F.face < (Id)b.faces.size() && boxNearCursor(b.faces[F.face], F.xf, px, py, tolPx) && outlineNearCursor(F, px, py, tolPx))
            near.push_back(i);
    }
    if (near.size() < 3 || near.size() > 400) return false; // too many to try every three
    const Ray r = cam_.ray(px, py);
    // Nothing in front of p along the ray through p itself (the cursor is a few
    // pixels off: on a face seen at a grazing angle, the depth under the
    // cursor says little about the depth at p).
    auto unhidden = [&](const Vec3& p) {
        double sx, sy;
        if (!cam_.project(p, sx, sy)) return false;
        const Ray rp = cam_.ray(sx, sy);
        double t;
        if (frontFace(faces, rp, t) < 0) return true;
        return (p - rp.origin).dot(rp.dir) <= t + std::max(cam_.pixelSize(p) * 3.0, 1e-4);
    };
    (void)tFront;
    double best = tolPx;
    bool found = false;
    for (size_t a = 0; a < near.size(); ++a) {
        const Plane& A = faces[near[a]].plane;
        for (size_t b = a + 1; b < near.size(); ++b) {
            const Plane& B = faces[near[b]].plane;
            Vec3 u = A.n.cross(B.n);
            if (u.length() < 1e-6) continue;
            // The line where A and B meet must pass near the cursor.
            const Vec3 p0 = (B.n.cross(u) * A.d + u.cross(A.n) * B.d) / u.length2();
            double s, t;
            if (!closestLineLine(p0, u.normalized(), r.origin, r.dir, s, t) || pixelDistance(p0 + u.normalized() * s, px, py) >= best) continue;
            for (size_t c = b + 1; c < near.size(); ++c) {
                const Plane& C = faces[near[c]].plane;
                const double det = C.n.dot(u);
                if (std::fabs(det) < 1e-6) continue;
                const Vec3 p = (B.n.cross(C.n) * A.d + C.n.cross(A.n) * B.d + u * C.d) / det;
                const double d = pixelDistance(p, px, py);
                if (d >= best) continue;
                if (cam_.perspective && (p - r.origin).dot(r.dir) < 0) continue;
                if (!faceContains(faces[near[a]], p) || !faceContains(faces[near[b]], p) || !faceContains(faces[near[c]], p)) continue;
                if (!seeThrough() && !unhidden(p)) continue;
                best = d;
                out = p;
                found = true;
            }
        }
    }
    return found;
}

bool InferenceEngine::pickIntersection(double px, double py, double tolPx, const std::vector<FaceRef>& faces, EdgeHit& hit) const {
    const Ray r = cam_.ray(px, py);
    double tFront;
    const int front = frontFace(faces, r, tFront);
    if (front < 0) return false;
    const FaceRef& A = faces[front];
    // Faces of one placement that share an edge meet along it: not an intersection.
    auto adjacent = [](const FaceRef& a, const FaceRef& b) {
        if (a.mesh != b.mesh || !std::equal(std::begin(a.xf.m), std::end(a.xf.m), std::begin(b.xf.m))) return false;
        const std::vector<Id> va = a.mesh->faceVertices(a.face), vb = b.mesh->faceVertices(b.face);
        int shared = 0;
        for (Id v : va) shared += std::find(vb.begin(), vb.end(), v) != vb.end();
        return shared >= 2;
    };
    // The piece [lo, hi] of the line p0 + u*s around sq that lies inside face F.
    auto clip = [](const FaceRef& F, const Vec3& p0, const Vec3& u, double sq, double& lo, double& hi) {
        const Face& face = F.mesh->faces[F.face];
        auto loop = [&](const std::vector<Id>& L) {
            for (size_t i = 0; i < L.size(); ++i) {
                const Vec3 a = F.xf.transformPoint(F.mesh->pos(L[i])), b = F.xf.transformPoint(F.mesh->pos(L[(i + 1) % L.size()]));
                double s, v;
                if (!closestLineLine(p0, u, a, b - a, s, v) || v < -1e-9 || v > 1 + 1e-9) continue;
                if ((p0 + u * s - lerp(a, b, v)).length() > kTol) continue;
                if (s < sq - 1e-9) lo = std::max(lo, s);
                else if (s > sq + 1e-9) hi = std::min(hi, s);
            }
        };
        loop(face.outer);
        for (const auto& h : face.holes) loop(h);
    };
    double bestD = tolPx;
    bool found = false;
    for (int j = 0; j < (int)faces.size(); ++j) {
        if (j == front) continue;
        const FaceRef& B = faces[j];
        Vec3 u = A.plane.n.cross(B.plane.n);
        if (u.length() < 1e-6) continue; // parallel
        const Vec3 p0 = (B.plane.n.cross(u) * A.plane.d + u.cross(A.plane.n) * B.plane.d) / u.length2();
        u = u.normalized();
        double s, t;
        if (!closestLineLine(p0, u, r.origin, r.dir, s, t)) continue;
        const Vec3 q = p0 + u * s;
        const double d = pixelDistance(q, px, py);
        if (d >= bestD) continue;
        const double depth = (q - r.origin).dot(r.dir);
        if (cam_.perspective && depth < 0) continue;
        if (depth > tFront + std::max(cam_.pixelSize(q) * 3.0, 1e-4)) continue; // hidden
        if (!faceContains(A, q) || !faceContains(B, q) || adjacent(A, B)) continue;
        double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
        clip(A, p0, u, s, lo, hi);
        clip(B, p0, u, s, lo, hi);
        if (!std::isfinite(lo)) lo = s;
        if (!std::isfinite(hi)) hi = s;
        if (hi - lo < kTol) continue;
        bestD = d;
        hit.seg = {p0 + u * lo, p0 + u * hi};
        hit.edge = kNone;
        hit.p = q;
        hit.intersection = true;
        found = true;
    }
    return found;
}

Id InferenceEngine::pickEdge(double px, double py, double tolPx, const std::set<Id>* ex) const {
    double tFace;
    pickFace(px, py, &tFace, ex);
    return pickEdgeT(px, py, tolPx, ex, tFace);
}

Id InferenceEngine::pickEdgeT(double px, double py, double tolPx, const std::set<Id>* ex, double tFace) const {
    const Mesh& m = doc_.mesh();
    Ray r = cam_.ray(px, py);
    Id best = kNone;
    double bestD = tolPx;
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        const Edge& E = m.edges[e];
        if (!E.alive) continue;
        if ((E.hidden || E.soft) && !doc_.style.showHidden) continue;
        if (ex && (ex->count(E.v[0]) || ex->count(E.v[1]))) continue;
        Vec3 a = m.pos(E.v[0]), b = m.pos(E.v[1]);
        double s, t;
        Vec3 q;
        if (closestLineLine(a, b - a, r.origin, r.dir, s, t)) q = lerp(a, b, std::clamp(s, 0.0, 1.0));
        else q = a;
        double d = pixelDistance(q, px, py);
        if (d > bestD) continue;
        // Occlusion: must not be behind the hit face.
        double depth = (q - r.origin).dot(r.dir);
        if (!seeThrough() && depth > tFace + std::max(cam_.pixelSize(q) * 3.0, 1e-4)) continue;
        if (cam_.perspective && depth < 0) continue;
        bestD = d;
        best = e;
    }
    return best;
}

std::vector<Id> InferenceEngine::edgesInBrush(double px, double py, double rad, bool includeSoft) const {
    std::vector<Id> out;
    const Mesh& m = doc_.mesh();
    rad = std::max(rad, 0.5);
    // Seen at the screen point (sx, sy): the edge's point there is not behind
    // the front-most face along that pixel's ray.
    auto visibleAt = [&](const Vec3& a, const Vec3& b, double sx, double sy) {
        const Ray r = cam_.ray(sx, sy);
        double s, t;
        const Vec3 q = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
        const double depth = (q - r.origin).dot(r.dir);
        if (cam_.perspective && depth < 0) return false;
        if (seeThrough()) return true;
        double tFace;
        pickFace(sx, sy, &tFace);
        return depth <= tFace + std::max(cam_.pixelSize(q) * 3.0, 1e-4);
    };
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        const Edge& E = m.edges[e];
        if (!E.alive) continue;
        if (E.hidden && !doc_.style.showHidden) continue;
        if (E.soft && !doc_.style.showHidden && !includeSoft) continue;
        const Vec3 a = m.pos(E.v[0]), b = m.pos(E.v[1]);
        double ax, ay, bx, by;
        const bool pa = cam_.project(a, ax, ay), pb = cam_.project(b, bx, by);
        if (!pa && !pb) continue;
        if (!pa || !pb) {
            // Reaching behind the camera: the point nearest the cursor's ray.
            const Ray r = cam_.ray(px, py);
            double s, t;
            const Vec3 q = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
            if (pixelDistance(q, px, py) <= rad && visibleAt(a, b, px, py)) out.push_back(e);
            continue;
        }
        // The stretch of the edge on screen inside the brush circle.
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        const double fx = ax - px, fy = ay - py;
        double u0 = 0, u1 = 1, uc = 0;
        if (len2 < 1e-12) {
            if (fx * fx + fy * fy > rad * rad) continue;
        } else {
            uc = std::clamp(-(fx * dx + fy * dy) / len2, 0.0, 1.0);
            const double cx = fx + uc * dx, cy = fy + uc * dy;
            if (cx * cx + cy * cy > rad * rad) continue;
            // |f + u d|² = r²
            const double B = 2 * (fx * dx + fy * dy), C = fx * fx + fy * fy - rad * rad;
            const double disc = std::max(0.0, B * B - 4 * len2 * C), sq = std::sqrt(disc);
            u0 = std::clamp((-B - sq) / (2 * len2), 0.0, 1.0);
            u1 = std::clamp((-B + sq) / (2 * len2), 0.0, 1.0);
        }
        // Some of that stretch must be in sight (a few points along it).
        for (double u : {uc, u0, u1, (u0 + u1) / 2, u0 + (u1 - u0) * 0.25, u0 + (u1 - u0) * 0.75})
            if (visibleAt(a, b, ax + u * dx, ay + u * dy)) {
                out.push_back(e);
                break;
            }
    }
    return out;
}

Id InferenceEngine::pickInstanceInFront(double px, double py) const {
    double t;
    // pickFace answers kNone when a component is the front-most thing there.
    if (pickFace(px, py, &t) != kNone && !seeThrough()) return kNone;
    return pickInstance(px, py);
}

int InferenceEngine::pickGuide(double px, double py, double tolPx) const {
    const auto& gs = doc_.state.guides;
    Ray r = cam_.ray(px, py);
    int best = -1;
    double bestD = tolPx;
    for (int i = 0; i < (int)gs.size(); ++i) {
        const Guide& g = gs[i];
        Vec3 q = g.p;
        if (g.kind == Guide::Line) {
            double s, t;
            if (!closestLineLine(g.p, g.dir, r.origin, r.dir, s, t)) continue;
            q = g.p + g.dir * s;
        }
        double d = pixelDistance(q, px, py);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

Id InferenceEngine::pickObject(double px, double py, double tolPx) const {
    if (!doc_.style.showObjects || doc_.editingComponent()) return kNone;
    Id best = kNone;
    double bestD = tolPx;
    const auto& objs = doc_.state.objects;
    for (Id i = 0; i < (Id)objs.size(); ++i) {
        if (!objs[i].alive || objs[i].hidden) continue;
        double x, y;
        if (!cam_.project(objs[i].pos, x, y)) continue;
        const double d = std::hypot(x - px, y - py);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

PickResult InferenceEngine::pick(double px, double py, double edgeTolPx, const std::set<Id>* ex) const {
    PickResult pr;
    Ray r = cam_.ray(px, py);
    double tInst;
    Id inst = pickInstance(px, py, &tInst);
    pr.edge = pickEdge(px, py, edgeTolPx, ex);
    if (inst != kNone && pr.edge == kNone) {
        // pickFace already treats components as occluders.
        double t;
        if (pickFace(px, py, &t, ex) == kNone) {
            pr.instance = inst;
            pr.t = tInst;
            pr.p = r.at(tInst);
            return pr;
        }
    }
    if (pr.edge != kNone) {
        const Mesh& m = doc_.mesh();
        Vec3 a = m.pos(m.edges[pr.edge].v[0]), b = m.pos(m.edges[pr.edge].v[1]);
        double s, t;
        pr.p = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
        return pr;
    }
    double t;
    pr.face = pickFace(px, py, &t, ex);
    if (pr.face != kNone) {
        pr.t = t;
        pr.p = r.at(t);
    }
    return pr;
}

Vec3 InferenceEngine::freePoint(const Ray& r, const Options& o) const {
    double t;
    if (o.plane) {
        if (intersectRayPlane(r, *o.plane, t)) return r.at(t);
        Ray back{r.origin, -r.dir};
        if (intersectRayPlane(back, *o.plane, t)) return back.at(t);
        return o.plane->project(r.at(cam_.distance()));
    }
    if (o.ref) {
        // Plane through the reference point facing the camera (axis-aligned).
        Vec3 f = cam_.forward();
        int ax = 2;
        if (std::fabs(f.x) > std::fabs(f[ax])) ax = 0;
        if (std::fabs(f.y) > std::fabs(f[ax])) ax = 1;
        Plane p(kAxes[ax], *o.ref);
        if (intersectRayPlane(r, p, t) && t < cam_.distance() * 200) return r.at(t);
        Plane pv(f, *o.ref);
        if (intersectRayPlane(r, pv, t)) return r.at(t);
        return r.at(cam_.distance());
    }
    Vec3 f = cam_.forward();
    if (std::fabs(f.z) < 0.25) {
        // Near-horizontal view (front/side): the vertical axis plane through the target.
        Plane pv(std::fabs(f.x) > std::fabs(f.y) ? Vec3(1, 0, 0) : Vec3(0, 1, 0), cam_.target);
        if (intersectRayPlane(r, pv, t)) return r.at(t);
    }
    Plane ground(Vec3(0, 0, 1), Vec3());
    if (intersectRayPlane(r, ground, t) && t < cam_.distance() * 200) return r.at(t);
    Plane pv(cam_.forward(), cam_.target);
    if (intersectRayPlane(r, pv, t)) return r.at(t);
    return r.at(cam_.distance());
}

InferenceResult InferenceEngine::infer(double px, double py, const Options& o) {
    const Mesh& m = doc_.mesh();
    Ray r = cam_.ray(px, py);
    InferenceResult res;
    const double snap = o.snapPx;

    double tFace = std::numeric_limits<double>::infinity();
    Id face = pickFace(px, py, &tFace, o.excludeVerts);
    // Faces of components and, inside an open component, of everything around
    // it: they hide what is behind them and count as faces to snap on.
    const std::vector<FaceRef> faces = worldFaces(o.excludeVerts, px, py, std::max(snap, 1.0));
    double tAll;
    const int front = frontFace(faces, r, tAll);
    if (front >= 0 && tAll < tFace - 1e-9) {
        tFace = tAll;
        face = faces[front].active ? faces[front].face : kNone;
    }
    if (face != kNone) rememberedNormal_ = m.faces[face].normal;
    else if (front >= 0) rememberedNormal_ = faces[front].plane.n;
    ix_.reset();
    if (EdgeHit ih; pickIntersection(px, py, snap, faces, ih)) ix_ = ih;
    auto visible = [&](const Vec3& p) {
        double depth = (p - r.origin).dot(r.dir);
        if (cam_.perspective && depth < 0) return false;
        // X-ray or back edges shown: what is behind the faces snaps too.
        return seeThrough() || depth <= tFace + std::max(cam_.pixelSize(p) * 3.0, 1e-4);
    };

    // Where the line ref + s*d pierces the hovered face, if that is near the
    // cursor (lines lying in the face plane never pierce it).
    auto pierceFace = [&](const Vec3& ref, const Vec3& d, Vec3& out) {
        if (face == kNone) return false;
        Plane P = m.facePlane(face);
        double den = P.n.dot(d);
        if (std::fabs(den) < 1e-6) return false;
        Vec3 p = ref + d * ((P.d - P.n.dot(ref)) / den);
        if (!m.pointInFace(face, p) || pixelDistance(p, px, py) > snap) return false;
        out = p;
        return true;
    };

    // --- point snaps --------------------------------------------------------
    Id bestV = kNone;
    double bestVD = snap;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
        if (!m.vertices[v].alive || excluded(v, o)) continue;
        const Vec3& p = m.vertices[v].p;
        double d = pixelDistance(p, px, py);
        if (d < bestVD && visible(p)) {
            // Ignore vertices only connected to hidden geometry.
            bestVD = d;
            bestV = v;
        }
    }
    // Corners of components.
    bool instPoint = false;
    Vec3 instP;
    if (bestV == kNone) {
        double bd = snap;
        for (const PlacedInstance& placed : snapSources()) {
            if (!snapsTo(placed) || !nearCursor(placed, px, py, snap)) continue;
            const Mat4& xf = placed.xf;
            for (const Vertex& v : sourceMesh(placed).vertices) {
                if (!v.alive) continue;
                Vec3 p = xf.transformPoint(v.p);
                double d = pixelDistance(p, px, py);
                if (d < bd && visible(p)) {
                    bd = d;
                    instP = p;
                    instPoint = true;
                }
            }
        }
    }
    // Ends of a face-face intersection line (where it leaves one of the faces).
    bool ixEnd = false;
    if (bestV == kNone && !instPoint && ix_) {
        double bd = snap;
        for (const Vec3& p : {ix_->seg.a, ix_->seg.b}) {
            const double d = pixelDistance(p, px, py);
            if (d < bd && visible(p)) {
                bd = d;
                instP = p;
                instPoint = ixEnd = true;
            }
        }
    }
    // Corners where three faces meet (no vertex there), when closer than an
    // intersection line's end.
    if (bestV == kNone && (!instPoint || ixEnd)) {
        const double lim = ixEnd ? pixelDistance(instP, px, py) : snap;
        if (Vec3 c; pickCorner(px, py, lim, faces, tFace, c)) {
            instP = c;
            instPoint = true;
            ixEnd = false;
            res.hasEdge = false;
            res.type = InferenceResult::Intersection;
        }
    }
    Id bestMid = kNone;
    double bestMD = snap;
    if (bestV == kNone && !instPoint) {
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            const Edge& E = m.edges[e];
            if (!E.alive || E.soft || E.hidden) continue;
            if (excluded(E.v[0], o) || excluded(E.v[1], o)) continue;
            Vec3 mid = (m.pos(E.v[0]) + m.pos(E.v[1])) * 0.5;
            double d = pixelDistance(mid, px, py);
            if (d < bestMD && visible(mid)) {
                bestMD = d;
                bestMid = e;
            }
        }
    }
    // Midpoints of component edges.
    std::optional<Segment> instMid;
    if (bestV == kNone && !instPoint && bestMid == kNone) {
        double bd = snap;
        const std::vector<PlacedInstance> placed = snapSources();
        for (int i = 0; i < (int)placed.size(); ++i) {
            if (!snapsTo(placed[i]) || !nearCursor(placed[i], px, py, snap)) continue;
            for (const Segment& s : instanceEdges(i, placed[i])) {
                Vec3 mid = (s.a + s.b) * 0.5;
                double d = pixelDistance(mid, px, py);
                if (d < bd && visible(mid)) {
                    bd = d;
                    instMid = s;
                }
            }
        }
    }
    bool origin = bestV == kNone && !instPoint && bestMid == kNone && !instMid && doc_.style.showAxes && pixelDistance(Vec3(), px, py) < snap &&
                  visible(Vec3());
    int guidePt = -1;
    if (bestV == kNone && !instPoint && bestMid == kNone && !instMid && !origin && doc_.style.showGuides) {
        double bd = snap;
        for (int i = 0; i < (int)doc_.state.guides.size(); ++i) {
            const Guide& g = doc_.state.guides[i];
            if (g.kind != Guide::Point) continue;
            double d = pixelDistance(g.p, px, py);
            if (d < bd) { bd = d; guidePt = i; }
        }
    }

    if (bestV != kNone) {
        res.type = InferenceResult::Endpoint;
        res.p = m.pos(bestV);
        res.vertex = bestV;
        remembered_ = res.p;
    } else if (instPoint) {
        res.type = ixEnd || res.type == InferenceResult::Intersection ? InferenceResult::Intersection : InferenceResult::Endpoint;
        res.p = instP;
        remembered_ = res.p;
        if (ixEnd) {
            res.hasEdge = true;
            res.edgeA = ix_->seg.a;
            res.edgeB = ix_->seg.b;
        }
    } else if (bestMid != kNone) {
        res.type = InferenceResult::Midpoint;
        res.edge = bestMid;
        res.p = (m.pos(m.edges[bestMid].v[0]) + m.pos(m.edges[bestMid].v[1])) * 0.5;
        remembered_ = res.p;
        rememberedEdge_ = Segment{m.pos(m.edges[bestMid].v[0]), m.pos(m.edges[bestMid].v[1])};
        res.hasEdge = true;
        res.edgeA = rememberedEdge_->a;
        res.edgeB = rememberedEdge_->b;
    } else if (instMid) {
        res.type = InferenceResult::Midpoint;
        res.p = (instMid->a + instMid->b) * 0.5;
        remembered_ = res.p;
        rememberedEdge_ = instMid;
        res.hasEdge = true;
        res.edgeA = instMid->a;
        res.edgeB = instMid->b;
    } else if (origin) {
        res.type = InferenceResult::Origin;
        res.p = Vec3();
        remembered_ = res.p;
    } else if (guidePt >= 0) {
        res.type = InferenceResult::GuidePoint;
        res.p = doc_.state.guides[guidePt].p;
        remembered_ = res.p;
    }

    // --- locked direction ----------------------------------------------------
    if (o.ref && o.lockDir) {
        Vec3 d = o.lockDir->normalized();
        Vec3 q;
        InferenceResult lr;
        Color lc = o.lockAxis >= 0 ? Renderer::axisColor(o.lockAxis) : Color::rgb8(220, 0, 220);
        if (res.type != InferenceResult::None) {
            double s = (res.p - *o.ref).dot(d);
            q = *o.ref + d * s;
            lr.hasLine = true;
            lr.lineA = res.p;
            lr.lineB = q;
            lr.lineColor = lc;
        } else {
            EdgeHit eh;
            double s, t;
            if (pickAnyEdge(px, py, 6.0, o.excludeVerts, tFace, eh)) {
                // Project the hovered edge point on the locked line.
                Vec3 a = eh.seg.a, b = eh.seg.b, ep = eh.p;
                if (closestLineLine(*o.ref, d, a, b - a, s, t) && t >= -1e-9 && t <= 1 + 1e-9) q = *o.ref + d * s;
                else q = *o.ref + d * (ep - *o.ref).dot(d);
            } else if (Vec3 fp; pierceFace(*o.ref, d, fp)) {
                q = fp; // stop exactly on the hovered surface
            } else if (closestLineLine(*o.ref, d, r.origin, r.dir, s, t)) {
                q = *o.ref + d * s;
            } else {
                q = *o.ref;
            }
        }
        lr.type = o.lockAxis == 0 ? InferenceResult::AxisX
                : o.lockAxis == 1 ? InferenceResult::AxisY
                : o.lockAxis == 2 ? InferenceResult::AxisZ : InferenceResult::Locked;
        lr.p = q;
        lr.dir = d;
        lr.hasDir = true;
        if (o.plane) lr.p = o.plane->project(lr.p);
        return lr;
    }

    // --- angle steps from the base edge (Shift) ---------------------------------
    if (o.ref && o.baseDir && o.angleStepDeg > 0 && o.baseDir->length() > kTol) {
        const Vec3 e = o.baseDir->normalized();
        Vec3 raw = res.type != InferenceResult::None ? res.p : std::isfinite(tFace) ? r.at(tFace) : freePoint(r, o);
        if (o.plane) raw = o.plane->project(raw);
        const Vec3 v = raw - *o.ref;
        if (v.length() > kTol) {
            // The plane of the angle: the drawing plane, or else the one
            // through the base edge and the cursor.
            Vec3 n = e.cross(v);
            if (o.plane && std::fabs(o.plane->n.dot(e)) < 1e-6) n = n.dot(o.plane->n) < 0 ? -o.plane->n : o.plane->n;
            const Vec3 side = n.length() > 1e-9 * v.length() ? n.normalized().cross(e) : Vec3();
            const double theta = std::atan2(v.dot(side), v.dot(e)); // 0..pi
            const double step = o.angleStepDeg * kPi / 180.0;
            const double ts = std::round(theta / step) * step;
            const Vec3 d = (e * std::cos(ts) + side * std::sin(ts)).normalized();
            InferenceResult ar;
            double s, t;
            if (res.type != InferenceResult::None) {
                s = (res.p - *o.ref).dot(d);
                ar.hasLine = true; // dotted line from the snapped point
                ar.lineA = res.p;
                ar.lineB = *o.ref + d * s;
                ar.lineColor = Color::rgb8(120, 120, 120);
            } else if (!closestLineLine(*o.ref, d, r.origin, r.dir, s, t)) {
                s = v.dot(d);
            }
            ar.type = InferenceResult::Angle;
            ar.p = *o.ref + d * s;
            ar.dir = d;
            ar.hasDir = true;
            ar.angleDeg = ts * 180.0 / kPi;
            return ar;
        }
    }

    if (res.type != InferenceResult::None) {
        if (o.plane && !o.plane->contains(res.p)) {
            // Keep the snap but project onto the drawing plane.
            res.hasLine = true;
            res.lineA = res.p;
            res.p = o.plane->project(res.p);
            res.lineB = res.p;
            res.lineColor = Color::rgb8(120, 120, 120);
        }
        if (o.ref) {
            Vec3 d = res.p - *o.ref;
            for (int i = 0; i < 3; ++i)
                if (d.length() > kTol && std::fabs(d.normalized().dot(kAxes[i])) > 1 - 1e-9) {
                    res.dir = kAxes[i];
                    res.hasDir = true;
                }
        }
        return res;
    }

    // --- axis inference from the reference point -------------------------------
    int bestAxis = -1;
    double bestAD = 8.0;
    Vec3 axisPt;
    if (o.ref && o.axes) {
        for (int i = 0; i < 3; ++i) {
            if (o.plane && std::fabs(o.plane->n.dot(kAxes[i])) > 1e-6) continue; // not in plane
            double s, t;
            if (!closestLineLine(*o.ref, kAxes[i], r.origin, r.dir, s, t)) continue;
            Vec3 q = *o.ref + kAxes[i] * s;
            double d = pixelDistance(q, px, py);
            // Ignore when the axis is nearly pointing at the camera.
            double sx0, sy0, sx1, sy1;
            if (!cam_.project(*o.ref, sx0, sy0) || !cam_.project(*o.ref + kAxes[i] * cam_.pixelSize(*o.ref) * 50, sx1, sy1))
                continue;
            if (std::hypot(sx1 - sx0, sy1 - sy0) < 10) continue;
            if (d < bestAD) {
                bestAD = d;
                bestAxis = i;
                axisPt = q;
            }
        }
    }
    // Directions of existing geometry (magenta): along the edges meeting at the
    // reference point, both ways, and parallel to the last hovered edge.
    bool edgeDir = false;
    InferenceResult::Type dirType = InferenceResult::AlongEdge;
    Vec3 edgeDirV, edgePt;
    bool squareToBase = false; // the Perpendicular found is to an edge at the start
    if (o.ref && o.axes) {
        struct Cand {
            Vec3 dir;
            InferenceResult::Type type;
            bool square = false;
        };
        std::vector<Cand> cands;
        // Square to the edge(s) the line starts from: discrete lines only, so
        // the light-blue inference never takes over a whole plane.
        {
            std::vector<Vec3> sq = o.squareDirs;
            if (o.baseDir && o.baseDir->length() > kTol) {
                const Vec3 e = o.baseDir->normalized();
                if (o.plane && std::fabs(o.plane->n.dot(e)) < 1e-6) sq.insert(sq.begin(), o.plane->n.cross(e));
                if (o.squareDirs.empty() && !o.plane) {
                    const Vec3 h = e.cross(Vec3(0, 0, 1));
                    if (h.length() > 1e-6) {
                        sq.push_back(h);
                        sq.push_back(h.cross(e));
                    }
                }
            }
            for (const Vec3& d : sq)
                if (d.length() > 1e-9) cands.push_back({d.normalized(), InferenceResult::Perpendicular, true});
        }
        Id rv = m.findVertex(*o.ref);
        if (rv != kNone)
            for (Id e : m.vertexEdges(rv)) {
                const Edge& E = m.edges[e];
                if (!E.alive || E.hidden) continue;
                Vec3 d = m.pos(E.other(rv)) - *o.ref;
                if (d.length() > kTol) cands.push_back({d.normalized(), InferenceResult::AlongEdge, false});
            }
        // Edges of components meeting at the reference point.
        const std::vector<PlacedInstance> placed = snapSources();
        for (int i = 0; i < (int)placed.size(); ++i) {
            if (!snapsTo(placed[i])) continue;
            const MeshBoxes& pb = boxesOf(sourceMesh(placed[i]));
            if (!pb.any || !boxContains(pb.all, placed[i].xf, *o.ref, 2 * kTol)) continue; // no corner of it at the reference point
            for (const Segment& s : instanceEdges(i, placed[i])) {
                Vec3 d = s.a.nearlyEquals(*o.ref) ? s.b - s.a : s.b.nearlyEquals(*o.ref) ? s.a - s.b : Vec3();
                if (d.length() > kTol) cands.push_back({d.normalized(), InferenceResult::AlongEdge, false});
            }
        }
        if (rememberedEdge_) {
            Vec3 d = rememberedEdge_->b - rememberedEdge_->a;
            if (d.length() > kTol) {
                d = d.normalized();
                cands.push_back({d, InferenceResult::Parallel, false});
                // Perpendicular: square to the edge within the drawing plane,
                // or else towards the edge's line.
                Vec3 foot = rememberedEdge_->a + d * (*o.ref - rememberedEdge_->a).dot(d);
                Vec3 pd = foot - *o.ref;
                if (o.plane && std::fabs(o.plane->n.dot(d)) < 1e-6)
                    cands.push_back({o.plane->n.cross(d).normalized(), InferenceResult::Perpendicular, false});
                else if (pd.length() > kTol)
                    cands.push_back({pd.normalized(), InferenceResult::Perpendicular, false});
            }
        }
        if (rememberedNormal_) cands.push_back({*rememberedNormal_, InferenceResult::FaceNormal, false});
        // The same line can come from several sources: keep the first.
        for (size_t i = 0; i < cands.size(); ++i)
            for (size_t j = i + 1; j < cands.size();)
                if (std::fabs(cands[i].dir.dot(cands[j].dir)) > 1 - 1e-9) cands.erase(cands.begin() + j);
                else ++j;
        for (const Cand& c : cands) {
            bool axisAligned = false;
            for (int i = 0; i < 3; ++i) axisAligned = axisAligned || std::fabs(c.dir.dot(kAxes[i])) > 1 - 1e-6;
            if (axisAligned) continue; // the axis inference already covers it
            if (o.plane && std::fabs(o.plane->n.dot(c.dir)) > 1e-6) continue;
            double s, t;
            if (!closestLineLine(*o.ref, c.dir, r.origin, r.dir, s, t)) continue;
            Vec3 q = *o.ref + c.dir * s;
            double sx0, sy0, sx1, sy1;
            if (!cam_.project(*o.ref, sx0, sy0) || !cam_.project(*o.ref + c.dir * cam_.pixelSize(*o.ref) * 50, sx1, sy1))
                continue;
            if (std::hypot(sx1 - sx0, sy1 - sy0) < 10) continue; // pointing at the camera
            double d = pixelDistance(q, px, py);
            if (d < bestAD) {
                bestAD = d;
                bestAxis = -1;
                edgeDir = true;
                dirType = c.type;
                edgeDirV = c.dir;
                edgePt = q;
                squareToBase = c.square;
            }
        }
    }
    // A direction inference must not keep the line from ending on geometry:
    // stop where the direction line crosses the hovered edge or pierces the
    // hovered face. An edge the line never meets wins over the direction.
    bool edgeWins = false;
    if (bestAxis >= 0 || edgeDir) {
        Vec3 d = edgeDir ? edgeDirV : kAxes[bestAxis];
        EdgeHit eh;
        Vec3 stop;
        bool stopped = false;
        if (pickAnyEdge(px, py, 6.0, o.excludeVerts, tFace, eh)) {
            Vec3 a = eh.seg.a, b = eh.seg.b;
            double s, t;
            bool atRef = false;
            if (closestLineLine(*o.ref, d, a, b - a, s, t) && t >= -1e-9 && t <= 1 + 1e-9) {
                Vec3 p1 = *o.ref + d * s, p2 = lerp(a, b, t);
                // An edge through the start point crosses the line there: ignore it.
                atRef = (p1 - *o.ref).length() < kTol * 10;
                if (!atRef && (p1 - p2).length() < std::max(cam_.pixelSize(p1) * 2, kTol * 10) && pixelDistance(p1, px, py) < snap) {
                    res.type = eh.intersection ? InferenceResult::OnIntersection : InferenceResult::OnEdge;
                    res.edge = eh.edge;
                    res.hasEdge = true;
                    res.edgeA = a;
                    res.edgeB = b;
                    stop = p1;
                    stopped = true;
                }
            }
            edgeWins = !stopped && !atRef;
        } else if (pierceFace(*o.ref, d, stop)) {
            res.type = InferenceResult::OnFace;
            res.face = face;
            stopped = true;
        }
        if (stopped && (!o.plane || o.plane->contains(stop, kTol * 10))) {
            res.p = stop;
            res.dir = d;
            res.hasDir = true;
            return res;
        }
        if (stopped) res = InferenceResult();
    }
    if (edgeDir && !edgeWins) {
        res.type = dirType;
        res.p = o.plane ? o.plane->project(edgePt) : edgePt;
        res.dir = edgeDirV;
        res.hasDir = true;
        res.square = squareToBase;
        if ((dirType == InferenceResult::Parallel || dirType == InferenceResult::Perpendicular) && !squareToBase && rememberedEdge_) {
            // Highlight the edge the line runs parallel or square to. Light
            // blue is kept for edges the line starts from.
            res.hasLine = true;
            res.lineA = rememberedEdge_->a;
            res.lineB = rememberedEdge_->b;
            res.lineColor = Color::rgb8(220, 0, 220);
        }
        return res;
    }
    if (bestAxis >= 0 && !edgeWins) {
        res.type = bestAxis == 0 ? InferenceResult::AxisX : bestAxis == 1 ? InferenceResult::AxisY : InferenceResult::AxisZ;
        res.p = axisPt;
        res.dir = kAxes[bestAxis];
        res.hasDir = true;
        // "From point": align with the remembered point along this axis.
        if (remembered_ && !remembered_->nearlyEquals(*o.ref)) {
            double s = (*remembered_ - *o.ref).dot(kAxes[bestAxis]);
            Vec3 q = *o.ref + kAxes[bestAxis] * s;
            if (pixelDistance(q, px, py) < snap && !q.nearlyEquals(*remembered_)) {
                res.type = InferenceResult::FromPoint;
                res.p = q;
                res.hasLine = true;
                res.lineA = *remembered_;
                res.lineB = q;
                // Colour of the alignment direction.
                Vec3 dd = q - *remembered_;
                int ax = 0;
                for (int i = 1; i < 3; ++i)
                    if (std::fabs(dd[i]) > std::fabs(dd[ax])) ax = i;
                res.lineColor = Renderer::axisColor(ax);
            }
        }
        return res;
    }

    // --- on edge / guide / face -------------------------------------------------
    if (EdgeHit eh; pickAnyEdge(px, py, 6.0, o.excludeVerts, tFace, eh)) {
        Vec3 a = eh.seg.a, b = eh.seg.b;
        res.type = eh.intersection ? InferenceResult::OnIntersection : InferenceResult::OnEdge;
        res.edge = eh.edge;
        res.hasEdge = true;
        res.edgeA = a;
        res.edgeB = b;
        rememberedEdge_ = eh.seg;
        res.p = eh.p;
        res.dir = (b - a).normalized();
        res.hasDir = true;
        if (o.plane) res.p = o.plane->project(res.p);
        return res;
    }
    if (doc_.style.showGuides) {
        int g = pickGuide(px, py, 6.0);
        if (g >= 0 && doc_.state.guides[g].kind == Guide::Line) {
            const Guide& gd = doc_.state.guides[g];
            double s, t;
            if (closestLineLine(gd.p, gd.dir, r.origin, r.dir, s, t)) {
                Vec3 q = gd.p + gd.dir * s;
                if (visible(q)) {
                    res.type = InferenceResult::OnGuide;
                    res.p = q;
                    res.dir = gd.dir;
                    res.hasDir = true;
                    if (o.plane) res.p = o.plane->project(res.p);
                    return res;
                }
            }
        }
    }
    if (face != kNone || std::isfinite(tFace)) { // a loose face or a component's face
        res.type = InferenceResult::OnFace;
        res.face = face;
        res.p = r.at(tFace);
        if (o.plane) {
            double t;
            if (intersectRayPlane(r, *o.plane, t) && !o.plane->contains(res.p)) res.p = r.at(t);
            else res.p = o.plane->project(res.p);
        }
        return res;
    }
    res.type = InferenceResult::Free;
    res.p = freePoint(r, o);
    return res;
}

void InferenceEngine::addPreview(const InferenceResult& r, Preview& pv) const {
    using MK = Preview::MarkerKind;
    switch (r.type) {
    case InferenceResult::Endpoint: pv.markers.push_back({r.p, MK::Endpoint}); break;
    case InferenceResult::Midpoint: pv.markers.push_back({r.p, MK::Midpoint}); break;
    case InferenceResult::OnEdge: pv.markers.push_back({r.p, MK::OnEdge}); break;
    case InferenceResult::OnFace: pv.markers.push_back({r.p, MK::OnFace}); break;
    case InferenceResult::Origin: pv.markers.push_back({r.p, MK::Origin}); break;
    case InferenceResult::GuidePoint:
    case InferenceResult::OnGuide: pv.markers.push_back({r.p, MK::Guide}); break;
    case InferenceResult::Intersection: pv.markers.push_back({r.p, MK::Intersection}); break;
    case InferenceResult::OnIntersection: pv.markers.push_back({r.p, MK::OnEdge}); break;
    case InferenceResult::FromPoint: pv.markers.push_back({r.p, MK::Plain}); break;
    default: break;
    }
    if (r.hasLine) pv.line(r.lineA, r.lineB, r.lineColor, 1.2f, Dash::Dotted);
    // Show the whole intersection line the cursor is on.
    if ((r.type == InferenceResult::OnIntersection || r.type == InferenceResult::Intersection) && r.hasEdge)
        pv.line(r.edgeA, r.edgeB, Color::rgb8(220, 0, 220), 2.0f, Dash::Dashed);
    std::string l = r.label();
    if (!l.empty()) pv.labels.push_back({r.p, l});
}

} // namespace gs
