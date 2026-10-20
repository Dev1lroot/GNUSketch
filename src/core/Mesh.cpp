// SPDX-License-Identifier: GPL-3.0-or-later
#include "Mesh.h"

#include "Triangulate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <unordered_set>

namespace gs {

uint64_t nextMeshRevision() {
    static std::atomic<uint64_t> counter{0};
    return ++counter;
}

namespace {

constexpr double kCell = 0.01; // spatial hash cell size (meters)

using LoopKey = std::vector<std::vector<Id>>;

LoopKey canonicalKey(const std::vector<Id>& outer, const std::vector<std::vector<Id>>& holes) {
    LoopKey k;
    std::vector<Id> o = outer;
    std::sort(o.begin(), o.end());
    k.push_back(std::move(o));
    std::vector<std::vector<Id>> hs;
    for (const auto& h : holes) {
        std::vector<Id> s = h;
        std::sort(s.begin(), s.end());
        hs.push_back(std::move(s));
    }
    std::sort(hs.begin(), hs.end());
    for (auto& h : hs) k.push_back(std::move(h));
    return k;
}

Vec3 loopNewell(const Mesh& m, const std::vector<Id>& loop) {
    return newellNormal((int)loop.size(), [&](int i) { return m.pos(loop[i]); });
}

std::vector<Vec2> project2D(const std::vector<Vec3>& pts, const Vec3& u, const Vec3& v) {
    std::vector<Vec2> r;
    r.reserve(pts.size());
    for (const Vec3& p : pts) r.emplace_back(p.dot(u), p.dot(v));
    return r;
}

double segDist2D(const Vec2& a, const Vec2& b, const Vec2& p) {
    Vec2 ab = b - a;
    double l2 = ab.dot(ab);
    double t = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0.0;
    return (a + ab * t - p).length();
}

} // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

void Mesh::ensureGrid() const {
    if (!gridDirty_) return;
    grid_.clear();
    gridDirty_ = false;
    for (Id i = 0; i < (Id)vertices.size(); ++i)
        if (vertices[i].alive) gridInsert(i);
}

void Mesh::gridInsert(Id v) const {
    if (gridDirty_) return;
    const Vec3& p = vertices[v].p;
    CellKey k{(int64_t)std::floor(p.x / kCell), (int64_t)std::floor(p.y / kCell), (int64_t)std::floor(p.z / kCell)};
    grid_[k].push_back(v);
}

Id Mesh::findVertex(const Vec3& p, double tol) const {
    ensureGrid();
    int64_t cx = (int64_t)std::floor(p.x / kCell), cy = (int64_t)std::floor(p.y / kCell),
            cz = (int64_t)std::floor(p.z / kCell);
    Id best = kNone;
    double bestD = tol * tol;
    int r = tol > kCell ? (int)std::ceil(tol / kCell) : 1;
    for (int64_t x = cx - r; x <= cx + r; ++x)
        for (int64_t y = cy - r; y <= cy + r; ++y)
            for (int64_t z = cz - r; z <= cz + r; ++z) {
                auto it = grid_.find(CellKey{x, y, z});
                if (it == grid_.end()) continue;
                for (Id v : it->second) {
                    if (!vertices[v].alive) continue;
                    double d = (vertices[v].p - p).length2();
                    if (d <= bestD) { bestD = d; best = v; }
                }
            }
    return best;
}

Id Mesh::findEdge(Id a, Id b) const {
    auto it = edgeMap_.find(edgeKey(a, b));
    if (it == edgeMap_.end()) return kNone;
    return edges[it->second].alive ? it->second : kNone;
}

void Mesh::edgeMapInsert(Id e) { edgeMap_[edgeKey(edges[e].v[0], edges[e].v[1])] = e; }

void Mesh::edgeMapErase(Id e) {
    auto it = edgeMap_.find(edgeKey(edges[e].v[0], edges[e].v[1]));
    if (it != edgeMap_.end() && it->second == e) edgeMap_.erase(it);
}

void Mesh::ensureAdjacency() const {
    if (!adjDirty_ && vEdges_.size() == vertices.size() && eFaces_.size() == edges.size()) return;
    vEdges_.assign(vertices.size(), {});
    eFaces_.assign(edges.size(), {});
    for (Id e = 0; e < (Id)edges.size(); ++e) {
        if (!edges[e].alive) continue;
        vEdges_[edges[e].v[0]].push_back(e);
        vEdges_[edges[e].v[1]].push_back(e);
    }
    for (Id f = 0; f < (Id)faces.size(); ++f) {
        const Face& F = faces[f];
        if (!F.alive) continue;
        auto addLoop = [&](const std::vector<Id>& L) {
            for (size_t i = 0; i < L.size(); ++i) {
                Id e = findEdge(L[i], L[(i + 1) % L.size()]);
                if (e == kNone) continue;
                auto& lst = eFaces_[e];
                if (std::find(lst.begin(), lst.end(), f) == lst.end()) lst.push_back(f);
            }
        };
        addLoop(F.outer);
        for (const auto& h : F.holes) addLoop(h);
    }
    adjDirty_ = false;
}

const std::vector<Id>& Mesh::vertexEdges(Id v) const {
    ensureAdjacency();
    return vEdges_[v];
}

const std::vector<Id>& Mesh::edgeFaces(Id e) const {
    ensureAdjacency();
    return eFaces_[e];
}

std::vector<Id> Mesh::faceEdges(Id f) const {
    std::vector<Id> r;
    const Face& F = faces[f];
    auto addLoop = [&](const std::vector<Id>& L) {
        for (size_t i = 0; i < L.size(); ++i) {
            Id e = findEdge(L[i], L[(i + 1) % L.size()]);
            if (e != kNone) r.push_back(e);
        }
    };
    addLoop(F.outer);
    for (const auto& h : F.holes) addLoop(h);
    return r;
}

std::vector<Id> Mesh::faceVertices(Id f) const {
    std::vector<Id> r = faces[f].outer;
    for (const auto& h : faces[f].holes) r.insert(r.end(), h.begin(), h.end());
    return r;
}

Plane Mesh::facePlane(Id f) const {
    const Face& F = faces[f];
    Vec3 c;
    for (Id v : F.outer) c += pos(v);
    c = c / double(std::max<size_t>(1, F.outer.size()));
    return Plane(F.normal, c);
}

double Mesh::faceArea(Id f) const {
    const Face& F = faces[f];
    double a = loopNewell(*this, F.outer).length() * 0.5;
    for (const auto& h : F.holes) a -= loopNewell(*this, h).length() * 0.5;
    return std::max(0.0, a);
}

double Mesh::edgeLength(Id e) const { return (pos(edges[e].v[1]) - pos(edges[e].v[0])).length(); }

Vec3 Mesh::faceInteriorPoint(Id f) const {
    const Face& F = faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    std::vector<std::vector<Vec2>> loops;
    std::vector<Vec3> flat;
    auto add = [&](const std::vector<Id>& L) {
        std::vector<Vec2> l;
        for (Id id : L) {
            l.emplace_back(pos(id).dot(u), pos(id).dot(v));
            flat.push_back(pos(id));
        }
        loops.push_back(std::move(l));
    };
    add(F.outer);
    for (const auto& h : F.holes) add(h);
    auto tris = triangulatePolygon(loops);
    std::vector<Vec2> all;
    for (const auto& l : loops) all.insert(all.end(), l.begin(), l.end());
    double best = -1;
    Vec3 res = flat.empty() ? Vec3() : flat[0];
    for (const auto& t : tris) {
        double a = std::fabs((all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]));
        if (a > best) {
            best = a;
            res = (flat[t[0]] + flat[t[1]] + flat[t[2]]) / 3.0;
        }
    }
    return res;
}

