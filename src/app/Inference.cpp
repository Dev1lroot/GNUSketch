// SPDX-License-Identifier: GPL-3.0-or-later
#include "Inference.h"

#include "core/Components.h"

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
    default: return "";
    }
}

double InferenceEngine::pixelDistance(const Vec3& p, double px, double py) const {
    double sx, sy;
    if (!cam_.project(p, sx, sy)) return 1e300;
    return std::hypot(sx - px, sy - py);
}

Id InferenceEngine::pickFace(double px, double py, double* tOut, const std::set<Id>* ex) const {
    const Mesh& m = doc_.mesh();
    Ray r = cam_.ray(px, py);
    Id best = kNone;
    double bestT = std::numeric_limits<double>::infinity();
    if (tOut) *tOut = bestT;
    if (doc_.style.faceStyle == RenderStyle::Wireframe) return kNone;
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        if (ex) {
            bool skip = false;
            for (Id v : F.outer)
                if (ex->count(v)) { skip = true; break; }
            if (skip) continue;
        }
        double t;
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
    if (st.editing != kNone) return kNone;
    Ray r = cam_.ray(px, py);
    const bool wire = doc_.style.faceStyle == RenderStyle::Wireframe;
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        const Instance& I = st.instances[i];
        if (!I.alive || I.hidden || ignoredInstances.count(i)) continue;
        const Mesh& m = st.defs[I.def].mesh;
        Mat4 xf = instanceWorldXf(st, i), inv = xf.inverted();
        // Ray in definition coordinates; same parameter t as in world space.
        Ray lr{inv.transformPoint(r.origin), inv.transformDir(r.dir)};
        if (!wire)
            for (Id f = 0; f < (Id)m.faces.size(); ++f) {
                if (!m.faces[f].alive || m.faces[f].hidden) continue;
                double t;
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

bool InferenceEngine::snapsTo(Id i) const {
    const ModelState& st = doc_.state;
    const Instance& I = st.instances[i];
    // The open component is the active mesh itself.
    return I.alive && !I.hidden && i != st.editing && !ignoredInstances.count(i);
}

const std::vector<InferenceEngine::Segment>& InferenceEngine::instanceEdges(Id i) const {
    const ModelState& st = doc_.state;
    const Mesh& m = st.defs[st.instances[i].def].mesh;
    const Mat4 xf = instanceWorldXf(st, i);
    CachedEdges& c = instEdges_[i];
    if (c.revision != m.revision || !std::equal(std::begin(c.xf.m), std::end(c.xf.m), std::begin(xf.m))) {
        c.revision = m.revision;
        c.xf = xf;
        c.edges.clear();
        for (const Edge& E : m.edges)
            if (E.alive && !E.soft && !E.hidden)
                c.edges.push_back({xf.transformPoint(m.pos(E.v[0])), xf.transformPoint(m.pos(E.v[1]))});
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
    for (Id i = 0; i < (Id)doc_.state.instances.size(); ++i) {
        if (!snapsTo(i)) continue;
        for (const Segment& s : instanceEdges(i)) {
            Vec3 q = closest(s.a, s.b);
            double d = pixelDistance(q, px, py);
            if (d >= bestD) continue;
            double depth = (q - r.origin).dot(r.dir);
            if (depth > tFace + std::max(cam_.pixelSize(q) * 3.0, 1e-4)) continue;
            if (cam_.perspective && depth < 0) continue;
            bestD = d;
            hit.seg = s;
            hit.edge = kNone;
            hit.p = q;
            found = true;
        }
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
        if (depth > tFace + std::max(cam_.pixelSize(q) * 3.0, 1e-4)) continue;
        if (cam_.perspective && depth < 0) continue;
        bestD = d;
        best = e;
    }
    return best;
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
    if (face != kNone) {
        rememberedNormal_ = m.faces[face].normal;
    } else if (std::isfinite(tFace)) {
        Vec3 n;
        pickInstance(px, py, nullptr, &n);
        if (n.length2() > 0.5) rememberedNormal_ = n;
    }
    auto visible = [&](const Vec3& p) {
        double depth = (p - r.origin).dot(r.dir);
        if (cam_.perspective && depth < 0) return false;
        return depth <= tFace + std::max(cam_.pixelSize(p) * 3.0, 1e-4);
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
        const ModelState& st = doc_.state;
        double bd = snap;
        for (Id i = 0; i < (Id)st.instances.size(); ++i) {
            const Instance& I = st.instances[i];
            if (!snapsTo(i)) continue;
            Mat4 xf = instanceWorldXf(st, i);
            for (const Vertex& v : st.defs[I.def].mesh.vertices) {
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
        for (Id i = 0; i < (Id)doc_.state.instances.size(); ++i) {
            if (!snapsTo(i)) continue;
            for (const Segment& s : instanceEdges(i)) {
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
        res.type = InferenceResult::Endpoint;
        res.p = instP;
        remembered_ = res.p;
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
    if (o.ref && o.axes) {
        struct Cand {
            Vec3 dir;
            InferenceResult::Type type;
        };
        std::vector<Cand> cands;
        Id rv = m.findVertex(*o.ref);
        if (rv != kNone)
            for (Id e : m.vertexEdges(rv)) {
                const Edge& E = m.edges[e];
                if (!E.alive || E.hidden) continue;
                Vec3 d = m.pos(E.other(rv)) - *o.ref;
                if (d.length() > kTol) cands.push_back({d.normalized(), InferenceResult::AlongEdge});
            }
        // Edges of components meeting at the reference point.
        for (Id i = 0; i < (Id)doc_.state.instances.size(); ++i) {
            if (!snapsTo(i)) continue;
            for (const Segment& s : instanceEdges(i)) {
                Vec3 d = s.a.nearlyEquals(*o.ref) ? s.b - s.a : s.b.nearlyEquals(*o.ref) ? s.a - s.b : Vec3();
                if (d.length() > kTol) cands.push_back({d.normalized(), InferenceResult::AlongEdge});
            }
        }
        if (rememberedEdge_) {
            Vec3 d = rememberedEdge_->b - rememberedEdge_->a;
            if (d.length() > kTol) {
                d = d.normalized();
                cands.push_back({d, InferenceResult::Parallel});
                // Perpendicular: square to the edge within the drawing plane,
                // or else towards the edge's line.
                Vec3 foot = rememberedEdge_->a + d * (*o.ref - rememberedEdge_->a).dot(d);
                Vec3 pd = foot - *o.ref;
                if (o.plane && std::fabs(o.plane->n.dot(d)) < 1e-6)
                    cands.push_back({o.plane->n.cross(d).normalized(), InferenceResult::Perpendicular});
                else if (pd.length() > kTol)
                    cands.push_back({pd.normalized(), InferenceResult::Perpendicular});
            }
        }
        if (rememberedNormal_) cands.push_back({*rememberedNormal_, InferenceResult::FaceNormal});
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
                    res.type = InferenceResult::OnEdge;
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
        if (dirType == InferenceResult::Parallel || dirType == InferenceResult::Perpendicular) {
            // Highlight the edge the line runs parallel to.
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
        res.type = InferenceResult::OnEdge;
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
    case InferenceResult::FromPoint: pv.markers.push_back({r.p, MK::Plain}); break;
    default: break;
    }
    if (r.hasLine) pv.line(r.lineA, r.lineB, r.lineColor, 1.2f, Dash::Dotted);
    std::string l = r.label();
    if (!l.empty()) pv.labels.push_back({r.p, l});
}

} // namespace gs
