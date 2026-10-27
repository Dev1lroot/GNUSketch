// SPDX-License-Identifier: GPL-3.0-or-later
#include "Terrain.h"

#include "Triangulate.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gs {

namespace {

// Faces converted to triangles must roughly face the brush direction, so a
// wall standing on a sculpted ground keeps its shape.
constexpr double kFacingCos = 0.3;
// Vertices farther than this many radii along the brush direction are
// outside the brush (e.g. the floor under a table top).
constexpr double kAxialRadii = 3.0;
constexpr int kMaxSplitsPerStep = 20000;
// Flatten snaps its plane to a world axis closer than ~25 degrees.
constexpr double kSnapCos = 0.9;
// Paint mode: opacity laid down per second at the brush centre.
constexpr double kPaintRate = 3.0;
// Painted weights below this are dropped; a vertex keeps at most this many.
constexpr float kMinWeight = 1.0f / 512;
constexpr size_t kMaxLayers = 4;

Vec3 loopNormal(const Mesh& m, const std::vector<Id>& loop) {
    return newellNormal((int)loop.size(), [&](int i) { return m.pos(loop[i]); });
}

// Smallest distance from p to face f (its plane region including holes).
double faceDistance(const Mesh& m, Id f, const Vec3& p) {
    Plane P = m.facePlane(f);
    double h = P.distance(p);
    Vec3 q = p - P.n * h;
    if (m.pointInFace(f, q, true)) return std::fabs(h);
    double best = 1e300;
    const Face& F = m.faces[f];
    auto loop = [&](const std::vector<Id>& L) {
        for (size_t i = 0; i < L.size(); ++i)
            best = std::min(best, distancePointSegment(m.pos(L[i]), m.pos(L[(i + 1) % L.size()]), p));
    };
    loop(F.outer);
    for (const auto& hl : F.holes) loop(hl);
    return best;
}

} // namespace

double terrainFalloff(double d, double radius, double hardness) {
    if (radius <= 0 || d >= radius) return 0;
    const double inner = std::clamp(hardness, 0.0, 0.95);
    const double t = d / radius;
    if (t <= inner) return 1;
    const double s = (t - inner) / (1 - inner);
    return 0.5 * (1 + std::cos(kPi * s));
}