bool Mesh::pointInFace(Id f, const Vec3& p, bool includeBoundary) const {
    const Face& F = faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    Vec2 q(p.dot(u), p.dot(v));
    auto loop2 = [&](const std::vector<Id>& L) {
        std::vector<Vec2> r;
        r.reserve(L.size());
        for (Id id : L) r.emplace_back(pos(id).dot(u), pos(id).dot(v));
        return r;
    };
    std::vector<std::vector<Vec2>> loops;
    loops.push_back(loop2(F.outer));
    for (const auto& h : F.holes) loops.push_back(loop2(h));
    for (const auto& l : loops)
        for (size_t i = 0; i < l.size(); ++i)
            if (segDist2D(l[i], l[(i + 1) % l.size()], q) <= kTol) return includeBoundary;
    auto inside = [&](const std::vector<Vec2>& l) {
        return pointInPolygon2D((int)l.size(), [&](int i) { return l[i]; }, q);
    };
    if (!inside(loops[0])) return false;
    for (size_t i = 1; i < loops.size(); ++i)
        if (inside(loops[i])) return false;
    return true;
}

bool Mesh::rayFace(Id f, const Ray& r, double& t) const {
    Plane P = facePlane(f);
    if (!intersectRayPlane(r, P, t)) return false;
    return pointInFace(f, r.at(t), true);
}

int Mesh::faceEdgeDirection(Id f, Id a, Id b) const {
    const Face& F = faces[f];
    auto check = [&](const std::vector<Id>& L) {
        for (size_t i = 0; i < L.size(); ++i) {
            Id x = L[i], y = L[(i + 1) % L.size()];
            if (x == a && y == b) return 1;
            if (x == b && y == a) return -1;
        }
        return 0;
    };
    int d = check(F.outer);
    if (d) return d;
    for (const auto& h : F.holes)
        if ((d = check(h))) return d;
    return 0;
}

bool Mesh::isFreeFace(Id f) const {
    for (Id e : faceEdges(f)) {
        for (Id g : edgeFaces(e))
            if (g != f && faces[g].alive) return false;
    }
    return true;
}

int Mesh::aliveVertexCount() const {
    int n = 0;
    for (const auto& v : vertices) n += v.alive;
    return n;
}
int Mesh::aliveEdgeCount() const {
    int n = 0;
    for (const auto& e : edges) n += e.alive;
    return n;
}
int Mesh::aliveFaceCount() const {
    int n = 0;
    for (const auto& f : faces) n += f.alive;
    return n;
}

void Mesh::bounds(Vec3& mn, Vec3& mx, bool& any) const {
    any = false;
    mn = Vec3(1e300, 1e300, 1e300);
    mx = Vec3(-1e300, -1e300, -1e300);
    for (const auto& e : edges) {
        if (!e.alive) continue;
        for (Id v : e.v) {
            const Vec3& p = pos(v);
            for (int i = 0; i < 3; ++i) {
                mn[i] = std::min(mn[i], p[i]);
                mx[i] = std::max(mx[i], p[i]);
            }
            any = true;
        }
    }
}

FaceSnapshot Mesh::snapshot(Id f) const {
    FaceSnapshot s;
    const Face& F = faces[f];
    auto add = [&](const std::vector<Id>& L) {
        std::vector<Vec3> l;
        for (Id v : L) l.push_back(pos(v));
        s.loops.push_back(std::move(l));
    };
    add(F.outer);
    for (const auto& h : F.holes) add(h);
    s.normal = F.normal;
    s.front = F.front;
    s.back = F.back;
    s.hidden = F.hidden;
    s.area = faceArea(f);
    return s;
}

bool Mesh::snapshotContains(const FaceSnapshot& s, const Vec3& p) {
    Vec3 u, v;
    planeBasis(s.normal, u, v);
    Vec2 q(p.dot(u), p.dot(v));
    for (size_t i = 0; i < s.loops.size(); ++i) {
        auto l = project2D(s.loops[i], u, v);
        bool in = pointInPolygon2D((int)l.size(), [&](int k) { return l[k]; }, q);
        if (i == 0 && !in) return false;
        if (i > 0 && in) return false;
    }
    return !s.loops.empty();
}

// ---------------------------------------------------------------------------
// Low-level construction
// ---------------------------------------------------------------------------

Id Mesh::addVertex(const Vec3& p) {
    Id v = findVertex(p);
    if (v != kNone) return v;
    return addVertexRaw(p);
}

Id Mesh::addVertexRaw(const Vec3& p) {
    bool inc = adjacencyInSync();
    Vertex vx;
    vx.p = p;
    vertices.push_back(vx);
    Id id = (Id)vertices.size() - 1;
    gridInsert(id);
    if (inc) vEdges_.push_back({});
    else adjDirty_ = true;
    revision = nextMeshRevision();
    return id;
}

Id Mesh::addEdgeRaw(Id a, Id b, Id curve) {
    if (a == b || a == kNone || b == kNone) return kNone;
    Id ex = findEdge(a, b);
    if (ex != kNone) return ex;
    bool inc = adjacencyInSync();
    Edge e;
    e.v[0] = a;
    e.v[1] = b;
    e.curve = curve;
    edges.push_back(e);
    Id id = (Id)edges.size() - 1;
    edgeMapInsert(id);
    if (inc) {
        vEdges_[a].push_back(id);
        vEdges_[b].push_back(id);
        eFaces_.push_back({});
    } else {
        adjDirty_ = true;
    }
    revision = nextMeshRevision();
    return id;
}

