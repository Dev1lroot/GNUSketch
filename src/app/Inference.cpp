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

Id InferenceEngine::pickInstance(double px, double py, double* tOut) const {
    const ModelState& st = doc_.state;
    Id best = kNone;
    double bestT = std::numeric_limits<double>::infinity();
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
            }
        }
    }
    if (tOut) *tOut = bestT;
    return best;
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
    auto visible = [&](const Vec3& p) {
        double depth = (p - r.origin).dot(r.dir);
        if (cam_.perspective && depth < 0) return false;
        return depth <= tFace + std::max(cam_.pixelSize(p) * 3.0, 1e-4);
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
    if (bestV == kNone && doc_.state.editing == kNone) {
        const ModelState& st = doc_.state;
        double bd = snap;
        for (Id i = 0; i < (Id)st.instances.size(); ++i) {
            const Instance& I = st.instances[i];
            if (!I.alive || I.hidden || ignoredInstances.count(i)) continue;
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
    bool origin = bestV == kNone && !instPoint && bestMid == kNone && doc_.style.showAxes && pixelDistance(Vec3(), px, py) < snap &&
                  visible(Vec3());
    int guidePt = -1;
    if (bestV == kNone && !instPoint && bestMid == kNone && !origin && doc_.style.showGuides) {
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
            Id e = pickEdgeT(px, py, 6.0, o.excludeVerts, tFace);
            double s, t;
            if (e != kNone) {
                // Project the hovered edge point on the locked line.
                Vec3 a = m.pos(m.edges[e].v[0]), b = m.pos(m.edges[e].v[1]);
                Vec3 ep = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
                if (closestLineLine(*o.ref, d, a, b - a, s, t) && t >= -1e-9 && t <= 1 + 1e-9) q = *o.ref + d * s;
                else q = *o.ref + d * (ep - *o.ref).dot(d);
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
    if (bestAxis >= 0) {
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
    Id e = pickEdgeT(px, py, 6.0, o.excludeVerts, tFace);
    if (e != kNone) {
        Vec3 a = m.pos(m.edges[e].v[0]), b = m.pos(m.edges[e].v[1]);
        double s, t;
        res.type = InferenceResult::OnEdge;
        res.edge = e;
        res.p = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
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