std::vector<Id> triangulateFace(Mesh& m, Id f) {
    if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) return {};
    Face proto = m.faces[f];
    if (proto.outer.size() == 3 && proto.holes.empty()) return {f};

    std::vector<Id> ids = proto.outer;
    for (const auto& h : proto.holes) ids.insert(ids.end(), h.begin(), h.end());
    Vec3 u, v;
    planeBasis(proto.normal, u, v);
    const Vec3 o = m.pos(ids[0]);
    std::vector<std::vector<Vec2>> loops;
    std::vector<Vec2> flat;
    auto to2 = [&](Id id) {
        Vec3 d = m.pos(id) - o;
        return Vec2{d.dot(u), d.dot(v)};
    };
    for (const auto* L : {&proto.outer}) {
        loops.emplace_back();
        for (Id id : *L) loops.back().push_back(to2(id));
    }
    for (const auto& h : proto.holes) {
        loops.emplace_back();
        for (Id id : h) loops.back().push_back(to2(id));
    }
    for (const auto& L : loops) flat.insert(flat.end(), L.begin(), L.end());

    std::vector<std::array<int, 3>> tris = triangulatePolygon(loops);
    if (tris.empty()) return {f};
    // Every triangle keeps projecting the texture the way the whole face did,
    // so it stays continuous however the triangles tilt later.
    if (proto.front.axis.length2() < 0.5) proto.front.axis = proto.normal;
    if (proto.back.axis.length2() < 0.5) proto.back.axis = -proto.normal;

    // Collinear boundary vertices produce zero-area ears; flip them away
    // against their neighbour so no slit opens when the vertices move.
    double scale = 0;
    for (const Vec2& p : flat) scale = std::max({scale, std::fabs(p.x), std::fabs(p.y)});
    const double eps = 1e-12 * std::max(scale * scale, 1e-12);
    auto area = [&](const std::array<int, 3>& t) {
        return (flat[t[1]] - flat[t[0]]).cross(flat[t[2]] - flat[t[0]]);
    };
    for (int round = 0; round < (int)tris.size() * 3; ++round) {
        bool changed = false;
        for (size_t a = 0; a < tris.size() && !changed; ++a) {
            if (std::fabs(area(tris[a])) > eps) continue;
            // Longest side i->j of the degenerate triangle (i, j, k).
            int best = 0;
            double bl = -1;
            for (int s = 0; s < 3; ++s) {
                double l = (flat[tris[a][(s + 1) % 3]] - flat[tris[a][s]]).length();
                if (l > bl) { bl = l; best = s; }
            }
            int i = tris[a][best], j = tris[a][(best + 1) % 3], k = tris[a][(best + 2) % 3];
            for (size_t b = 0; b < tris.size(); ++b) {
                if (b == a) continue;
                for (int s = 0; s < 3; ++s) {
                    if (tris[b][s] != j || tris[b][(s + 1) % 3] != i) continue;
                    int l = tris[b][(s + 2) % 3];
                    std::array<int, 3> t1{k, i, l}, t2{l, j, k};
                    if (area(t1) <= eps || area(t2) <= eps) continue;
                    tris[a] = t1;
                    tris[b] = t2;
                    changed = true;
                    break;
                }
                if (changed) break;
            }
        }
        if (!changed) break;
    }

    std::vector<Id> out;
    for (const auto& t : tris) {
        std::vector<Id> loop = {ids[t[0]], ids[t[1]], ids[t[2]]};
        if (loop[0] == loop[1] || loop[1] == loop[2] || loop[0] == loop[2]) continue;
        Vec3 n = loopNormal(m, loop);
        if (n.length() < 1e-14) continue;
        if (n.dot(proto.normal) < 0) {
            std::reverse(loop.begin(), loop.end());
            n = -n;
        }
        for (size_t i = 0; i < 3; ++i) {
            Id a = loop[i], b = loop[(i + 1) % 3];
            if (m.findEdge(a, b) != kNone) continue;
            Id e = m.addEdgeRaw(a, b);
            m.edges[e].soft = m.edges[e].smooth = true;
        }
        Face F = proto;
        F.outer = std::move(loop);
        F.holes.clear();
        F.normal = n.normalized();
        if (out.empty()) {
            m.faces[f] = std::move(F);
            out.push_back(f);
        } else {
            m.faces.push_back(std::move(F));
            out.push_back((Id)m.faces.size() - 1);
        }
    }
    if (out.empty()) return {f};
    m.touch();
    return out;
}

void paintVertex(Vertex& v, Id mat, double amount) {
    const float a = (float)std::clamp(amount, 0.0, 1.0);
    if (a <= 0) return;
    bool found = false;
    for (PaintWeight& w : v.paint) {
        w.weight *= 1 - a;
        if (w.material == mat) {
            w.weight += a;
            found = true;
        }
    }
    if (!found && mat != kNone) v.paint.push_back({mat, a});
    std::erase_if(v.paint, [](const PaintWeight& w) { return w.weight < kMinWeight; });
    if (v.paint.size() > kMaxLayers) {
        std::sort(v.paint.begin(), v.paint.end(), [](const auto& x, const auto& y) { return x.weight > y.weight; });
        v.paint.resize(kMaxLayers);
    }
}