Id Mesh::addFaceRaw(const std::vector<Id>& outerIn, const std::vector<std::vector<Id>>& holesIn,
                    const Vec3& normalHint) {
    if (outerIn.size() < 3) return kNone;
    std::vector<Id> outer = outerIn;
    Vec3 nn = loopNewell(*this, outer);
    if (nn.length() < 1e-14) return kNone;
    if (normalHint.length2() > 0 && nn.dot(normalHint) < 0) {
        std::reverse(outer.begin(), outer.end());
        nn = -nn;
    }
    Vec3 n = nn.normalized();
    std::vector<std::vector<Id>> holes;
    for (const auto& hIn : holesIn) {
        if (hIn.size() < 3) continue;
        std::vector<Id> h = hIn;
        if (loopNewell(*this, h).dot(n) > 0) std::reverse(h.begin(), h.end());
        holes.push_back(std::move(h));
    }
    auto ensureEdges = [&](const std::vector<Id>& L) {
        for (size_t i = 0; i < L.size(); ++i) addEdgeRaw(L[i], L[(i + 1) % L.size()]);
    };
    ensureEdges(outer);
    for (const auto& h : holes) ensureEdges(h);

    Face F;
    F.outer = std::move(outer);
    F.holes = std::move(holes);
    F.normal = n;
    faces.push_back(std::move(F));
    Id id = (Id)faces.size() - 1;
    if (adjacencyInSync()) {
        for (Id e : faceEdges(id)) eFaces_[e].push_back(id);
    } else {
        adjDirty_ = true;
    }
    revision = nextMeshRevision();
    return id;
}

// ---------------------------------------------------------------------------
// Drawing lines with automatic intersection and face inference
// ---------------------------------------------------------------------------

void Mesh::splitEdgesThrough(Id v) {
    const Vec3 P = pos(v);
    Id n = (Id)edges.size();
    for (Id e = 0; e < n; ++e) {
        if (!edges[e].alive || edges[e].has(v)) continue;
        const Vec3& a = pos(edges[e].v[0]);
        const Vec3& b = pos(edges[e].v[1]);
        // Quick reject by bounding box.
        bool out = false;
        for (int i = 0; i < 3 && !out; ++i)
            out = P[i] < std::min(a[i], b[i]) - kTol || P[i] > std::max(a[i], b[i]) + kTol;
        if (out) continue;
        double len = (b - a).length();
        if (len < kTol) continue;
        double t = closestParamOnSegment(a, b, P);
        double eps = kTol / len;
        if (t <= eps || t >= 1 - eps) continue;
        if ((lerp(a, b, t) - P).length() <= kTol) splitEdge(e, v);
    }
}

void Mesh::splitEdge(Id e, Id v) {
    Id a = edges[e].v[0], b = edges[e].v[1];
    if (v == a || v == b) return;
    Edge copy = edges[e];
    Id eav = findEdge(a, v), evb = findEdge(v, b);
    edgeMapErase(e);
    if (eav == kNone) {
        edges[e].v[1] = v;
        edgeMapInsert(e);
    } else {
        edges[e].alive = false;
    }
    if (evb == kNone) {
        Edge ne = copy;
        ne.v[0] = v;
        ne.v[1] = b;
        edges.push_back(ne);
        edgeMapInsert((Id)edges.size() - 1);
    }
    for (Face& F : faces) {
        if (!F.alive) continue;
        auto fix = [&](std::vector<Id>& L) {
            for (size_t i = 0; i < L.size(); ++i) {
                Id x = L[i], y = L[(i + 1) % L.size()];
                if ((x == a && y == b) || (x == b && y == a)) {
                    L.insert(L.begin() + (i + 1), v);
                    return;
                }
            }
        };
        fix(F.outer);
        for (auto& h : F.holes) fix(h);
    }
    adjDirty_ = true;
    revision = nextMeshRevision();
}

void Mesh::insertSegment(const Vec3& pa, const Vec3& pb, Id curve, std::vector<Id>& created,
                         std::vector<Id>& lineEdges) {
    Id va = addVertex(pa);
    Id vb = addVertex(pb);
    if (va == vb) return;
    splitEdgesThrough(va);
    splitEdgesThrough(vb);

    const Vec3 A = pos(va), B = pos(vb), D = B - A;
    const double len = D.length();
    const double eps1 = kTol / len;
    std::vector<std::pair<double, Id>> params = {{0.0, va}, {1.0, vb}};

    Id ne = (Id)edges.size();
    for (Id e = 0; e < ne; ++e) {
        if (!edges[e].alive || edges[e].has(va) || edges[e].has(vb)) continue;
        const Vec3 P1 = pos(edges[e].v[0]), P2 = pos(edges[e].v[1]);
        bool out = false;
        for (int i = 0; i < 3 && !out; ++i)
            out = std::max(P1[i], P2[i]) < std::min(A[i], B[i]) - kTol ||
                  std::min(P1[i], P2[i]) > std::max(A[i], B[i]) + kTol;
        if (out) continue;
        double s, t;
        Vec3 E = P2 - P1;
        double elen = E.length();
        if (elen < kTol) continue;
        if (!closestLineLine(A, D, P1, E, s, t)) continue;
        double eps2 = kTol / elen;
        if (s <= eps1 || s >= 1 - eps1 || t <= eps2 || t >= 1 - eps2) continue;
        Vec3 Q1 = A + D * s, Q2 = P1 + E * t;
        if ((Q1 - Q2).length() > kTol) continue;
        Id w = addVertex(Q2);
        if (!edges[e].has(w)) splitEdge(e, w);
        params.emplace_back(s, w);
    }
    for (Id w = 0; w < (Id)vertices.size(); ++w) {
        if (!vertices[w].alive || w == va || w == vb) continue;
        const Vec3& P = vertices[w].p;
        bool out = false;
        for (int i = 0; i < 3 && !out; ++i)
            out = P[i] < std::min(A[i], B[i]) - kTol || P[i] > std::max(A[i], B[i]) + kTol;
        if (out) continue;
        double t = closestParamOnSegment(A, B, P);
        if (t <= eps1 || t >= 1 - eps1) continue;
        if ((lerp(A, B, t) - P).length() <= kTol) params.emplace_back(t, w);
    }
    std::sort(params.begin(), params.end());
    std::vector<Id> chain;
    for (const auto& pr : params)
        if (std::find(chain.begin(), chain.end(), pr.second) == chain.end()) chain.push_back(pr.second);
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        Id u = chain[i], w = chain[i + 1];
        Id e = findEdge(u, w);
        if (e == kNone) {
            e = addEdgeRaw(u, w, curve);
            created.push_back(e);
        }
        lineEdges.push_back(e);
    }
}

std::vector<Id> Mesh::drawSegments(const std::vector<std::pair<Vec3, Vec3>>& segs, Id curve) {
    std::vector<Id> created, lineEdges;
    for (const auto& s : segs) {
        if (s.first.nearlyEquals(s.second)) continue;
        insertSegment(s.first, s.second, curve, created, lineEdges);
    }
    // Re-tracing existing edges also re-runs face finding (SketchUp heals faces
    // when an edge is traced over), so infer from every edge along the lines.
    std::vector<Id> all = lineEdges;
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    inferFaces(all);
    std::vector<Id> alive;
    for (Id e : lineEdges)
        if (e >= 0 && edges[e].alive) alive.push_back(e);
    return alive;
}

std::vector<Id> Mesh::drawLine(const Vec3& a, const Vec3& b, Id curve) { return drawSegments({{a, b}}, curve); }

std::vector<Id> Mesh::drawPolyline(const std::vector<Vec3>& pts, bool closed, Id curve) {
    std::vector<std::pair<Vec3, Vec3>> segs;
    for (size_t i = 0; i + 1 < pts.size(); ++i) segs.emplace_back(pts[i], pts[i + 1]);
    if (closed && pts.size() > 2) segs.emplace_back(pts.back(), pts.front());
    return drawSegments(segs, curve);
}

// ---------------------------------------------------------------------------
// Face inference
// ---------------------------------------------------------------------------

void Mesh::inferFaces(const std::vector<Id>& newEdges) {
    std::vector<Plane> planes;
    auto addPlane = [&](const Plane& pIn) {
        Plane p = pIn.canonical();
        for (const Plane& q : planes)
            if (q.coincident(p)) return;
        planes.push_back(p);
    };
    for (Id e : newEdges) {
        if (e < 0 || !edges[e].alive) continue;
        const Vec3 A = pos(edges[e].v[0]), B = pos(edges[e].v[1]);
        const Vec3 D = B - A;
        for (Id x : edges[e].v) {
            std::vector<Id> adj = vertexEdges(x);
            for (Id e2 : adj) {
                if (e2 == e || !edges[e2].alive) continue;
                Vec3 Y = pos(edges[e2].other(x));
                Vec3 AY = Y - A;
                Vec3 n = D.cross(AY);
                if (n.length() <= 1e-7 * D.length() * AY.length()) continue; // collinear
                addPlane(Plane(n, A));
            }
        }
    }
    std::set<Id> newSet(newEdges.begin(), newEdges.end());
    for (const Plane& P : planes) {
        std::set<Id> inPlane;
        for (Id e : newSet)
            if (e >= 0 && edges[e].alive && P.contains(pos(edges[e].v[0])) && P.contains(pos(edges[e].v[1])))
                inPlane.insert(e);
        rebuildPlane(P, inPlane, {});
    }
}

std::vector<Mesh::Region> Mesh::computeRegions(const Plane& P, const std::vector<Id>& planeEdges) const {
    Vec3 u, v;
    planeBasis(P.n, u, v);

    std::unordered_map<Id, std::vector<Id>> adj;
    for (Id e : planeEdges) {
        Id a = edges[e].v[0], b = edges[e].v[1];
        if (a == b) continue;
        auto& la = adj[a];
        if (std::find(la.begin(), la.end(), b) == la.end()) la.push_back(b);
        auto& lb = adj[b];
        if (std::find(lb.begin(), lb.end(), a) == lb.end()) lb.push_back(a);
    }
    // Iteratively prune dangling edges: they never bound a face.
    std::vector<Id> stack;
    for (const auto& kv : adj)
        if (kv.second.size() < 2) stack.push_back(kv.first);
    while (!stack.empty()) {
        Id x = stack.back();
        stack.pop_back();
        auto it = adj.find(x);
        if (it == adj.end() || it->second.size() >= 2) continue;
        for (Id n : it->second) {
            auto jt = adj.find(n);
            if (jt == adj.end()) continue;
            auto& nl = jt->second;
            nl.erase(std::remove(nl.begin(), nl.end(), x), nl.end());
            if (nl.size() < 2) stack.push_back(n);
        }
        adj.erase(it);
    }
    if (adj.empty()) return {};

    std::unordered_map<Id, Vec2> p2;
    for (const auto& kv : adj) p2[kv.first] = Vec2(pos(kv.first).dot(u), pos(kv.first).dot(v));
    for (auto& kv : adj) {
        const Vec2 c = p2[kv.first];
        std::sort(kv.second.begin(), kv.second.end(), [&](Id a, Id b) {
            Vec2 da = p2[a] - c, db = p2[b] - c;
            return std::atan2(da.y, da.x) < std::atan2(db.y, db.x);
        });
    }

    auto dkey = [](Id a, Id b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
    std::unordered_set<uint64_t> visited;
    std::vector<std::vector<Id>> walks;
    size_t halfEdges = 0;
    for (const auto& kv : adj) halfEdges += kv.second.size();

    // Deterministic iteration order.
    std::vector<Id> verts;
    for (const auto& kv : adj) verts.push_back(kv.first);
    std::sort(verts.begin(), verts.end());

    for (Id s : verts) {
        for (Id n : adj.at(s)) {
            if (visited.count(dkey(s, n))) continue;
            std::vector<Id> w;
            Id a = s, b = n;
            size_t guard = 0;
            do {
                visited.insert(dkey(a, b));
                w.push_back(a);
                const auto& nb = adj.at(b);
                size_t idx = std::find(nb.begin(), nb.end(), a) - nb.begin();
                Id c = nb[(idx + nb.size() - 1) % nb.size()];
                a = b;
                b = c;
            } while (!(a == s && b == n) && ++guard <= halfEdges);
            walks.push_back(std::move(w));
        }
    }

    // Split walks at edges traversed in both directions (bridges to holes).
    struct W {
        std::vector<Id> v;
        double area;
        int origin;
    };
    std::vector<W> pos_, neg_;
    std::vector<bool> originHasPositive(walks.size(), false);
    const double tinyArea = kTol * kTol * 0.1;
    for (size_t wi = 0; wi < walks.size(); ++wi) {
        std::vector<std::vector<Id>> st = {walks[wi]};
        while (!st.empty()) {
            std::vector<Id> cur = std::move(st.back());
            st.pop_back();
            int k = (int)cur.size();
            std::unordered_map<uint64_t, int> seen;
            bool split = false;
            for (int i = 0; i < k && !split; ++i) {
                Id a = cur[i], b = cur[(i + 1) % k];
                uint64_t key = edgeKey(a, b);
                auto it = seen.find(key);
                if (it != seen.end()) {
                    int j = it->second;
                    if (cur[j] == b && cur[(j + 1) % k] == a) {
                        std::vector<Id> s1(cur.begin() + j + 1, cur.begin() + i);
                        std::vector<Id> s2;
                        for (int q = i + 1; q < k; ++q) s2.push_back(cur[q]);
                        for (int q = 0; q < j; ++q) s2.push_back(cur[q]);
                        if (s1.size() >= 3) st.push_back(std::move(s1));
                        if (s2.size() >= 3) st.push_back(std::move(s2));
                        split = true;
                    }
                } else {
                    seen[key] = i;
                }
            }
            if (split) continue;
            if (cur.size() < 3) continue;
            double area = signedArea2D((int)cur.size(), [&](int i) { return p2[cur[i]]; });
            if (area > tinyArea) {
                pos_.push_back({cur, area, (int)wi});
                originHasPositive[wi] = true;
            } else if (area < -tinyArea) {
                neg_.push_back({cur, area, (int)wi});
            }
        }
    }

    std::vector<Region> regions;
    for (const W& p : pos_) {
        Region r;
        r.outer = p.v;
        r.area = p.area;
        for (const W& n : neg_)
            if (n.origin == p.origin) r.holes.push_back(n.v);
        regions.push_back(std::move(r));
    }
    // Disconnected components nested inside a region become holes of the
    // smallest region containing them.
    for (const W& n : neg_) {
        if (originHasPositive[n.origin]) continue;
        std::unordered_set<Id> nv(n.v.begin(), n.v.end());
        Vec2 test = p2[n.v[0]];
        int best = -1;
        for (size_t i = 0; i < pos_.size(); ++i) {
            const W& p = pos_[i];
            bool shares = false;
            for (Id id : p.v)
                if (nv.count(id)) { shares = true; break; }
            if (shares) continue;
            if (!pointInPolygon2D((int)p.v.size(), [&](int k) { return p2[p.v[k]]; }, test)) continue;
            if (best < 0 || p.area < pos_[best].area) best = (int)i;
        }
        if (best >= 0) regions[best].holes.push_back(n.v);
    }
    return regions;
}

Vec3 Mesh::regionInteriorPoint(const Plane& P, const Region& r) const {
    Vec3 u, v;
    planeBasis(P.n, u, v);
    std::vector<std::vector<Vec2>> loops;
    auto add = [&](const std::vector<Id>& L) {
        std::vector<Vec2> l;
        for (Id id : L) l.emplace_back(pos(id).dot(u), pos(id).dot(v));
        loops.push_back(std::move(l));
    };
    add(r.outer);
    for (const auto& h : r.holes) add(h);
    auto tris = triangulatePolygon(loops);
    std::vector<Vec2> all;
    for (const auto& l : loops) all.insert(all.end(), l.begin(), l.end());
    double best = -1;
    Vec2 c = all.empty() ? Vec2() : all[0];
    for (const auto& t : tris) {
        double a = std::fabs((all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]));
        if (a > best) {
            best = a;
            c = (all[t[0]] + all[t[1]] + all[t[2]]) * (1.0 / 3.0);
        }
    }
    return P.n * P.d + u * c.x + v * c.y;
}