Vec3 terrainNormal(const Mesh& m, const Vec3& center, Id face, const Vec3& toViewer, double radius) {
    if (face < 0 || face >= (Id)m.faces.size() || !m.faces[face].alive) return Vec3(0, 0, 1);
    Vec3 nh = m.faces[face].normal;
    if (nh.dot(toViewer) < 0) nh = -nh;
    Vec3 sum = nh * 1e-9;
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive) continue;
        double w;
        if (f == face) {
            w = 1;
        } else {
            if (F.outer.size() != 3 || !F.holes.empty()) continue;
            Vec3 c = (m.pos(F.outer[0]) + m.pos(F.outer[1]) + m.pos(F.outer[2])) / 3.0;
            w = terrainFalloff((c - center).length(), radius, 0.0);
            if (w <= 0) continue;
        }
        Vec3 n = F.normal;
        if (n.dot(nh) < 0) n = -n;
        sum += n * (w * std::max(m.faceArea(f), 1e-12));
    }
    Vec3 n = sum.normalized();
    return n.length2() > 0.5 ? n : nh;
}

bool TerrainStroke::begin(Mesh& m, const Vec3& hit, Id face, const Vec3& toViewer) {
    if (face < 0 || face >= (Id)m.faces.size() || !m.faces[face].alive) return false;
    dir_ = terrainNormal(m, hit, face, toViewer, brush_.radius);
    if (mode_ == TerrainMode::Flatten) {
        // Flatten levels to a height: on a roughly level (or upright) patch
        // the plane snaps to the world axis instead of following the bumps.
        for (int i = 0; i < 3; ++i)
            if (std::fabs(dir_[i]) >= kSnapCos) {
                Vec3 a;
                a[i] = dir_[i] > 0 ? 1 : -1;
                dir_ = a;
            }
    }
    origin_ = hit;
    refine(m, hit, face);
    shareTextureAxis(m, face);
    return true;
}

void TerrainStroke::shareTextureAxis(Mesh& m, Id start) {
    // Triangles sculpted before texture axes existed project along their own
    // tilted normals, which shows seams. Give the connected triangles without
    // an axis the nearest world axis (the ground's, usually) instead.
    if (!isTri(m, start)) return;
    Vec3 axis;
    {
        const Vec3 n = m.faces[start].normal;
        int best = 0;
        for (int i = 1; i < 3; ++i)
            if (std::fabs(dir_[i]) > std::fabs(dir_[best])) best = i;
        axis[best] = n[best] < 0 ? -1 : 1;
    }
    std::vector<char> seen(m.faces.size(), 0);
    std::vector<Id> stack = {start};
    seen[start] = 1;
    while (!stack.empty()) {
        const Id f = stack.back();
        stack.pop_back();
        Face& F = m.faces[f];
        if (F.front.axis.length2() < 0.5) F.front.axis = axis;
        if (F.back.axis.length2() < 0.5) F.back.axis = -axis;
        for (size_t i = 0; i < 3; ++i) {
            const Id e = m.findEdge(F.outer[i], F.outer[(i + 1) % 3]);
            if (e == kNone) continue;
            for (Id g : edgeFaces_[e]) {
                if (seen[g] || !isTri(m, g)) continue;
                seen[g] = 1;
                const Face& G = m.faces[g];
                // Only spread through triangles that still lack an axis.
                if (G.front.axis.length2() > 0.5 && G.back.axis.length2() > 0.5) continue;
                stack.push_back(g);
            }
        }
    }
    m.touch();
}

bool TerrainStroke::isTri(const Mesh& m, Id f) const {
    const Face& F = m.faces[f];
    return F.alive && F.outer.size() == 3 && F.holes.empty();
}

void TerrainStroke::buildAdjacency(const Mesh& m) {
    edgeFaces_.assign(m.edges.size(), {});
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive) continue;
        auto add = [&](const std::vector<Id>& L) {
            for (size_t i = 0; i < L.size(); ++i) {
                Id e = m.findEdge(L[i], L[(i + 1) % L.size()]);
                if (e != kNone) edgeFaces_[e].push_back(f);
            }
        };
        add(F.outer);
        for (const auto& h : F.holes) add(h);
    }
}