Vec3 Mesh::chooseNormal(const std::vector<Id>& outer, const Plane& P) const {
    Vec3 n = P.n;
    // 1. Consistent orientation with neighbouring faces.
    for (size_t i = 0; i < outer.size(); ++i) {
        Id a = outer[i], b = outer[(i + 1) % outer.size()];
        Id e = findEdge(a, b);
        if (e == kNone) continue;
        for (Id f : edgeFaces(e)) {
            if (!faces[f].alive) continue;
            int d = faceEdgeDirection(f, a, b);
            if (d == 1) return -n;
            if (d == -1) return n;
        }
    }
    // 2. Faces on the ground plane face down (so push/pull up makes outward normals).
    if (std::fabs(n.z) > 0.999 && std::fabs(P.d) <= kTol) return Vec3(0, 0, -1);
    // 3. Face the camera.
    Vec3 c = pos(outer[0]);
    return (viewHint - c).dot(n) >= 0 ? n : -n;
}

void Mesh::rebuildPlane(const Plane& P, const std::set<Id>& newEdges, std::vector<FaceSnapshot> dirty) {
    std::vector<Id> pe;
    for (Id e = 0; e < (Id)edges.size(); ++e) {
        if (!edges[e].alive) continue;
        if (P.contains(pos(edges[e].v[0])) && P.contains(pos(edges[e].v[1]))) pe.push_back(e);
    }
    std::vector<Region> regions = computeRegions(P, pe);

    std::map<LoopKey, int> regionByKey;
    for (size_t i = 0; i < regions.size(); ++i)
        regionByKey[canonicalKey(regions[i].outer, regions[i].holes)] = (int)i;
    std::vector<bool> exists(regions.size(), false);

    for (Id f = 0; f < (Id)faces.size(); ++f) {
        const Face& F = faces[f];
        if (!F.alive) continue;
        if (std::fabs(F.normal.dot(P.n)) < 1 - 1e-6) continue;
        bool coplanar = true;
        for (Id v : faceVertices(f))
            if (!P.contains(pos(v))) { coplanar = false; break; }
        if (!coplanar) continue;
        auto it = regionByKey.find(canonicalKey(F.outer, F.holes));
        if (it != regionByKey.end()) {
            exists[it->second] = true;
            continue;
        }
        bool touched = false;
        for (Id e : newEdges) {
            const Vec3 a = pos(edges[e].v[0]), b = pos(edges[e].v[1]);
            if (pointInFace(f, (a + b) * 0.5, true) || pointInFace(f, a, true) || pointInFace(f, b, true)) {
                touched = true;
                break;
            }
        }
        if (!touched) continue;
        dirty.push_back(snapshot(f));
        faces[f].alive = false;
        adjDirty_ = true;
        revision = nextMeshRevision();
    }

    for (size_t i = 0; i < regions.size(); ++i) {
        if (exists[i]) continue;
        const Region& r = regions[i];
        bool touchesNew = false;
        auto checkLoop = [&](const std::vector<Id>& L) {
            for (size_t k = 0; k < L.size() && !touchesNew; ++k) {
                Id e = findEdge(L[k], L[(k + 1) % L.size()]);
                if (e != kNone && newEdges.count(e)) touchesNew = true;
            }
        };
        checkLoop(r.outer);
        for (const auto& h : r.holes) checkLoop(h);

        Vec3 sp = regionInteriorPoint(P, r);
        const FaceSnapshot* parent = nullptr;
        for (const FaceSnapshot& s : dirty)
            if (snapshotContains(s, sp) && (!parent || s.area < parent->area)) parent = &s;
        if (!touchesNew && !parent) continue;

        Vec3 hint = parent ? parent->normal : chooseNormal(r.outer, P);
        Id nf = addFaceRaw(r.outer, r.holes, hint);
        if (nf != kNone && parent) {
            faces[nf].front = parent->front;
            faces[nf].back = parent->back;
            faces[nf].hidden = parent->hidden;
        }
    }
}

// ---------------------------------------------------------------------------
// Erasing
// ---------------------------------------------------------------------------

void Mesh::eraseFace(Id f) {
    if (f < 0 || !faces[f].alive) return;
    faces[f].alive = false;
    adjDirty_ = true;
    revision = nextMeshRevision();
}

void Mesh::eraseEdge(Id e, bool heal) {
    if (e < 0 || !edges[e].alive) return;
    std::vector<Id> fs;
    for (Id f : edgeFaces(e))
        if (faces[f].alive) fs.push_back(f);
    Id a = edges[e].v[0], b = edges[e].v[1];

    if (heal && fs.size() == 2) {
        Plane p1 = facePlane(fs[0]), p2 = facePlane(fs[1]);
        if (p1.coincident(p2, kTol * 10) && faces[fs[0]].normal.dot(faces[fs[1]].normal) > 0) {
            std::vector<FaceSnapshot> snaps = {snapshot(fs[0]), snapshot(fs[1])};
            faces[fs[0]].alive = faces[fs[1]].alive = false;
            edgeMapErase(e);
            edges[e].alive = false;
            adjDirty_ = true;
            revision = nextMeshRevision();
            rebuildPlane(p1, {}, snaps);
            removeOrphanVertices();
            if (vertices[a].alive) mergeCollinearAt(a);
            if (vertices[b].alive) mergeCollinearAt(b);
            return;
        }
    }
    for (Id f : fs) faces[f].alive = false;
    edgeMapErase(e);
    edges[e].alive = false;
    adjDirty_ = true;
    revision = nextMeshRevision();
    removeOrphanVertices();
}

void Mesh::removeOrphanVertices() {
    std::vector<int> deg(vertices.size(), 0);
    for (const Edge& e : edges)
        if (e.alive) { deg[e.v[0]]++; deg[e.v[1]]++; }
    bool any = false;
    for (size_t v = 0; v < vertices.size(); ++v)
        if (vertices[v].alive && deg[v] == 0) { vertices[v].alive = false; any = true; }
    if (any) {
        adjDirty_ = true;
        revision = nextMeshRevision();
    }
}

void Mesh::reverseFace(Id f) {
    Face& F = faces[f];
    std::reverse(F.outer.begin(), F.outer.end());
    for (auto& h : F.holes) std::reverse(h.begin(), h.end());
    F.normal = -F.normal;
    std::swap(F.front, F.back);
    revision = nextMeshRevision();
}

bool Mesh::mergeCollinearAt(Id v) {
    std::vector<Id> ve;
    for (Id e : vertexEdges(v))
        if (edges[e].alive) ve.push_back(e);
    if (ve.size() != 2) return false;
    Id e1 = ve[0], e2 = ve[1];
    Id a = edges[e1].other(v), b = edges[e2].other(v);
    if (a == b || findEdge(a, b) != kNone) return false;
    Vec3 da = (pos(a) - pos(v)).normalized(), db = (pos(b) - pos(v)).normalized();
    if (da.dot(db) > -1 + 1e-9) return false;
    std::vector<Id> f1, f2;
    for (Id f : edgeFaces(e1)) if (faces[f].alive) f1.push_back(f);
    for (Id f : edgeFaces(e2)) if (faces[f].alive) f2.push_back(f);
    std::sort(f1.begin(), f1.end());
    std::sort(f2.begin(), f2.end());
    if (f1 != f2) return false;

    edgeMapErase(e1);
    edgeMapErase(e2);
    edges[e1].v[0] = a;
    edges[e1].v[1] = b;
    edges[e2].alive = false;
    edgeMapInsert(e1);
    for (Id f : f1) {
        auto strip = [&](std::vector<Id>& L) { L.erase(std::remove(L.begin(), L.end(), v), L.end()); };
        strip(faces[f].outer);
        for (auto& h : faces[f].holes) strip(h);
    }
    vertices[v].alive = false;
    adjDirty_ = true;
    revision = nextMeshRevision();
    return true;
}

// ---------------------------------------------------------------------------
// Push / Pull
// ---------------------------------------------------------------------------

Id Mesh::pushPull(Id f, double dist, bool keepOriginal) {
    if (f < 0 || !faces[f].alive || std::fabs(dist) < kTol) return f;
    const Face F = faces[f];
    const Vec3 n = F.normal;
    const Vec3 off = n * dist;
    std::vector<std::vector<Id>> loops = {F.outer};
    for (const auto& h : F.holes) loops.push_back(h);

    // Stretch mode: every boundary edge has exactly one neighbour face whose
    // plane contains the push direction -> just move the face (box gets taller
    // or shorter without new geometry).
    if (!keepOriginal) {
        bool stretch = true;
        for (Id e : faceEdges(f)) {
            int others = 0;
            for (Id g : edgeFaces(e)) {
                if (g == f || !faces[g].alive) continue;
                ++others;
                if (std::fabs(faces[g].normal.dot(n)) > 1e-6) stretch = false;
            }
            if (others != 1) stretch = false;
            if (!stretch) break;
        }
        if (stretch) {
            std::set<Id> vs;
            for (const auto& L : loops) vs.insert(L.begin(), L.end());
            transformVertices(vs, Mat4::translation(off), true);
            return f;
        }
    }

    const bool isFree = keepOriginal || isFreeFace(f);
    std::unordered_map<Id, Id> vm;
    for (const auto& L : loops)
        for (Id v : L) vm[v] = addVertex(pos(v) + off);
    // New vertices landing on existing edges (e.g. the far side of a wall) split
    // them so the new faces share topology with their neighbours.
    for (const auto& kv : vm) splitEdgesThrough(kv.second);

    std::map<Id, Id> curveMap;
    auto mapCurve = [&](Id c) {
        if (c == kNone) return kNone;
        auto it = curveMap.find(c);
        if (it != curveMap.end()) return it->second;
        Id nc = newCurveId();
        curveMap[c] = nc;
        return nc;
    };

    std::vector<std::pair<Id, Id>> oldEdges;
    std::vector<Id> created;
    const double sgn = isFree ? 1.0 : (dist > 0 ? 1.0 : -1.0);
    for (const auto& L : loops) {
        size_t k = L.size();
        for (size_t i = 0; i < k; ++i) {
            Id a = L[i], b = L[(i + 1) % k], p = L[(i + k - 1) % k];
            Id a2 = vm[a], b2 = vm[b];
            oldEdges.emplace_back(a, b);
            Vec3 t = pos(b) - pos(a);
            Vec3 sideN = t.cross(n) * sgn;
            Id eab = findEdge(a, b), epa = findEdge(p, a);
            Edge eabCopy = eab != kNone ? edges[eab] : Edge();
            created.push_back(addFaceRaw({a, b, b2, a2}, {}, sideN));
            // Top edge inherits the curve/soft flags of the bottom edge.
            Id top = findEdge(a2, b2);
            if (top != kNone && eab != kNone) {
                edges[top].curve = mapCurve(eabCopy.curve);
                edges[top].soft = eabCopy.soft;
                edges[top].smooth = eabCopy.smooth;
            }
            // Vertical edge between two segments of the same curve is soft.
            Id vert = findEdge(a, a2);
            if (vert != kNone && eab != kNone && epa != kNone && edges[eab].curve != kNone &&
                edges[eab].curve == edges[epa].curve) {
                edges[vert].soft = true;
                edges[vert].smooth = true;
            }
        }
    }

    std::vector<Id> newOuter;
    for (Id v : F.outer) newOuter.push_back(vm[v]);
    std::vector<std::vector<Id>> newHoles;
    for (const auto& h : F.holes) {
        std::vector<Id> nh;
        for (Id v : h) nh.push_back(vm[v]);
        newHoles.push_back(std::move(nh));
    }
    bool flipMoved = isFree && dist < 0;
    Id moved = addFaceRaw(newOuter, newHoles, flipMoved ? -n : n);
    if (moved != kNone) {
        FaceSide fr = F.front, bk = F.back;
        if (fr.tex.valid) fr.tex.origin += off;
        if (bk.tex.valid) bk.tex.origin += off;
        if (flipMoved) std::swap(fr, bk);
        faces[moved].front = fr;
        faces[moved].back = bk;
    }
    if (isFree) {
        if (dist > 0) reverseFace(f); // cap faces away from the extrusion
    } else {
        eraseFace(f);
    }

    // Heal: merge new side faces with coplanar neighbours.
    for (const auto& pr : oldEdges) {
        Id e = findEdge(pr.first, pr.second);
        if (e == kNone) continue;
        std::vector<Id> fs;
        for (Id g : edgeFaces(e))
            if (faces[g].alive) fs.push_back(g);
        if (fs.size() != 2) continue;
        const Face &A = faces[fs[0]], &B = faces[fs[1]];
        if (A.normal.dot(B.normal) < 1 - 1e-9) continue;
        if (A.front.material != B.front.material || A.back.material != B.back.material) continue;
        if (!facePlane(fs[0]).coincident(facePlane(fs[1]), kTol * 10)) continue;
        eraseEdge(e, true);
    }
    for (const auto& L : loops)
        for (Id v : L)
            if (vertices[v].alive) mergeCollinearAt(v);

    // New faces landing coplanar with an opposite-facing face cancel out where
    // they overlap: the moved face when pushed all the way through a solid, or
    // a side face sliding along a neighbour (the floor of a doorway). Cut the
    // plane along the new face's boundary and remove both coincident parts.
    created.push_back(moved);
    for (Id s : created) {
        if (s == kNone || !faces[s].alive) continue;
        const Plane sp = facePlane(s);
        const Vec3 sn = faces[s].normal;
        std::vector<FaceSnapshot> opposite;
        for (Id g = 0; g < (Id)faces.size(); ++g) {
            if (g == s || !faces[g].alive || faces[g].normal.dot(sn) > -1 + 1e-6) continue;
            if (facePlane(g).coincident(sp, kTol * 10)) opposite.push_back(snapshot(g));
        }
        if (opposite.empty()) continue;
        const FaceSnapshot ss = snapshot(s);
        std::vector<Id> se = faceEdges(s);
        rebuildPlane(sp, std::set<Id>(se.begin(), se.end()), {});
        std::vector<Id> loose;
        for (Id g = 0; g < (Id)faces.size(); ++g) {
            if (!faces[g].alive || std::fabs(faces[g].normal.dot(sn)) < 1 - 1e-6) continue;
            if (!facePlane(g).coincident(sp, kTol * 10)) continue;
            Vec3 p = faceInteriorPoint(g);
            if (!snapshotContains(ss, p)) continue;
            bool covered = false;
            for (const FaceSnapshot& o : opposite) covered = covered || snapshotContains(o, p);
            if (!covered) continue;
            std::vector<Id> ge = faceEdges(g);
            loose.insert(loose.end(), ge.begin(), ge.end());
            eraseFace(g);
        }
        // Edges that only bounded the cancelled area go away too.
        std::set<Id> ends;
        for (Id e : loose) {
            if (!edges[e].alive) continue;
            bool used = false;
            for (Id g : edgeFaces(e)) used = used || faces[g].alive;
            if (used) continue;
            ends.insert(edges[e].v[0]);
            ends.insert(edges[e].v[1]);
            edgeMapErase(e);
            edges[e].alive = false;
            adjDirty_ = true;
            revision = nextMeshRevision();
        }
        removeOrphanVertices();
        for (Id v : ends)
            if (vertices[v].alive) mergeCollinearAt(v);
    }
    if (moved != kNone && !faces[moved].alive) return kNone;
    return moved;
}