Id TerrainStroke::longestEdge(const Mesh& m, Id f) const {
    const auto& L = m.faces[f].outer;
    Id best = kNone;
    double bl = -1;
    for (size_t i = 0; i < 3; ++i) {
        Id a = L[i], b = L[(i + 1) % 3];
        double l = (m.pos(b) - m.pos(a)).length2();
        if (l > bl) { bl = l; best = m.findEdge(a, b); }
    }
    return best;
}

void TerrainStroke::bisect(Mesh& m, Id e) {
    const Id a = m.edges[e].v[0], b = m.edges[e].v[1];
    const std::vector<Id> faces = edgeFaces_[e];
    const Id v = m.addVertexRaw((m.pos(a) + m.pos(b)) * 0.5);
    // Painted textures blend linearly along the edge, like the GPU shows them.
    for (Id end : {a, b})
        for (const PaintWeight& w : m.vertices[end].paint) {
            auto& P = m.vertices[v].paint;
            auto it = std::find_if(P.begin(), P.end(), [&](const PaintWeight& x) { return x.material == w.material; });
            if (it == P.end()) P.push_back({w.material, w.weight * 0.5f});
            else it->weight += w.weight * 0.5f;
        }
    m.splitEdge(e, v, &faces);
    // e now runs a-v; the second half v-b was appended.
    const Id ev = m.findEdge(v, b);
    edgeFaces_.resize(m.edges.size());
    if (ev != kNone) edgeFaces_[ev] = faces;
    --splitBudget_;

    for (Id f : faces) {
        Face& F = m.faces[f];
        if (F.outer.size() != 4 || !F.holes.empty()) continue; // n-gon neighbour: v just joins its loop
        const auto q = F.outer;
        const int i = int(std::find(q.begin(), q.end(), v) - q.begin());
        if (i >= 4) continue;
        const Id next = q[(i + 1) % 4], opp = q[(i + 2) % 4], prev = q[(i + 3) % 4];
        Face G = F;
        F.outer = {prev, v, opp};
        G.outer = {v, next, opp};
        m.faces.push_back(std::move(G));
        const Id g = (Id)m.faces.size() - 1;
        m.recomputeNormal(f);
        m.recomputeNormal(g);
        Id d = m.addEdgeRaw(v, opp);
        m.edges[d].soft = m.edges[d].smooth = true;
        edgeFaces_.resize(m.edges.size());
        edgeFaces_[d] = {f, g};
        // Edges of the new half now belong to g instead of f.
        for (Id moved : {m.findEdge(v, next), m.findEdge(next, opp)}) {
            if (moved == kNone) continue;
            for (Id& x : edgeFaces_[moved])
                if (x == f) x = g;
        }
    }
}

void TerrainStroke::refineEdge(Mesh& m, Id e, int depth) {
    // Longest-edge propagation: a neighbour whose longest edge is longer gets
    // that edge split first, so triangles never degrade into slivers.
    for (int guard = 0; guard < 8 && depth < 48; ++guard) {
        bool again = false;
        const double len = m.edgeLength(e);
        const std::vector<Id> faces = edgeFaces_[e];
        for (Id f : faces) {
            if (!isTri(m, f)) continue;
            Id l = longestEdge(m, f);
            if (l == kNone || l == e || m.edgeLength(l) <= len * (1 + 1e-9)) continue;
            refineEdge(m, l, depth + 1);
            again = true;
            break;
        }
        if (!again || splitBudget_ <= 0) break;
    }
    if (splitBudget_ > 0 && m.edges[e].alive) bisect(m, e);
}