// ---------------------------------------------------------------------------
// Transforms, duplication, welding
// ---------------------------------------------------------------------------

void Mesh::recomputeNormal(Id f) {
    Vec3 nn = loopNewell(*this, faces[f].outer);
    if (nn.length() > 1e-14) faces[f].normal = nn.normalized();
}

void Mesh::transformVertices(const std::set<Id>& verts, const Mat4& m, bool weldAfter) {
    if (verts.empty()) return;
    // A mirroring transform flips loop winding; reverse the loops of fully
    // transformed faces so their front side stays on the same physical side.
    const bool mirror = m.det3() < 0;
    for (Id v : verts) vertices[v].p = m.transformPoint(vertices[v].p);
    for (Id f = 0; f < (Id)faces.size(); ++f) {
        Face& F = faces[f];
        if (!F.alive) continue;
        bool any = false, all = true;
        for (Id v : faceVertices(f)) {
            if (verts.count(v)) any = true;
            else all = false;
        }
        if (!any) continue;
        if (all) {
            for (FaceSide* s : {&F.front, &F.back}) {
                if (!s->tex.valid) continue;
                s->tex.origin = m.transformPoint(s->tex.origin);
                s->tex.u = transformCovector(m, s->tex.u);
                s->tex.v = transformCovector(m, s->tex.v);
            }
            if (mirror) {
                std::reverse(F.outer.begin(), F.outer.end());
                for (auto& h : F.holes) std::reverse(h.begin(), h.end());
            }
        }
        recomputeNormal(f);
    }
    markGeometryMoved();
    adjDirty_ = true;
    if (weldAfter) weld(verts);
}

void Mesh::duplicate(const std::set<Id>& faceIds, const std::set<Id>& edgeIds, std::set<Id>& newFaces,
                     std::set<Id>& newEdges, std::set<Id>& newVerts) {
    std::map<Id, Id> vm;
    auto mapV = [&](Id v) {
        auto it = vm.find(v);
        if (it != vm.end()) return it->second;
        Id nv = addVertexRaw(pos(v));
        vm[v] = nv;
        newVerts.insert(nv);
        return nv;
    };
    std::map<Id, Id> curveMap;
    auto mapCurve = [&](Id c) {
        if (c == kNone) return kNone;
        auto it = curveMap.find(c);
        if (it != curveMap.end()) return it->second;
        Id nc = newCurveId();
        curveMap[c] = nc;
        return nc;
    };
    std::set<Id> allEdges = edgeIds;
    for (Id f : faceIds)
        if (faces[f].alive)
            for (Id e : faceEdges(f)) allEdges.insert(e);
    for (Id e : allEdges) {
        if (!edges[e].alive) continue;
        Edge src = edges[e];
        Id ne = addEdgeRaw(mapV(src.v[0]), mapV(src.v[1]));
        if (ne == kNone) continue;
        edges[ne].soft = src.soft;
        edges[ne].smooth = src.smooth;
        edges[ne].hidden = src.hidden;
        edges[ne].curve = mapCurve(src.curve);
        newEdges.insert(ne);
    }
    for (Id f : faceIds) {
        if (!faces[f].alive) continue;
        Face src = faces[f];
        std::vector<Id> o;
        for (Id v : src.outer) o.push_back(mapV(v));
        std::vector<std::vector<Id>> hs;
        for (const auto& h : src.holes) {
            std::vector<Id> nh;
            for (Id v : h) nh.push_back(mapV(v));
            hs.push_back(std::move(nh));
        }
        Id nf = addFaceRaw(o, hs, src.normal);
        if (nf == kNone) continue;
        faces[nf].front = src.front;
        faces[nf].back = src.back;
        faces[nf].hidden = src.hidden;
        newFaces.insert(nf);
    }
}