void TerrainStroke::refine(Mesh& m, const Vec3& c, Id forceFace) {
    const double R = brush_.radius, L = brush_.detail();
    // Convert the n-gons under the brush.
    const Id nf = (Id)m.faces.size();
    for (Id f = 0; f < nf; ++f) {
        if (!m.faces[f].alive || isTri(m, f)) continue;
        if (f != forceFace) {
            if (mode_ != TerrainMode::Paint && std::fabs(m.faces[f].normal.dot(dir_)) < kFacingCos) continue;
            if (faceDistance(m, f, c) >= R) continue;
        }
        triangulateFace(m, f);
    }

    buildAdjacency(m);
    splitBudget_ = kMaxSplitsPerStep;
    for (int pass = 0; pass < 64 && splitBudget_ > 0; ++pass) {
        std::vector<std::pair<double, Id>> todo;
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            const Edge& E = m.edges[e];
            if (!E.alive) continue;
            const Vec3 &a = m.pos(E.v[0]), &b = m.pos(E.v[1]);
            const double len = (b - a).length();
            if (len <= L) continue;
            if (distancePointSegment(a, b, c) >= R + L) continue;
            bool tri = false;
            for (Id f : edgeFaces_[e]) tri = tri || isTri(m, f);
            if (tri) todo.push_back({len, e});
        }
        if (todo.empty()) break;
        std::sort(todo.begin(), todo.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (const auto& [len, e] : todo) {
            if (splitBudget_ <= 0) break;
            if (!m.edges[e].alive || m.edgeLength(e) <= L) continue;
            refineEdge(m, e, 0);
        }
    }
    m.touch();
}

void TerrainStroke::apply(Mesh& m, const Vec3& c, double dt) {
    refine(m, c);
    const double R = brush_.radius;
    const double rate = brush_.force * dt;
    if (rate <= 0 && mode_ != TerrainMode::Paint) return;

    // Only vertices surrounded by triangles move: anything shared with an
    // n-gon face stays put so that face stays planar.
    std::vector<char> movable(m.vertices.size(), 0);
    for (const Face& F : m.faces) {
        if (!F.alive) continue;
        const bool tri = F.outer.size() == 3 && F.holes.empty();
        auto mark = [&](const std::vector<Id>& L) {
            for (Id v : L)
                if (!tri) movable[v] = 2;
                else if (movable[v] == 0) movable[v] = 1;
        };
        mark(F.outer);
        for (const auto& h : F.holes) mark(h);
    }

    if (mode_ == TerrainMode::Paint) {
        paint(m, c, dt, movable);
        return;
    }

    std::vector<char> moved(m.vertices.size(), 0);
    bool any = false;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
        if (movable[v] != 1 || !m.vertices[v].alive) continue;
        Vec3& p = m.vertices[v].p;
        const Vec3 rel = p - c;
        const double h = rel.dot(dir_);
        if (std::fabs(h) > kAxialRadii * R) continue;
        const double w = terrainFalloff((rel - dir_ * h).length(), R, brush_.hardness);
        if (w <= 0) continue;
        const double step = rate * w;
        switch (mode_) {
        case TerrainMode::Raise: p += dir_ * step; break;
        case TerrainMode::Lower: p -= dir_ * step; break;
        case TerrainMode::Flatten: {
            const double off = (p - origin_).dot(dir_);
            if (std::fabs(off) < 1e-9) continue;
            p -= dir_ * std::clamp(off, -step, step);
            break;
        }
        case TerrainMode::Paint: break; // handled by paint()
        }
        moved[v] = 1;
        any = true;
    }
    if (!any) return;
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive) continue;
        for (Id v : F.outer)
            if (moved[v]) { m.recomputeNormal(f); break; }
    }
    m.markGeometryMoved();
    m.touch();
}

void TerrainStroke::paint(Mesh& m, const Vec3& c, double dt, const std::vector<char>& paintable) {
    // A sphere around the brush centre, so painting wraps over ridges and corners.
    // Vertices of n-gon faces are skipped: their paint would smear over the whole face.
    const double R = brush_.radius;
    bool any = false;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
        if (paintable[v] != 1 || !m.vertices[v].alive) continue;
        const double w = terrainFalloff((m.pos(v) - c).length(), R, brush_.hardness);
        if (w <= 0) continue;
        paintVertex(m.vertices[v], paintMat_, kPaintRate * dt * w);
        any = true;
    }
    if (any) m.touch();
}

} // namespace gs