void Mesh::weld(const std::set<Id>& candidates) {
    std::vector<Id> list;
    if (candidates.empty()) {
        for (Id v = 0; v < (Id)vertices.size(); ++v)
            if (vertices[v].alive) list.push_back(v);
    } else {
        list.assign(candidates.begin(), candidates.end());
    }
    gridDirty_ = true;
    ensureGrid();
    std::unordered_map<Id, Id> remap;
    for (Id v : list) {
        if (!vertices[v].alive) continue;
        const Vec3 p = vertices[v].p;
        Id best = kNone;
        int64_t cx = (int64_t)std::floor(p.x / kCell), cy = (int64_t)std::floor(p.y / kCell),
                cz = (int64_t)std::floor(p.z / kCell);
        for (int64_t x = cx - 1; x <= cx + 1; ++x)
            for (int64_t y = cy - 1; y <= cy + 1; ++y)
                for (int64_t z = cz - 1; z <= cz + 1; ++z) {
                    auto it = grid_.find(CellKey{x, y, z});
                    if (it == grid_.end()) continue;
                    for (Id w : it->second) {
                        if (w == v || !vertices[w].alive) continue;
                        if ((vertices[w].p - p).length2() <= kTol * kTol) {
                            if (best == kNone || w < best) best = w;
                        }
                    }
                }
        if (best != kNone) {
            remap[v] = best;
            vertices[v].alive = false;
        }
    }
    if (remap.empty()) return;
    auto R = [&](Id v) {
        auto it = remap.find(v);
        while (it != remap.end()) {
            v = it->second;
            it = remap.find(v);
        }
        return v;
    };
    edgeMap_.clear();
    for (Id e = 0; e < (Id)edges.size(); ++e) {
        Edge& E = edges[e];
        if (!E.alive) continue;
        E.v[0] = R(E.v[0]);
        E.v[1] = R(E.v[1]);
        if (E.v[0] == E.v[1]) { E.alive = false; continue; }
        uint64_t k = edgeKey(E.v[0], E.v[1]);
        auto it = edgeMap_.find(k);
        if (it != edgeMap_.end()) {
            Edge& keep = edges[it->second];
            keep.soft = keep.soft && E.soft;
            keep.smooth = keep.smooth && E.smooth;
            E.alive = false;
            continue;
        }
        edgeMap_[k] = e;
    }
    std::map<LoopKey, Id> seen;
    for (Id f = 0; f < (Id)faces.size(); ++f) {
        Face& F = faces[f];
        if (!F.alive) continue;
        auto fix = [&](std::vector<Id>& L) {
            for (Id& v : L) v = R(v);
            std::vector<Id> out;
            for (Id v : L)
                if (out.empty() || out.back() != v) out.push_back(v);
            while (out.size() > 1 && out.front() == out.back()) out.pop_back();
            L.swap(out);
        };
        fix(F.outer);
        for (auto& h : F.holes) fix(h);
        F.holes.erase(std::remove_if(F.holes.begin(), F.holes.end(), [](const auto& h) { return h.size() < 3; }),
                      F.holes.end());
        if (F.outer.size() < 3) { F.alive = false; continue; }
        LoopKey k = canonicalKey(F.outer, F.holes);
        if (seen.count(k)) { F.alive = false; continue; }
        seen[k] = f;
    }
    gridDirty_ = true;
    adjDirty_ = true;
    revision = nextMeshRevision();
}

void Mesh::compact(std::vector<Id>* vmapOut, std::vector<Id>* emapOut, std::vector<Id>* fmapOut) {
    std::vector<Id> vmap(vertices.size(), kNone), emap(edges.size(), kNone), fmap(faces.size(), kNone);
    std::vector<Vertex> nv;
    for (size_t i = 0; i < vertices.size(); ++i)
        if (vertices[i].alive) { vmap[i] = (Id)nv.size(); nv.push_back(vertices[i]); }
    std::vector<Edge> ne;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!edges[i].alive) continue;
        Edge e = edges[i];
        e.v[0] = vmap[e.v[0]];
        e.v[1] = vmap[e.v[1]];
        if (e.v[0] == kNone || e.v[1] == kNone) continue;
        emap[i] = (Id)ne.size();
        ne.push_back(e);
    }
    std::vector<Face> nf;
    for (size_t i = 0; i < faces.size(); ++i) {
        if (!faces[i].alive) continue;
        Face f = faces[i];
        bool ok = true;
        auto remapLoop = [&](std::vector<Id>& L) {
            for (Id& v : L) {
                v = vmap[v];
                if (v == kNone) ok = false;
            }
        };
        remapLoop(f.outer);
        for (auto& h : f.holes) remapLoop(h);
        if (!ok) continue;
        fmap[i] = (Id)nf.size();
        nf.push_back(std::move(f));
    }
    vertices.swap(nv);
    edges.swap(ne);
    faces.swap(nf);
    edgeMap_.clear();
    for (Id e = 0; e < (Id)edges.size(); ++e) edgeMap_[edgeKey(edges[e].v[0], edges[e].v[1])] = e;
    adjDirty_ = true;
    gridDirty_ = true;
    revision = nextMeshRevision();
    if (vmapOut) *vmapOut = std::move(vmap);
    if (emapOut) *emapOut = std::move(emap);
    if (fmapOut) *fmapOut = std::move(fmap);
}

// ---------------------------------------------------------------------------
// Texture coordinates
// ---------------------------------------------------------------------------

Vec2 faceTexCoord(const Face& f, bool frontSide, const Vec3& p, const Material* m) {
    const FaceSide& s = frontSide ? f.front : f.back;
    if (s.tex.valid) {
        Vec3 d = p - s.tex.origin;
        return {d.dot(s.tex.u), d.dot(s.tex.v)};
    }
    Vec3 n = frontSide ? f.normal : -f.normal;
    Vec3 u, v;
    planeBasis(n, u, v);
    double w = 1, h = 1;
    if (m && m->hasTexture()) {
        w = m->texture.worldWidth > 1e-9 ? m->texture.worldWidth : 1;
        h = m->texture.worldHeight > 1e-9 ? m->texture.worldHeight : 1;
    }
    return {p.dot(u) / w, p.dot(v) / h};
}

TexMap fitTextureToFace(const Mesh& mesh, Id f, bool frontSide) {
    const Face& F = mesh.faces[f];
    Vec3 n = frontSide ? F.normal : -F.normal;
    Vec3 u, v;
    planeBasis(n, u, v);
    double minU = 1e300, minV = 1e300, maxU = -1e300, maxV = -1e300;
    for (Id id : F.outer) {
        const Vec3& p = mesh.pos(id);
        minU = std::min(minU, p.dot(u)); maxU = std::max(maxU, p.dot(u));
        minV = std::min(minV, p.dot(v)); maxV = std::max(maxV, p.dot(v));
    }
    TexMap t;
    double W = std::max(maxU - minU, 1e-9), H = std::max(maxV - minV, 1e-9);
    t.valid = true;
    t.origin = u * minU + v * minV;
    t.u = u / W;
    t.v = v / H;
    return t;
}

} // namespace gs
