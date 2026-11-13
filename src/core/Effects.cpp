// SPDX-License-Identifier: GPL-3.0-or-later
#include "Effects.h"

#include "Components.h"
#include "Solids.h"
#include "Triangulate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace gs {

namespace {

uint64_t edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// ---------------------------------------------------------------------------
// The selection as a triangle surface
// ---------------------------------------------------------------------------

struct Surface {
    struct V {
        Vec3 p;
        Id mesh = kNone;    // existing mesh vertex
        bool pinned = false; // shared with a face outside the selection
        int root = -1;      // new vertex on this outline edge
        std::vector<PaintWeight> paint;
    };
    struct T {
        std::array<int, 3> v;
        int src = 0;
    };
    struct Source {
        FaceSide front, back;
    };
    // An edge of the original faces. Pieces of a split edge inherit it.
    struct EdgeInfo {
        bool soft = false, smooth = false, hidden = false;
        Id curve = kNone;
        int root = -1; // shared with faces outside the selection
    };
    // An original edge shared with faces outside: gets the new vertices kept on it.
    struct Root {
        Id a, b;
        std::vector<Id> outside;
    };
    // One output polygon.
    struct Poly {
        std::vector<int> outer;
        std::vector<std::vector<int>> holes;
        int src = 0;
    };

    std::vector<V> verts;
    std::vector<T> tris;
    std::vector<Source> sources;
    std::unordered_map<uint64_t, EdgeInfo> edges;
    std::vector<Root> roots;
    std::vector<Id> oldFaces, oldEdges;
    bool limited = false;

    Vec3 triNormal(const T& t) const { // length = 2 * area
        return (verts[t.v[1]].p - verts[t.v[0]].p).cross(verts[t.v[2]].p - verts[t.v[0]].p);
    }
    double area() const {
        double a = 0;
        for (const T& t : tris) a += triNormal(t).length() * 0.5;
        return a;
    }

    bool extract(const Mesh& m, const std::set<Id>& selected);
    void refine(double maxEdge);
    // Splits edges for as long as the rule gives a positive priority (highest
    // first) and sets where along the edge to split.
    using SplitRule = std::function<double(int a, int b, double& t)>;
    void refineWith(const SplitRule& rule);
    // Longest edge allowed when subdividing to `wanted`, within the triangle limit.
    double limitedDetail(double wanted);
    bool write(Mesh& m, const std::vector<Poly>& polys, bool softenAll, bool visibleNew, bool smoothNew, EffectResult* out) const;
    std::vector<Poly> trianglePolys() const {
        std::vector<Poly> polys;
        polys.reserve(tris.size());
        for (const T& t : tris) polys.push_back({{t.v[0], t.v[1], t.v[2]}, {}, t.src});
        return polys;
    }
    // Polygons from a grouping of the triangles: straightened outlines between
    // vertices that matter. A group spanning several charts is not expected; a
    // bend between groups of the same chart is straightened away, a bend between
    // charts is kept (it is a crease of the model).
    using SnapFn = std::function<bool(int vertex, Vec3& pos, std::array<double, 3>& key)>;
    std::vector<Poly> clusterPolys(const std::vector<int>& cluster, const std::vector<int>& chartOf, const SnapFn& snap);
};

bool Surface::extract(const Mesh& m, const std::set<Id>& selected) {
    std::unordered_map<Id, int> vmap;
    auto vertexOf = [&](Id id) {
        auto it = vmap.find(id);
        if (it != vmap.end()) return it->second;
        V v;
        v.p = m.pos(id);
        v.mesh = id;
        v.paint = m.vertices[id].paint;
        for (Id e : m.vertexEdges(id)) {
            if (!m.edges[e].alive) continue;
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive && !selected.count(f)) v.pinned = true;
        }
        verts.push_back(std::move(v));
        vmap[id] = (int)verts.size() - 1;
        return (int)verts.size() - 1;
    };

    std::unordered_set<Id> seenEdges;
    for (Id f : selected) {
        if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) continue;
        const Face& F = m.faces[f];
        oldFaces.push_back(f);

        Vec3 u, w;
        planeBasis(F.normal, u, w);
        std::vector<std::vector<Vec2>> loops;
        std::vector<Vec2> flat;
        std::vector<Id> ids;
        auto addLoop = [&](const std::vector<Id>& L) {
            loops.emplace_back();
            for (Id id : L) {
                Vec2 q(m.pos(id).dot(u), m.pos(id).dot(w));
                loops.back().push_back(q);
                flat.push_back(q);
                ids.push_back(id);
            }
        };
        addLoop(F.outer);
        for (const auto& h : F.holes) addLoop(h);
        auto tri2 = triangulatePolygon(loops);
        flipDegenerateTriangles(flat, tri2);

        Source src{F.front, F.back};
        // Keep projecting the texture the way the whole face did, however the
        // triangles tilt later.
        if (src.front.axis.length2() < 0.5) src.front.axis = F.normal;
        if (src.back.axis.length2() < 0.5) src.back.axis = -F.normal;
        sources.push_back(src);
        const int srcId = (int)sources.size() - 1;

        for (auto t : tri2) {
            if (ids[t[0]] == ids[t[1]] || ids[t[1]] == ids[t[2]] || ids[t[0]] == ids[t[2]]) continue;
            T out;
            for (int k = 0; k < 3; ++k) out.v[k] = vertexOf(ids[t[k]]);
            out.src = srcId;
            Vec3 n = triNormal(out);
            if (n.length2() < 1e-30) continue;
            if (n.dot(F.normal) < 0) std::swap(out.v[1], out.v[2]);
            tris.push_back(out);
        }

        for (Id e : m.faceEdges(f)) {
            if (!seenEdges.insert(e).second) continue;
            oldEdges.push_back(e);
            const Edge& E = m.edges[e];
            const int sa = vertexOf(E.v[0]), sb = vertexOf(E.v[1]);
            EdgeInfo info;
            info.soft = E.soft;
            info.smooth = E.smooth;
            info.hidden = E.hidden;
            info.curve = E.curve;
            std::vector<Id> outside;
            for (Id g : m.edgeFaces(e))
                if (m.faces[g].alive && !selected.count(g)) outside.push_back(g);
            if (!outside.empty()) {
                roots.push_back({E.v[0], E.v[1], std::move(outside)});
                info.root = (int)roots.size() - 1;
            }
            edges[edgeKey(sa, sb)] = info;
        }
    }
    return !tris.empty();
}

double Surface::limitedDetail(double wanted) {
    // Longest-edge bisection ends up with roughly area / (0.35 L^2) triangles.
    const double byLimit = std::sqrt(area() / (0.35 * kEffectTriangleLimit));
    if (byLimit > wanted) limited = true;
    return std::max(wanted, byLimit);
}

void Surface::refineWith(const SplitRule& rule) {
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(tris.size() * 3);
    using Item = std::tuple<double, int, int>;
    std::priority_queue<Item> queue;
    auto push = [&](int a, int b) {
        double t = 0.5;
        const double priority = rule(a, b, t);
        if (priority > 0) queue.emplace(priority, a, b);
    };
    for (int t = 0; t < (int)tris.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            int a = tris[t].v[k], b = tris[t].v[(k + 1) % 3];
            auto& list = edgeTris[edgeKey(a, b)];
            if (list.empty()) push(a, b);
            list.push_back(t);
        }
    auto replaceTri = [&](int a, int b, int from, int to) {
        auto& list = edgeTris[edgeKey(a, b)];
        for (int& t : list)
            if (t == from) t = to;
    };
    while (!queue.empty() && (int)tris.size() < kEffectTriangleLimit) {
        auto [priority, a, b] = queue.top();
        queue.pop();
        auto it = edgeTris.find(edgeKey(a, b));
        if (it == edgeTris.end()) continue;
        double at = 0.5;
        if (rule(a, b, at) <= 0) continue;
        std::vector<int> onEdge = std::move(it->second);
        edgeTris.erase(it);

        V mid;
        mid.p = lerp(verts[a].p, verts[b].p, at);
        std::map<Id, float> mix;
        for (const PaintWeight& w : verts[a].paint) mix[w.material] += w.weight * float(1 - at);
        for (const PaintWeight& w : verts[b].paint) mix[w.material] += w.weight * float(at);
        for (const auto& [mat, weight] : mix) mid.paint.push_back({mat, weight});
        auto info = edges.find(edgeKey(a, b));
        const bool hasInfo = info != edges.end();
        EdgeInfo copy;
        if (hasInfo) {
            copy = info->second;
            mid.root = copy.root;
            mid.pinned = copy.root >= 0;
            edges.erase(info);
        }
        verts.push_back(std::move(mid));
        const int c = (int)verts.size() - 1;
        if (hasInfo) {
            edges[edgeKey(a, c)] = copy;
            edges[edgeKey(c, b)] = copy;
        }

        std::vector<int> aSide, bSide;
        for (int t : onEdge) {
            T tri = tris[t];
            int k = 0;
            while (tri.v[k] == a || tri.v[k] == b) ++k;
            const int opp = tri.v[k];
            // Keep the winding: replace the far end of the split edge in each half.
            T t1 = tri, t2 = tri;
            for (int s = 0; s < 3; ++s) {
                if (tri.v[s] == b) t1.v[s] = c;
                if (tri.v[s] == a) t2.v[s] = c;
            }
            tris[t] = t1; // keeps a, c, opp
            tris.push_back(t2);
            const int nt = (int)tris.size() - 1;
            replaceTri(b, opp, t, nt);
            aSide.push_back(t);
            bSide.push_back(nt);
            edgeTris[edgeKey(c, opp)] = {t, nt};
            push(c, opp);
            push(a, opp);
            push(b, opp);
        }
        edgeTris[edgeKey(a, c)] = aSide;
        edgeTris[edgeKey(c, b)] = bSide;
        push(a, c);
        push(c, b);
    }
    if (!queue.empty()) limited = true;
}

void Surface::refine(double maxEdge) {
    if (maxEdge <= 0) return;
    const double max2 = maxEdge * maxEdge;
    // Always splitting the globally longest edge: it is the longest of every
    // triangle it belongs to, which keeps the triangles well shaped.
    refineWith([&](int a, int b, double& t) {
        t = 0.5;
        const double l2 = (verts[a].p - verts[b].p).length2();
        return l2 > max2 ? l2 : 0.0;
    });
}

bool Surface::write(Mesh& m, const std::vector<Poly>& polys, bool softenAll, bool visibleNew, bool smoothNew,
                    EffectResult* out) const {
    for (Id f : oldFaces) m.eraseFace(f);

    std::vector<Id> ids(verts.size(), kNone);
    std::vector<char> used(verts.size(), 0);
    auto markLoop = [&](const std::vector<int>& L) {
        for (int v : L) used[v] = 1;
    };
    for (const Poly& p : polys) {
        markLoop(p.outer);
        for (const auto& h : p.holes) markLoop(h);
    }
    std::vector<std::vector<std::pair<double, int>>> onRoot(roots.size());
    for (int v = 0; v < (int)verts.size(); ++v) {
        const V& V_ = verts[v];
        if (V_.mesh != kNone) {
            ids[v] = V_.mesh;
            if (!V_.pinned) m.vertices[V_.mesh].p = V_.p;
            m.vertices[V_.mesh].paint = V_.paint;
            continue;
        }
        if (!used[v]) continue;
        ids[v] = m.addVertexRaw(V_.p);
        m.vertices[ids[v]].paint = V_.paint;
        if (V_.root >= 0) {
            const Root& r = roots[V_.root];
            onRoot[V_.root].emplace_back(closestParamOnSegment(m.pos(r.a), m.pos(r.b), V_.p), v);
        }
    }
    // Faces outside the selection take in the vertices kept on their edges.
    for (size_t r = 0; r < roots.size(); ++r) {
        auto& list = onRoot[r];
        std::sort(list.begin(), list.end());
        Id from = roots[r].a;
        for (const auto& [t, v] : list) {
            Id e = m.findEdge(from, roots[r].b);
            if (e == kNone) break;
            m.splitEdge(e, ids[v], &roots[r].outside);
            from = ids[v];
        }
    }

    struct Seam {
        int a, b, count = 0;
        Vec3 n0, n1;
    };
    std::unordered_map<uint64_t, Seam> seams;
    seams.reserve(polys.size() * 4);
    for (const Poly& p : polys) {
        std::vector<Id> outer;
        outer.reserve(p.outer.size());
        for (int v : p.outer) outer.push_back(ids[v]);
        std::vector<std::vector<Id>> holes;
        for (const auto& h : p.holes) {
            holes.emplace_back();
            for (int v : h) holes.back().push_back(ids[v]);
        }
        Id f = m.addFaceRaw(outer, holes);
        if (f == kNone) continue;
        m.faces[f].front = sources[p.src].front;
        m.faces[f].back = sources[p.src].back;
        if (out) out->faces.insert(f);
        const Vec3 n = m.faces[f].normal;
        auto seamsOf = [&](const std::vector<int>& L) {
            for (size_t i = 0; i < L.size(); ++i) {
                int a = L[i], b = L[(i + 1) % L.size()];
                Seam& s = seams[edgeKey(a, b)];
                s.a = a;
                s.b = b;
                if (s.count == 0) s.n0 = n;
                else if (s.count == 1) s.n1 = n;
                ++s.count;
            }
        };
        seamsOf(p.outer);
        for (const auto& h : p.holes) seamsOf(h);
    }

    // Edges keep what they were; new ones are soft only over shallow bends.
    const double shallow = std::cos(40.0 * kPi / 180.0);
    for (const auto& [key, s] : seams) {
        Id e = m.findEdge(ids[s.a], ids[s.b]);
        if (e == kNone) continue;
        Edge& E = m.edges[e];
        auto info = edges.find(key);
        const bool isShallow = s.count == 2 && s.n0.dot(s.n1) >= shallow;
        if (info != edges.end()) {
            E.hidden = info->second.hidden;
            E.curve = info->second.curve;
            if (info->second.root >= 0) continue; // also an edge of an unselected face
            E.soft = info->second.soft;
            E.smooth = info->second.smooth;
            if (softenAll && s.count == 2) E.soft = E.smooth = true;
            continue;
        }
        if (softenAll) {
            E.soft = E.smooth = s.count == 2;
        } else if (visibleNew) {
            E.soft = false;
            E.smooth = smoothNew && isShallow;
        } else {
            E.soft = E.smooth = isShallow;
        }
    }

    m.removeEdgesWithoutFaces(oldEdges);
    m.removeOrphanVertices();
    m.touch();
    m.markGeometryMoved();
    if (out) out->limited = limited;
    return true;
}

std::vector<Surface::Poly> Surface::clusterPolys(const std::vector<int>& cluster, const std::vector<int>& chartOf,
                                                 const SnapFn& snap) {
    const int nt = (int)tris.size(), nv = (int)verts.size();
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(nt * 3);
    auto hasDir = [&](int t, int a, int b) {
        for (int k = 0; k < 3; ++k)
            if (tris[t].v[k] == a && tris[t].v[(k + 1) % 3] == b) return true;
        return false;
    };
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) edgeTris[edgeKey(tris[t].v[k], tris[t].v[(k + 1) % 3])].push_back(t);

    // Connected pieces of each cluster, across consistently wound manifold edges.
    std::vector<int> comp(nt, -1);
    int nc = 0;
    std::vector<int> compChart;
    for (int seed = 0; seed < nt; ++seed) {
        if (comp[seed] >= 0) continue;
        std::vector<int> stack{seed};
        comp[seed] = nc;
        while (!stack.empty()) {
            int t = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                int a = tris[t].v[k], b = tris[t].v[(k + 1) % 3];
                const auto& list = edgeTris[edgeKey(a, b)];
                if (list.size() != 2) continue;
                int o = list[0] == t ? list[1] : list[0];
                if (comp[o] >= 0 || cluster[o] != cluster[t] || !hasDir(o, b, a)) continue;
                comp[o] = nc;
                stack.push_back(o);
            }
        }
        compChart.push_back(chartOf[cluster[seed]]);
        ++nc;
    }

    // Outline cycles of each piece.
    std::vector<std::vector<int>> compTris(nc);
    for (int t = 0; t < nt; ++t) compTris[comp[t]].push_back(t);
    std::vector<std::vector<std::vector<int>>> cycles(nc);
    std::vector<char> bad(nc, 0);
    for (int c = 0; c < nc; ++c) {
        std::unordered_map<int, std::vector<int>> next;
        for (int t : compTris[c])
            for (int k = 0; k < 3; ++k) {
                int a = tris[t].v[k], b = tris[t].v[(k + 1) % 3];
                bool twin = false;
                for (int o : edgeTris[edgeKey(a, b)])
                    if (o != t && comp[o] == c && hasDir(o, b, a)) twin = true;
                if (!twin) next[a].push_back(b);
            }
        for (const auto& [v, outs] : next)
            if (outs.size() != 1) bad[c] = 1; // the outline touches itself
        if (bad[c]) continue;
        std::unordered_set<int> done;
        for (const auto& [start, outs] : next) {
            if (done.count(start)) continue;
            std::vector<int> cycle;
            int v = start;
            while (!done.count(v)) {
                done.insert(v);
                cycle.push_back(v);
                auto it = next.find(v);
                if (it == next.end()) { bad[c] = 1; break; }
                v = it->second[0];
            }
            if (v != start) bad[c] = 1;
            cycles[c].push_back(std::move(cycle));
        }
    }

    // Which vertices stay: the junctions and real corners of the outlines.
    std::vector<char> keep(nv, 0);
    std::vector<std::vector<int>> outlineEdges(nv); // other ends of outline edges
    std::vector<std::set<int>> vComps(nv);
    for (int t = 0; t < nt; ++t)
        for (int v : tris[t].v) vComps[v].insert(comp[t]);
    std::vector<char> onBorder(nv, 0);
    for (const auto& [key, list] : edgeTris) {
        bool outline = list.size() != 2;
        if (!outline) outline = comp[list[0]] != comp[list[1]];
        if (!outline) continue;
        const int a = int(key >> 32), b = int(key & 0xffffffffu);
        if (list.size() != 2) onBorder[a] = onBorder[b] = 1;
        outlineEdges[a].push_back(b);
        outlineEdges[b].push_back(a);
    }
    const double straight = std::cos(0.25 * kPi / 180.0);
    for (int v = 0; v < nv; ++v) {
        const auto& oe = outlineEdges[v];
        if (oe.empty()) continue;
        if (verts[v].mesh != kNone && verts[v].pinned) { keep[v] = 1; continue; }
        if (vComps[v].size() >= 3 || oe.size() != 2) { keep[v] = 1; continue; }
        Vec3 d0 = (verts[oe[0]].p - verts[v].p).normalized(), d1 = (verts[oe[1]].p - verts[v].p).normalized();
        if (-d0.dot(d1) >= straight) continue;
        if (vComps[v].size() == 1) { keep[v] = 1; continue; }
        auto it = vComps[v].begin();
        int c0 = *it++, c1 = *it;
        if (compChart[c0] != compChart[c1]) keep[v] = 1;
    }

    // Snapping junctions onto the pattern merges those of one corner.
    std::vector<int> rep(nv);
    std::iota(rep.begin(), rep.end(), 0);
    std::vector<Vec3> snapped(nv);
    std::vector<char> hasSnap(nv, 0);
    if (snap) {
        std::map<std::array<double, 3>, int> corners;
        for (int v = 0; v < nv; ++v) {
            if (!keep[v] || verts[v].pinned || onBorder[v] || vComps[v].empty()) continue;
            int chart = -2;
            bool oneChart = true;
            for (int c : vComps[v]) {
                if (chart != -2 && chart != compChart[c]) oneChart = false;
                chart = compChart[c];
            }
            if (!oneChart || outlineEdges[v].size() < 3) continue;
            Vec3 pos;
            std::array<double, 3> key{};
            if (!snap(v, pos, key)) continue;
            key[2] = chart;
            auto [it, fresh] = corners.emplace(key, v);
            rep[v] = it->second;
            snapped[it->second] = pos;
            hasSnap[it->second] = 1;
        }
    }

    // Every cycle must remain a proper polygon; where it would not, keep more of
    // its vertices. A piece that cannot be fixed goes out as its triangles.
    auto filtered = [&](const std::vector<int>& cycle) {
        std::vector<int> L;
        for (int v : cycle) {
            if (!keep[v]) continue;
            const int r = rep[v];
            if (L.empty() || L.back() != r) L.push_back(r);
        }
        while (L.size() > 1 && L.front() == L.back()) L.pop_back();
        return L;
    };
    auto posOf = [&](int v) { return hasSnap[v] ? snapped[v] : verts[v].p; };
    auto normalOf = [&](const std::vector<int>& L) {
        return newellNormal((int)L.size(), [&](int i) { return posOf(L[i]); });
    };
    auto proper = [&](const std::vector<int>& L) {
        if (L.size() < 3) return false;
        std::vector<int> sorted = L;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return false;
        return normalOf(L).length() > 1e-14;
    };
    for (int pass = 0; pass < 12; ++pass) {
        bool changed = false;
        for (int c = 0; c < nc; ++c) {
            if (bad[c]) {
                for (int t : compTris[c])
                    for (int v : tris[t].v) {
                        if (!keep[v] || rep[v] != v) changed = true;
                        keep[v] = 1;
                        if (rep[v] != v) rep[v] = v;
                    }
                continue;
            }
            for (const auto& cycle : cycles[c]) {
                if (proper(filtered(cycle))) continue;
                changed = true;
                if (pass >= 8) { bad[c] = 1; break; }
                for (int v : cycle) rep[v] = v;
                const size_t n = cycle.size();
                for (size_t k = 0; k < 4 && k < n; ++k) keep[cycle[k * n / std::min<size_t>(4, n)]] = 1;
            }
        }
        if (!changed) break;
    }

    for (int v = 0; v < nv; ++v)
        if (hasSnap[v] && rep[v] == v) verts[v].p = snapped[v];

    std::vector<Poly> polys;
    for (int c = 0; c < nc; ++c) {
        const int src = tris[compTris[c][0]].src;
        bool asTris = bad[c] != 0;
        std::vector<std::vector<int>> loops;
        if (!asTris) {
            for (const auto& cycle : cycles[c]) {
                loops.push_back(filtered(cycle));
                asTris = asTris || !proper(loops.back());
            }
        }
        if (asTris || loops.empty()) {
            for (int t : compTris[c]) polys.push_back({{tris[t].v[0], tris[t].v[1], tris[t].v[2]}, {}, tris[t].src});
            continue;
        }
        size_t best = 0;
        double bestArea = -1;
        for (size_t i = 0; i < loops.size(); ++i) {
            double a = normalOf(loops[i]).length();
            if (a > bestArea) { bestArea = a; best = i; }
        }
        Poly p;
        p.src = src;
        p.outer = loops[best];
        for (size_t i = 0; i < loops.size(); ++i)
            if (i != best) p.holes.push_back(loops[i]);
        polys.push_back(std::move(p));
    }
    return polys;
}

bool emptyError(std::string* error) {
    if (error) *error = "Select faces first.";
    return false;
}

std::vector<int> faceAdjacencyFlat(const Surface& s, double cosTol = 1 - 1e-9, double distTol = kTol) {
    // Groups connected coplanar triangles with the same materials.
    const int nt = (int)s.tris.size();
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(nt * 3);
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])].push_back(t);
    std::vector<int> group(nt, -1);
    int groups = 0;
    auto sameSide = [](const FaceSide& a, const FaceSide& b) {
        if (a.material != b.material || a.tex.valid != b.tex.valid) return false;
        if (!a.tex.valid) return true;
        return a.tex.origin.nearlyEquals(b.tex.origin) && a.tex.u.nearlyEquals(b.tex.u, 1e-9) && a.tex.v.nearlyEquals(b.tex.v, 1e-9);
    };
    for (int seed = 0; seed < nt; ++seed) {
        if (group[seed] >= 0) continue;
        const Vec3 n0 = s.triNormal(s.tris[seed]).normalized();
        const double d0 = n0.dot(s.verts[s.tris[seed].v[0]].p);
        const Surface::Source& src = s.sources[s.tris[seed].src];
        group[seed] = groups;
        std::vector<int> stack{seed};
        while (!stack.empty()) {
            int t = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                const auto& list = edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])];
                if (list.size() != 2) continue;
                int o = list[0] == t ? list[1] : list[0];
                if (group[o] >= 0) continue;
                const Surface::Source& so = s.sources[s.tris[o].src];
                if (!sameSide(src.front, so.front) || !sameSide(src.back, so.back)) continue;
                Vec3 nO = s.triNormal(s.tris[o]);
                if (nO.length2() < 1e-30 || nO.normalized().dot(n0) < cosTol) continue;
                bool onPlane = true;
                for (int v : s.tris[o].v) onPlane = onPlane && std::fabs(n0.dot(s.verts[v].p) - d0) <= distTol;
                if (!onPlane) continue;
                group[o] = groups;
                stack.push_back(o);
            }
        }
        ++groups;
    }
    return group;
}

std::vector<Surface::Poly> flatPolys(Surface& s) {
    std::vector<int> group = faceAdjacencyFlat(s);
    int n = 0;
    for (int g : group) n = std::max(n, g + 1);
    std::vector<int> chart(n);
    std::iota(chart.begin(), chart.end(), 0);
    return s.clusterPolys(group, chart, nullptr);
}

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B1u;
    for (uint32_t v : {uint32_t(x), uint32_t(y), uint32_t(z)}) {
        h ^= v * 0xCC9E2D51u;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xE6546B64u;
    }
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

// Smooth value noise in -1..1.
double valueNoise(const Vec3& p, uint32_t seed) {
    const int xi = (int)std::floor(p.x), yi = (int)std::floor(p.y), zi = (int)std::floor(p.z);
    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const double u = fade(p.x - xi), v = fade(p.y - yi), w = fade(p.z - zi);
    auto corner = [&](int dx, int dy, int dz) {
        return hash3(xi + dx, yi + dy, zi + dz, seed) / double(0xFFFFFFFFu) * 2.0 - 1.0;
    };
    auto mix = [](double a, double b, double t) { return a + (b - a) * t; };
    return mix(mix(mix(corner(0, 0, 0), corner(1, 0, 0), u), mix(corner(0, 1, 0), corner(1, 1, 0), u), v),
               mix(mix(corner(0, 0, 1), corner(1, 0, 1), u), mix(corner(0, 1, 1), corner(1, 1, 1), u), v), w);
}

double layeredNoise(const Vec3& p, int octaves, uint32_t seed) {
    double sum = 0, weight = 1, total = 0, freq = 1;
    for (int o = 0; o < octaves; ++o) {
        sum += valueNoise(p * freq + Vec3(o * 17.3, o * 5.7, o * 11.1), seed + o * 1013) * weight;
        total += weight;
        weight *= 0.5;
        freq *= 2;
    }
    return total > 0 ? sum / total : 0;
}

// ---------------------------------------------------------------------------
// Decimate: quadric-error edge collapse
// ---------------------------------------------------------------------------

struct Quadric {
    double a2 = 0, ab = 0, ac = 0, ad = 0, b2 = 0, bc = 0, bd = 0, c2 = 0, cd = 0, d2 = 0;
    void addPlane(const Vec3& n, double d, double w) {
        a2 += w * n.x * n.x; ab += w * n.x * n.y; ac += w * n.x * n.z; ad += w * n.x * d;
        b2 += w * n.y * n.y; bc += w * n.y * n.z; bd += w * n.y * d;
        c2 += w * n.z * n.z; cd += w * n.z * d;
        d2 += w * d * d;
    }
    Quadric& operator+=(const Quadric& q) {
        a2 += q.a2; ab += q.ab; ac += q.ac; ad += q.ad; b2 += q.b2; bc += q.bc; bd += q.bd; c2 += q.c2; cd += q.cd; d2 += q.d2;
        return *this;
    }
    double error(const Vec3& p) const {
        return a2 * p.x * p.x + 2 * ab * p.x * p.y + 2 * ac * p.x * p.z + 2 * ad * p.x + b2 * p.y * p.y +
               2 * bc * p.y * p.z + 2 * bd * p.y + c2 * p.z * p.z + 2 * cd * p.z + d2;
    }
};

void collapseToTarget(Surface& s, int target) {
    const int nv = (int)s.verts.size();
    std::vector<std::vector<int>> vTris(nv);
    std::vector<char> triAlive(s.tris.size(), 1), vertAlive(nv, 1), openBorder(nv, 0);
    std::vector<Quadric> Q(nv);
    std::vector<int> version(nv, 0);
    std::unordered_map<uint64_t, int> edgeUse;
    for (int t = 0; t < (int)s.tris.size(); ++t) {
        const auto& T = s.tris[t];
        for (int k = 0; k < 3; ++k) {
            vTris[T.v[k]].push_back(t);
            ++edgeUse[edgeKey(T.v[k], T.v[(k + 1) % 3])];
        }
        Vec3 n = s.triNormal(T);
        const double area = n.length() * 0.5;
        if (area < 1e-16) continue;
        n = n.normalized();
        const double d = -n.dot(s.verts[T.v[0]].p);
        for (int k = 0; k < 3; ++k) Q[T.v[k]].addPlane(n, d, area);
    }
    // The outline of an open surface resists leaving its line.
    std::unordered_set<uint64_t> borderEdges;
    for (int t = 0; t < (int)s.tris.size(); ++t) {
        const auto& T = s.tris[t];
        for (int k = 0; k < 3; ++k) {
            int a = T.v[k], b = T.v[(k + 1) % 3];
            if (edgeUse[edgeKey(a, b)] != 1) continue;
            borderEdges.insert(edgeKey(a, b));
            openBorder[a] = openBorder[b] = 1;
            Vec3 e = s.verts[b].p - s.verts[a].p;
            Vec3 n = e.cross(s.triNormal(T)).normalized();
            const double w = 100 * e.length2();
            Q[a].addPlane(n, -n.dot(s.verts[a].p), w);
            Q[b].addPlane(n, -n.dot(s.verts[b].p), w);
        }
    }

    struct Item {
        double cost;
        int keep, drop, verKeep, verDrop;
        Vec3 to;
        bool operator<(const Item& o) const { return cost > o.cost; }
    };
    std::priority_queue<Item> heap;
    auto neighbours = [&](int v) {
        std::vector<int> out;
        for (int t : vTris[v])
            if (triAlive[t])
                for (int w : s.tris[t].v)
                    if (w != v && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        return out;
    };
    auto pushEdge = [&](int a, int b) {
        Quadric q = Q[a];
        q += Q[b];
        const bool isBorderEdge = borderEdges.count(edgeKey(a, b)) != 0;
        auto can = [&](int drop, int keep) {
            if (s.verts[drop].pinned) return false;
            return !openBorder[drop] || (openBorder[keep] && isBorderEdge);
        };
        Item best{1e300, -1, -1, 0, 0, Vec3()};
        auto consider = [&](int keep, int drop, const Vec3& to) {
            const double c = q.error(to);
            if (c < best.cost) best = {c, keep, drop, version[keep], version[drop], to};
        };
        if (can(a, b)) consider(b, a, s.verts[b].p);
        if (can(b, a)) consider(a, b, s.verts[a].p);
        // Both free to move: the middle often fits the surface better.
        if (can(a, b) && can(b, a) && !openBorder[a] && !openBorder[b])
            consider(a, b, lerp(s.verts[a].p, s.verts[b].p, 0.5));
        if (best.keep >= 0) heap.push(best);
    };
    for (const auto& [key, use] : edgeUse) pushEdge(int(key >> 32), int(key & 0xffffffffu));

    int alive = (int)s.tris.size();
    while (alive > target && !heap.empty()) {
        Item it = heap.top();
        heap.pop();
        if (!vertAlive[it.keep] || !vertAlive[it.drop] || version[it.keep] != it.verKeep || version[it.drop] != it.verDrop) continue;
        const int k = it.keep, r = it.drop;
        // The link condition: only the triangles on the edge may disappear.
        std::vector<int> nk = neighbours(k), nr = neighbours(r);
        int common = 0, shared = 0;
        for (int w : nr)
            if (std::find(nk.begin(), nk.end(), w) != nk.end()) ++common;
        for (int t : vTris[r])
            if (triAlive[t] && (s.tris[t].v[0] == k || s.tris[t].v[1] == k || s.tris[t].v[2] == k)) ++shared;
        if (shared == 0 || common != shared) continue;
        // No triangle may flip or collapse.
        bool ok = true;
        auto check = [&](int moved) {
            for (int t : vTris[moved]) {
                if (!triAlive[t]) continue;
                const auto& T = s.tris[t];
                if (T.v[0] == (moved == k ? r : k) || T.v[1] == (moved == k ? r : k) || T.v[2] == (moved == k ? r : k)) continue;
                std::array<Vec3, 3> p;
                for (int i = 0; i < 3; ++i) p[i] = s.verts[T.v[i]].p;
                Vec3 before = (p[1] - p[0]).cross(p[2] - p[0]);
                for (int i = 0; i < 3; ++i)
                    if (T.v[i] == moved) p[i] = it.to;
                Vec3 after = (p[1] - p[0]).cross(p[2] - p[0]);
                if (after.length2() < 1e-24 || after.normalized().dot(before.normalized()) < 0.3) ok = false;
            }
        };
        check(k);
        check(r);
        if (!ok) continue;

        for (int t : vTris[r]) {
            if (!triAlive[t]) continue;
            auto& T = s.tris[t];
            if (T.v[0] == k || T.v[1] == k || T.v[2] == k) {
                triAlive[t] = 0;
                --alive;
                continue;
            }
            for (int& v : T.v)
                if (v == r) v = k;
            vTris[k].push_back(t);
        }
        // Edges carried over from the dropped vertex keep their looks.
        for (int w : nr) {
            auto info = s.edges.find(edgeKey(r, w));
            if (info == s.edges.end() || w == k) continue;
            Surface::EdgeInfo copy = info->second;
            s.edges.erase(info);
            if (!s.edges.count(edgeKey(k, w))) s.edges[edgeKey(k, w)] = copy;
            if (borderEdges.erase(edgeKey(r, w))) borderEdges.insert(edgeKey(k, w));
        }
        s.edges.erase(edgeKey(k, r));
        borderEdges.erase(edgeKey(k, r));
        vertAlive[r] = 0;
        s.verts[k].p = it.to;
        Q[k] += Q[r];
        ++version[k];
        for (int w : neighbours(k)) pushEdge(k, w);
    }

    std::vector<Surface::T> kept;
    kept.reserve(alive);
    for (int t = 0; t < (int)s.tris.size(); ++t)
        if (triAlive[t]) kept.push_back(s.tris[t]);
    s.tris = std::move(kept);
}

// ---------------------------------------------------------------------------
// Remesh patterns
// ---------------------------------------------------------------------------

struct Lattice {
    RemeshParams::Shape shape;
    double size; // width of a cell

    // Cell containing q; the corner of it closest to q.
    std::array<int, 2> cell(const Vec2& q, Vec2* nearest = nullptr) const {
        std::array<int, 2> c{};
        std::vector<Vec2> corners;
        switch (shape) {
        case RemeshParams::Quads: {
            c = {(int)std::floor(q.x / size), (int)std::floor(q.y / size)};
            corners = {Vec2(c[0] * size, c[1] * size), Vec2((c[0] + 1) * size, c[1] * size),
                       Vec2((c[0] + 1) * size, (c[1] + 1) * size), Vec2(c[0] * size, (c[1] + 1) * size)};
            break;
        }
        case RemeshParams::Triangles: {
            // Rows of up and down triangles with sides `size`.
            const double h = size * std::sqrt(3.0) / 2;
            const int row = (int)std::floor(q.y / h);
            const double x = q.x - (row & 1 ? size / 2 : 0);
            const int col = (int)std::floor(x / size);
            const double fx = x / size - col, fy = q.y / h - row;
            const double off = row & 1 ? size / 2 : 0;
            Vec2 a(col * size + off, row * h), b((col + 1) * size + off, row * h);
            Vec2 top(col * size + off + size / 2, (row + 1) * h);
            if (fx * 2 < fy) {
                c = {col * 2 - 1, row};
                corners = {a, top, Vec2(col * size + off - size / 2, (row + 1) * h)};
            } else if ((1 - fx) * 2 < fy) {
                c = {col * 2 + 1, row};
                corners = {b, top, Vec2(col * size + off + size * 1.5, (row + 1) * h)};
            } else {
                c = {col * 2, row};
                corners = {a, b, top};
            }
            break;
        }
        case RemeshParams::Hexagons: {
            // Pointy-top hexagons `size` wide; axial coordinates with cube rounding.
            const double r = size / std::sqrt(3.0);
            const double fq = (std::sqrt(3.0) / 3 * q.x - q.y / 3) / r, fr = (2.0 / 3 * q.y) / r, fs = -fq - fr;
            double rq = std::round(fq), rr = std::round(fr), rs = std::round(fs);
            const double dq = std::fabs(rq - fq), dr = std::fabs(rr - fr), ds = std::fabs(rs - fs);
            if (dq > dr && dq > ds) rq = -rr - rs;
            else if (dr > ds) rr = -rq - rs;
            c = {(int)rq, (int)rr};
            const Vec2 centre(r * std::sqrt(3.0) * (rq + rr / 2), r * 1.5 * rr);
            for (int i = 0; i < 6; ++i) {
                const double a = kPi / 180 * (60 * i + 30);
                corners.emplace_back(centre.x + r * std::cos(a), centre.y + r * std::sin(a));
            }
            break;
        }
        }
        if (nearest) {
            double best = 1e300;
            for (const Vec2& k : corners) {
                double d = (k - q).length();
                if (d < best) { best = d; *nearest = k; }
            }
        }
        return c;
    }
    double cellArea() const {
        switch (shape) {
        case RemeshParams::Quads: return size * size;
        case RemeshParams::Triangles: return size * size * std::sqrt(3.0) / 4;
        case RemeshParams::Hexagons: return size * size * std::sqrt(3.0) / 2;
        }
        return size * size;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The effects
// ---------------------------------------------------------------------------

bool effectNoise(Mesh& m, const std::set<Id>& faces, const NoiseParams& p, EffectResult* out, std::string* error) {
    if (!p.x && !p.y && !p.z) {
        if (error) *error = "Choose at least one axis.";
        return false;
    }
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    const double scale = std::max(p.scale, 1e-3);
    s.refine(s.limitedDetail(p.detail > 0 ? p.detail : scale / 4));
    const Vec3 offsets[3] = {Vec3(0, 0, 0), Vec3(37.1, 11.7, 53.3), Vec3(71.9, 91.3, 23.7)};
    for (Surface::V& v : s.verts) {
        if (v.pinned) continue;
        const Vec3 q = v.p / scale;
        Vec3 d;
        if (p.x) d.x = layeredNoise(q + offsets[0], p.octaves, p.seed);
        if (p.y) d.y = layeredNoise(q + offsets[1], p.octaves, p.seed);
        if (p.z) d.z = layeredNoise(q + offsets[2], p.octaves, p.seed);
        v.p += d * p.amount;
    }
    return s.write(m, s.trianglePolys(), p.smooth, false, false, out);
}

bool effectBlur(Mesh& m, const std::set<Id>& faces, const BlurParams& p, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    const double radius = std::max(p.radius, 1e-3);
    const double detail = s.limitedDetail(p.detail > 0 ? p.detail : radius / 2);
    s.refine(detail);

    const int nv = (int)s.verts.size();
    std::unordered_map<uint64_t, int> use;
    for (const auto& t : s.tris)
        for (int k = 0; k < 3; ++k) ++use[edgeKey(t.v[k], t.v[(k + 1) % 3])];
    std::vector<char> fixed(nv, 0);
    std::vector<std::vector<int>> around(nv);
    for (const auto& [key, n] : use) {
        const int a = int(key >> 32), b = int(key & 0xffffffffu);
        around[a].push_back(b);
        around[b].push_back(a);
        if (n != 2) fixed[a] = fixed[b] = 1; // outline: keeps the surface where it ends
    }
    for (int v = 0; v < nv; ++v) fixed[v] = fixed[v] || s.verts[v].pinned;

    // Uniform diffusion spreads a point over sigma^2 = lambda h^2 / 2 per pass.
    constexpr double kLambda = 0.5;
    const double passes = 2 * radius * radius / (kLambda * detail * detail) * std::clamp(p.strength, 0.0, 1.0);
    const int count = std::min((int)std::ceil(passes), 400);
    std::vector<Vec3> next(nv), original(nv);
    for (int v = 0; v < nv; ++v) original[v] = s.verts[v].p;
    auto normals = [&] {
        std::vector<Vec3> n(nv);
        for (const auto& t : s.tris) {
            const Vec3 fn = s.triNormal(t);
            for (int v : t.v) n[v] += fn;
        }
        return n;
    };
    for (int i = 0; i < count; ++i) {
        for (int v = 0; v < nv; ++v) {
            next[v] = s.verts[v].p;
            if (fixed[v] || around[v].empty()) continue;
            Vec3 sum;
            for (int w : around[v]) sum += s.verts[w].p;
            next[v] += (sum / (double)around[v].size() - s.verts[v].p) * kLambda;
        }
        for (int v = 0; v < nv; ++v) s.verts[v].p = next[v];
    }
    if (p.keepVolume) {
        // Volume lost = what the surface swept moving inwards. Give it back evenly
        // along the normals, leaving the fixed outline where it is.
        const std::vector<Vec3> n = normals(); // length = 6 x the area around the vertex
        double swept = 0, freeArea = 0;
        for (int v = 0; v < nv; ++v) {
            if (fixed[v]) continue;
            swept += (original[v] - s.verts[v].p).dot(n[v]) / 6;
            freeArea += n[v].length() / 6;
        }
        if (freeArea > 1e-12) {
            const double offset = swept / freeArea;
            for (int v = 0; v < nv; ++v)
                if (!fixed[v]) s.verts[v].p += n[v].normalized() * offset;
        }
    }
    return s.write(m, s.trianglePolys(), true, false, false, out);
}

bool effectDecimate(Mesh& m, const std::set<Id>& faces, const DecimateParams& p, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    const int have = (int)s.tris.size();
    const double wanted = p.mode == DecimateParams::Ratio ? have * std::clamp(p.ratio, 0.0, 1.0) : s.area() * std::max(p.density, 0.0);
    collapseToTarget(s, std::max(4, (int)std::ceil(wanted)));
    return s.write(m, p.mergeFlat ? flatPolys(s) : s.trianglePolys(), false, false, false, out);
}

bool effectSubdivide(Mesh& m, const std::set<Id>& faces, double edgesPerMeter, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    s.refine(s.limitedDetail(1.0 / std::max(edgesPerMeter, 1e-3)));
    return s.write(m, s.trianglePolys(), false, true, true, out);
}

bool effectTriangulate(Mesh& m, const std::set<Id>& faces, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    return s.write(m, s.trianglePolys(), false, true, true, out);
}

bool effectMergeFlat(Mesh& m, const std::set<Id>& faces, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    return s.write(m, flatPolys(s), false, true, true, out);
}

bool effectRemesh(Mesh& m, const std::set<Id>& faces, const RemeshParams& p, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    const Lattice lattice{p.shape, 1.0 / std::max(p.cellsPerMeter, 1e-3)};
    // Triangular cells are the narrowest: they need a finer base to come out clean.
    const double fine = s.limitedDetail(lattice.size / (p.shape == RemeshParams::Triangles ? 14 : 8));
    s.refine(fine);
    const int nt = (int)s.tris.size();

    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(nt * 3);
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])].push_back(t);
    std::vector<Vec3> triN(nt);
    std::vector<double> triArea(nt);
    for (int t = 0; t < nt; ++t) {
        Vec3 n = s.triNormal(s.tris[t]);
        triArea[t] = n.length() * 0.5;
        triN[t] = n.normalized();
    }

    // Charts: connected triangles facing about the same way, each projected
    // flat onto its own plane for the pattern.
    constexpr double kChartCos = 0.57; // ~55 degrees
    std::vector<int> chart(nt, -1);
    struct Chart {
        Vec3 sum, n, u, v;
    };
    std::vector<Chart> charts;
    for (int seed = 0; seed < nt; ++seed) {
        if (chart[seed] >= 0) continue;
        const int id = (int)charts.size();
        Chart c;
        c.sum = triN[seed] * std::max(triArea[seed], 1e-12);
        chart[seed] = id;
        std::queue<int> open;
        open.push(seed);
        while (!open.empty()) {
            int t = open.front();
            open.pop();
            for (int k = 0; k < 3; ++k) {
                const auto& list = edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])];
                if (list.size() != 2) continue;
                int o = list[0] == t ? list[1] : list[0];
                if (chart[o] >= 0 || triN[o].dot(c.sum.normalized()) < kChartCos) continue;
                chart[o] = id;
                c.sum += triN[o] * std::max(triArea[o], 1e-12);
                open.push(o);
            }
        }
        c.n = c.sum.normalized();
        planeBasis(c.n, c.u, c.v);
        const double a = p.angle * kPi / 180;
        const Vec3 u2 = c.u * std::cos(a) + c.v * std::sin(a), v2 = c.v * std::cos(a) - c.u * std::sin(a);
        c.u = u2;
        c.v = v2;
        charts.push_back(c);
    }

    // Cells: triangles of a chart by the pattern cell of their centre, keeping
    // different materials apart.
    using CellKey = std::tuple<int, int, int, Id, Id>;
    std::map<CellKey, int> cellIds;
    std::vector<int> cluster(nt);
    std::vector<int> chartOf;
    std::vector<double> clusterArea;
    for (int t = 0; t < nt; ++t) {
        const auto& T = s.tris[t];
        const Chart& c = charts[chart[t]];
        const Vec3 centre = (s.verts[T.v[0]].p + s.verts[T.v[1]].p + s.verts[T.v[2]].p) / 3.0;
        auto cell = lattice.cell(Vec2(centre.dot(c.u), centre.dot(c.v)));
        CellKey key{chart[t], cell[0], cell[1], s.sources[T.src].front.material, s.sources[T.src].back.material};
        auto [it, fresh] = cellIds.emplace(key, (int)chartOf.size());
        if (fresh) {
            chartOf.push_back(chart[t]);
            clusterArea.push_back(0);
        }
        cluster[t] = it->second;
        clusterArea[it->second] += triArea[t];
    }

    // A cell's narrow tips can come apart from it at this resolution: pieces other
    // than a cell's largest go to the neighbour they share most outline with.
    for (int round = 0; round < 4; ++round) {
        std::vector<int> piece(nt, -1);
        std::vector<double> pieceArea;
        std::vector<int> pieceCluster;
        for (int seed = 0; seed < nt; ++seed) {
            if (piece[seed] >= 0) continue;
            const int id = (int)pieceArea.size();
            pieceArea.push_back(0);
            pieceCluster.push_back(cluster[seed]);
            std::vector<int> stack{seed};
            piece[seed] = id;
            while (!stack.empty()) {
                int t = stack.back();
                stack.pop_back();
                pieceArea[id] += triArea[t];
                for (int k = 0; k < 3; ++k) {
                    const auto& list = edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])];
                    if (list.size() != 2) continue;
                    int o = list[0] == t ? list[1] : list[0];
                    if (piece[o] < 0 && cluster[o] == cluster[t]) {
                        piece[o] = id;
                        stack.push_back(o);
                    }
                }
            }
        }
        std::vector<int> largest(chartOf.size(), -1);
        for (int i = 0; i < (int)pieceArea.size(); ++i) {
            int& best = largest[pieceCluster[i]];
            if (best < 0 || pieceArea[i] > pieceArea[best]) best = i;
        }
        std::vector<std::map<int, double>> neighbours(pieceArea.size());
        for (const auto& [key, list] : edgeTris) {
            if (list.size() != 2 || piece[list[0]] == piece[list[1]]) continue;
            const int a = list[0], b = list[1];
            if (chart[a] != chart[b]) continue;
            const Surface::Source& sa = s.sources[s.tris[a].src];
            const Surface::Source& sb = s.sources[s.tris[b].src];
            if (sa.front.material != sb.front.material || sa.back.material != sb.back.material) continue;
            const double len = (s.verts[int(key >> 32)].p - s.verts[int(key & 0xffffffffu)].p).length();
            neighbours[piece[a]][cluster[b]] += len;
            neighbours[piece[b]][cluster[a]] += len;
        }
        std::vector<int> moveTo(pieceArea.size(), -1);
        bool moved = false;
        for (int i = 0; i < (int)pieceArea.size(); ++i) {
            if (largest[pieceCluster[i]] == i) continue;
            double bestLen = 0;
            for (const auto& [c, len] : neighbours[i])
                if (c != pieceCluster[i] && len > bestLen) {
                    bestLen = len;
                    moveTo[i] = c;
                }
            moved = moved || moveTo[i] >= 0;
        }
        if (!moved) break;
        for (int t = 0; t < nt; ++t)
            if (moveTo[piece[t]] >= 0) {
                clusterArea[cluster[t]] -= triArea[t];
                cluster[t] = moveTo[piece[t]];
                clusterArea[cluster[t]] += triArea[t];
            }
    }

    // Slivers cut off by the outline join the neighbour they share most with.
    {
        const double sliver = 0.12 * lattice.cellArea();
        std::vector<int> parent(chartOf.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
        std::vector<std::map<int, double>> shared(chartOf.size());
        for (const auto& [key, list] : edgeTris) {
            if (list.size() != 2) continue;
            const int a = cluster[list[0]], b = cluster[list[1]];
            if (a == b || chartOf[a] != chartOf[b]) continue;
            if (s.sources[s.tris[list[0]].src].front.material != s.sources[s.tris[list[1]].src].front.material ||
                s.sources[s.tris[list[0]].src].back.material != s.sources[s.tris[list[1]].src].back.material) continue;
            const double len = (s.verts[int(key >> 32)].p - s.verts[int(key & 0xffffffffu)].p).length();
            shared[a][b] += len;
            shared[b][a] += len;
        }
        std::vector<int> order(chartOf.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return clusterArea[a] < clusterArea[b]; });
        for (int c : order) {
            if (clusterArea[c] >= sliver) break;
            int best = -1;
            double bestLen = 0;
            for (const auto& [n, len] : shared[c])
                if (find(n) != find(c) && len > bestLen) { bestLen = len; best = n; }
            if (best < 0) continue;
            const int rootB = find(best);
            parent[find(c)] = rootB;
            clusterArea[rootB] += clusterArea[c];
        }
        for (int t = 0; t < nt; ++t) cluster[t] = find(cluster[t]);
    }

    // Junctions inside a chart snap onto the exact pattern corner.
    std::vector<std::vector<int>> vTris(s.verts.size());
    for (int t = 0; t < nt; ++t)
        for (int v : s.tris[t].v) vTris[v].push_back(t);
    Surface::SnapFn snap = [&](int vertex, Vec3& pos, std::array<double, 3>& key) {
        const auto& around = vTris[vertex];
        if (around.empty()) return false;
        const Chart& c = charts[chart[around[0]]];
        const Vec3& q = s.verts[vertex].p;
        const Vec2 q2(q.dot(c.u), q.dot(c.v));
        Vec2 corner;
        lattice.cell(q2, &corner);
        if ((corner - q2).length() > fine * 2.5) return false;
        const Vec3 base = c.u * corner.x + c.v * corner.y;
        for (int t : around) {
            const double denom = triN[t].dot(c.n);
            if (denom < 0.1) continue;
            const double h = triN[t].dot(s.verts[s.tris[t].v[0]].p - base) / denom;
            pos = base + c.n * h;
            key = {std::round(corner.x / lattice.size * 1e6), std::round(corner.y / lattice.size * 1e6), 0};
            return true;
        }
        return false;
    };
    std::vector<Surface::Poly> polys = s.clusterPolys(cluster, chartOf, snap);
    return s.write(m, polys, false, true, p.smooth, out);
}

int BendParams::segmentCount() const {
    if (segments > 0) return segments;
    return std::max(2, (int)std::ceil(std::fabs(angle) / (7.5 * kPi / 180) - 1e-9));
}

double BendParams::radius() const { return std::fabs(angle) < 1e-9 ? 1e12 : length / angle; }

Vec3 BendParams::apply(const Vec3& p) const {
    if (std::fabs(angle) < 1e-9 || length <= 0) return p;
    const Vec3 side = axis.cross(dir);
    const Vec3 d = p - origin;
    const double s = d.dot(dir), w = d.dot(side), h = d.dot(axis);
    if (s <= 0) return p;
    const double R = length / angle;
    const double phi = angle * std::min(s, length) / length;
    const double r = R - w;
    const Vec3 centre = origin + side * R;
    Vec3 q = centre + (dir * std::sin(phi) - side * std::cos(phi)) * r + axis * h;
    if (s > length) q += (dir * std::cos(angle) + side * std::sin(angle)) * (s - length);
    return q;
}

bool BendParams::fits(const Vec3& p) const {
    if (std::fabs(angle) < 1e-9) return true;
    const Vec3 d = p - origin;
    const double s = d.dot(dir);
    // The ends of the stretch count: the arc starts or ends right there.
    if (s < -1e-9 || s > length + 1e-9) return true;
    return (radius() - d.dot(axis.cross(dir))) / radius() > 1e-6;
}

bool makeBend(const Vec3& a, const Vec3& b, const Vec3& axis, double angle, int segments, BendParams& out) {
    out.axis = axis.normalized();
    Vec3 along = b - a;
    along -= out.axis * along.dot(out.axis);
    if (along.length() < kTol * 10) return false;
    out.origin = a;
    out.dir = along.normalized();
    out.length = along.length();
    out.angle = angle;
    out.segments = segments;
    return true;
}

bool effectBend(Mesh& m, const std::set<Id>& faces, const std::set<Id>* edgesIn, const BendParams& p, EffectResult* out, std::string* error) {
    // Loose edges: those bounding no face at all.
    struct Loose {
        Id a, b;
        Vec3 pa, pb;
    };
    std::vector<Loose> loose;
    auto addLoose = [&](Id e) {
        if (e < 0 || e >= (Id)m.edges.size() || !m.edges[e].alive) return;
        for (Id f : m.edgeFaces(e))
            if (m.faces[f].alive) return;
        const Edge& E = m.edges[e];
        loose.push_back({E.v[0], E.v[1], m.pos(E.v[0]), m.pos(E.v[1])});
    };
    if (edgesIn) {
        for (Id e : *edgesIn) addLoose(e);
    } else {
        for (Id e = 0; e < (Id)m.edges.size(); ++e) addLoose(e);
    }

    Surface s;
    const bool hasFaces = s.extract(m, faces);
    if (!hasFaces && loose.empty()) {
        if (error) *error = "Select the geometry to bend first.";
        return false;
    }
    const auto tooTight = [&] {
        if (error) *error = "The bend is too tight for this geometry: it would turn inside out. Use a longer stretch or a smaller angle.";
        return false;
    };
    const int n = p.segmentCount();
    const double step = p.length / n;
    // A segment boundary strictly between two positions along the bend, if any.
    constexpr double kOnSlice = 1e-6;
    auto boundaryBetween = [&](double lo, double hi, double& at) {
        if (lo > hi) std::swap(lo, hi);
        for (int k = std::max(0, (int)std::ceil((lo + kOnSlice) / step)); k <= n; ++k) {
            const double x = k * step;
            if (x >= hi - kOnSlice) return false;
            if (x > lo + kOnSlice) {
                at = x;
                return true;
            }
        }
        return false;
    };
    auto along = [&](const Vec3& q) { return (q - p.origin).dot(p.dir); };

    // Checks every point that ends up on the arc, before anything is changed.
    for (const Loose& l : loose) {
        if (!p.fits(l.pa) || !p.fits(l.pb)) return tooTight();
        const double sa = along(l.pa), sb = along(l.pb);
        double lo = std::min(sa, sb), at;
        while (boundaryBetween(lo, std::max(sa, sb), at)) {
            if (!p.fits(lerp(l.pa, l.pb, (at - sa) / (sb - sa)))) return tooTight();
            lo = at;
        }
    }
    if (hasFaces) {
        // Cuts every edge where a segment boundary crosses it, so the surface
        // folds exactly along the arc's segments and each piece stays flat.
        s.refineWith([&](int a, int b, double& t) {
            const double sa = along(s.verts[a].p), sb = along(s.verts[b].p);
            double at;
            if (!boundaryBetween(sa, sb, at)) return 0.0;
            t = (at - sa) / (sb - sa);
            return std::fabs(sb - sa);
        });
        for (const Surface::V& v : s.verts)
            if (!v.pinned && !p.fits(v.p)) return tooTight();
        for (Surface::V& v : s.verts)
            if (!v.pinned) v.p = p.apply(v.p);
        s.write(m, flatPolys(s), false, false, false, out);
    }
    std::unordered_set<Id> moved;
    for (const Surface::V& v : s.verts)
        if (v.mesh != kNone) moved.insert(v.mesh);

    auto hasFace = [&](Id v) {
        for (Id e : m.vertexEdges(v))
            if (m.edges[e].alive)
                for (Id f : m.edgeFaces(e))
                    if (m.faces[f].alive) return true;
        return false;
    };
    static const std::vector<Id> kNoFaces;
    std::vector<std::pair<Id, Vec3>> finalPos;
    for (const Loose& l : loose) {
        // Cut at the segment boundaries; new vertices land on the arc.
        Id from = l.a;
        const double sa = along(l.pa), sb = along(l.pb);
        std::vector<double> cuts;
        double lo = std::min(sa, sb), at;
        while (boundaryBetween(lo, std::max(sa, sb), at)) {
            cuts.push_back(at);
            lo = at;
        }
        if (sa > sb) std::reverse(cuts.begin(), cuts.end());
        for (double x : cuts) {
            const Vec3 q = lerp(l.pa, l.pb, (x - sa) / (sb - sa));
            const Id v = m.addVertexRaw(q);
            Id e = m.findEdge(from, l.b);
            if (e == kNone) break;
            m.splitEdge(e, v, &kNoFaces);
            finalPos.emplace_back(v, p.apply(q));
            if (out) {
                Id piece = m.findEdge(from, v);
                if (piece != kNone) out->edges.insert(piece);
            }
            from = v;
        }
        if (out) {
            Id piece = m.findEdge(from, l.b);
            if (piece != kNone) out->edges.insert(piece);
        }
        // An end joined to faces outside the bend stays with them.
        if (moved.insert(l.a).second && !hasFace(l.a)) finalPos.emplace_back(l.a, p.apply(l.pa));
        if (moved.insert(l.b).second && !hasFace(l.b)) finalPos.emplace_back(l.b, p.apply(l.pb));
    }
    for (const auto& [v, q] : finalPos) m.vertices[v].p = q;
    m.touch();
    m.markGeometryMoved();
    return true;
}

bool applyBend(ModelState& st, Selection& sel, const BendParams& p, std::string* error) {
    bool any = false;
    if (!sel.faces.empty() || !sel.edges.empty()) {
        EffectResult r;
        if (!effectBend(st.activeMesh(), sel.faces, &sel.edges, p, &r, error)) return false;
        sel.faces = r.faces;
        sel.edges = r.edges;
        any = true;
    }
    if (!sel.instances.empty() && st.editing == kNone) {
        Selection instances;
        instances.instances = sel.instances;
        if (!applyEffect(st, instances, [&](Mesh& m, const std::set<Id>& f, EffectResult* r, std::string* e) {
                return effectBend(m, f, nullptr, p, r, e);
            }, nullptr, error))
            return false;
        any = true;
    }
    if (!any && error) *error = "Select the geometry to bend first.";
    return any;
}

namespace {

// Does a triangle overlap a box around the origin? Separating axis test.
bool triangleOverlapsBox(const Vec3& v0, const Vec3& v1, const Vec3& v2, const Vec3& half) {
    auto separated = [&](const Vec3& axis) {
        const double a = v0.dot(axis), b = v1.dot(axis), c = v2.dot(axis);
        const double r = half.x * std::fabs(axis.x) + half.y * std::fabs(axis.y) + half.z * std::fabs(axis.z);
        return std::min({a, b, c}) > r || std::max({a, b, c}) < -r;
    };
    const Vec3 e[3] = {v1 - v0, v2 - v1, v0 - v2};
    const Vec3 boxAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    for (const Vec3& b : boxAxes)
        if (separated(b)) return false;
    if (separated(e[0].cross(e[1]))) return false;
    for (const Vec3& ed : e)
        for (const Vec3& b : boxAxes)
            if (separated(ed.cross(b))) return false;
    return true;
}

} // namespace

bool effectVoxelate(Mesh& m, const std::set<Id>& faces, const VoxelateParams& p, EffectResult* out, std::string* error) {
    if (!(p.size > 0)) {
        if (error) *error = "The cube size must be above zero.";
        return false;
    }
    struct Tri {
        Vec3 a, b, c;
        Id face;
    };
    std::vector<Tri> tris;
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    for (Id f : faces) {
        if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) continue;
        const Face& F = m.faces[f];
        Vec3 u, v;
        planeBasis(F.normal, u, v);
        std::vector<std::vector<Vec2>> loops;
        std::vector<Vec3> pts;
        auto add = [&](const std::vector<Id>& L) {
            loops.emplace_back();
            for (Id id : L) {
                const Vec3& q = m.pos(id);
                loops.back().emplace_back(q.dot(u), q.dot(v));
                pts.push_back(q);
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], q[k]);
                    mx[k] = std::max(mx[k], q[k]);
                }
            }
        };
        add(F.outer);
        for (const auto& h : F.holes) add(h);
        for (const auto& t : triangulatePolygon(loops)) tris.push_back({pts[t[0]], pts[t[1]], pts[t[2]], f});
    }
    if (tris.empty()) {
        if (error) *error = "Select faces or components first.";
        return false;
    }

    constexpr int64_t kMaxCells = 32000000;
    double size = p.size;
    bool limited = false;
    int ix0, iy0, iz0, nx, ny, nz;
    std::vector<char> cell; // 0 empty, 1 filled, 2 outside
    std::unordered_map<int64_t, std::vector<Id>> sources;
    std::vector<int> quads; // cell index * 6 + direction
    auto index = [&](int x, int y, int z) { return (int64_t(z) * ny + y) * nx + x; };
    static const int kDir[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (;;) {
        // One empty layer all around, for the flood from outside.
        ix0 = (int)std::floor(mn.x / size) - 2;
        iy0 = (int)std::floor(mn.y / size) - 2;
        iz0 = (int)std::floor(mn.z / size) - 2;
        nx = (int)std::ceil(mx.x / size) + 2 - ix0;
        ny = (int)std::ceil(mx.y / size) + 2 - iy0;
        nz = (int)std::ceil(mx.z / size) + 2 - iz0;
        if (double(nx) * ny * nz > double(kMaxCells)) {
            size *= 1.26;
            limited = true;
            continue;
        }
        cell.assign(size_t(nx) * ny * nz, 0);
        sources.clear();
        // A face lying exactly on a grid plane counts for the cubes behind it, so a
        // box that fits the grid does not grow by a layer. Touching does not count.
        const double inward = size * 1e-4;
        const Vec3 half = Vec3(size, size, size) * (0.5 * (1 - 1e-6));
        for (const Tri& t : tris) {
            const Vec3 shift = m.faces[t.face].normal * -inward;
            const Vec3 a = t.a + shift, b = t.b + shift, c = t.c + shift;
            auto range = [&](int k, int i0, int n, int& lo, int& hi) {
                lo = std::clamp((int)std::floor(std::min({a[k], b[k], c[k]}) / size) - i0, 0, n - 1);
                hi = std::clamp((int)std::floor(std::max({a[k], b[k], c[k]}) / size) - i0, 0, n - 1);
            };
            int x0, x1, y0, y1, z0, z1;
            range(0, ix0, nx, x0, x1);
            range(1, iy0, ny, y0, y1);
            range(2, iz0, nz, z0, z1);
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const Vec3 centre((ix0 + x + 0.5) * size, (iy0 + y + 0.5) * size, (iz0 + z + 0.5) * size);
                        if (!triangleOverlapsBox(a - centre, b - centre, c - centre, half)) continue;
                        const int64_t i = index(x, y, z);
                        cell[i] = 1;
                        std::vector<Id>& src = sources[i];
                        if (src.size() < 6 && std::find(src.begin(), src.end(), t.face) == src.end()) src.push_back(t.face);
                    }
        }
        // What cannot be reached from outside is inside. An open surface has no
        // inside: the flood goes around it.
        std::vector<int64_t> stack{0};
        cell[0] = 2;
        while (!stack.empty()) {
            const int64_t i = stack.back();
            stack.pop_back();
            const int z = int(i / (int64_t(nx) * ny)), y = int((i / nx) % ny), x = int(i % nx);
            for (const auto& d : kDir) {
                const int X = x + d[0], Y = y + d[1], Z = z + d[2];
                if (X < 0 || Y < 0 || Z < 0 || X >= nx || Y >= ny || Z >= nz) continue;
                const int64_t j = index(X, Y, Z);
                if (cell[j] != 0) continue;
                cell[j] = 2;
                stack.push_back(j);
            }
        }
        if (p.fill)
            for (char& c : cell)
                if (c == 0) c = 1;
        // Sides between a cube and what is not one.
        quads.clear();
        bool tooMany = false;
        for (int z = 0; z < nz && !tooMany; ++z)
            for (int y = 0; y < ny && !tooMany; ++y)
                for (int x = 0; x < nx; ++x) {
                    if (cell[index(x, y, z)] != 1) continue;
                    for (int k = 0; k < 6; ++k)
                        if (cell[index(x + kDir[k][0], y + kDir[k][1], z + kDir[k][2])] != 1) quads.push_back(int(k));
                    if ((int)quads.size() > kEffectTriangleLimit) {
                        tooMany = true;
                        break;
                    }
                }
        if (!tooMany) break;
        size *= 1.26;
        limited = true;
    }

    const Id fallback = tris[0].face;
    // Keep the source sides before the faces go.
    std::map<Id, Face> sourceFaces;
    for (const Tri& t : tris) {
        if (sourceFaces.count(t.face)) continue;
        Face F = m.faces[t.face];
        F.outer.clear();
        F.holes.clear();
        sourceFaces.emplace(t.face, std::move(F));
    }
    // The old surface goes, with the edges only it used.
    std::set<Id> oldEdges;
    for (Id f : faces)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive)
            for (Id e : m.faceEdges(f)) oldEdges.insert(e);
    for (Id f : faces)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive) m.eraseFace(f);
    for (Id e : oldEdges) {
        bool used = false;
        for (Id f : m.edgeFaces(e)) used = used || m.faces[f].alive;
        if (!used) m.eraseEdge(e, false);
    }
    m.removeOrphanVertices();

    std::unordered_map<int64_t, Id> corners;
    const int64_t cx = int64_t(nx) + 1, cy = int64_t(ny) + 1;
    auto corner = [&](int x, int y, int z) {
        const int64_t key = (int64_t(z) * cy + y) * cx + x;
        auto it = corners.find(key);
        if (it != corners.end()) return it->second;
        const Id v = m.addVertexRaw(Vec3((ix0 + x) * size, (iy0 + y) * size, (iz0 + z) * size));
        corners.emplace(key, v);
        return v;
    };
    std::set<Id> made;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                const int64_t i = index(x, y, z);
                if (cell[i] != 1) continue;
                for (int k = 0; k < 6; ++k) {
                    if (cell[index(x + kDir[k][0], y + kDir[k][1], z + kDir[k][2])] == 1) continue;
                    const int axis = k / 2, high = k % 2;
                    // The four corners of that side, counter-clockwise seen from outside.
                    int c[4][3];
                    for (int n = 0; n < 4; ++n) {
                        const int a = (n == 1 || n == 2) ? 1 : 0, b = (n >= 2) ? 1 : 0;
                        const int ua = (axis + 1) % 3, ub = (axis + 2) % 3;
                        const int base[3] = {x, y, z};
                        c[n][axis] = base[axis] + high;
                        c[n][ua] = base[ua] + a;
                        c[n][ub] = base[ub] + b;
                    }
                    std::vector<Id> loop;
                    for (int n = 0; n < 4; ++n) {
                        const int o = high ? n : 3 - n;
                        loop.push_back(corner(c[o][0], c[o][1], c[o][2]));
                    }
                    for (int n = 0; n < 4; ++n) m.addEdgeRaw(loop[n], loop[(n + 1) % 4]);
                    Vec3 nrm;
                    nrm[axis] = high ? 1 : -1;
                    const Id nf = m.addFaceRaw(loop, {}, nrm);
                    if (nf == kNone) continue;
                    // The look of the source facing most this way.
                    Id best = kNone;
                    double bestDot = -2;
                    auto s = sources.find(i);
                    if (s != sources.end())
                        for (Id src : s->second) {
                            const double dd = std::fabs(sourceFaces.at(src).normal.dot(nrm));
                            if (dd > bestDot) {
                                bestDot = dd;
                                best = src;
                            }
                        }
                    if (best == kNone) best = fallback;
                    const Face& S = sourceFaces.at(best);
                    const bool same = S.normal.dot(nrm) >= 0;
                    Face& N = m.faces[nf];
                    N.front = same ? S.front : S.back;
                    N.back = same ? S.back : S.front;
                    // A placed image only makes sense on sides facing about the same way.
                    if (std::fabs(S.normal.dot(nrm)) < 0.5) N.front.tex.valid = N.back.tex.valid = false;
                    N.front.axis = N.back.axis = Vec3();
                    made.insert(nf);
                }
            }
    m.touch();
    m.markGeometryMoved();
    if (p.mergeFlat) {
        EffectResult merged;
        if (effectMergeFlat(m, made, &merged, nullptr)) made = merged.faces;
    }
    if (out) {
        out->faces = made;
        out->limited = limited;
    }
    return true;
}

int effectFaceCount(const ModelState& st, const Selection& sel) {
    int n = (int)sel.faces.size();
    if (st.editing != kNone) return n;
    std::set<Id> defs;
    for (Id i : sel.instances)
        if (i >= 0 && i < (Id)st.instances.size() && st.instances[i].alive) defs.insert(st.instances[i].def);
    for (Id d : defs) n += st.defs[d].mesh.aliveFaceCount();
    return n;
}

bool applyEffect(ModelState& st, Selection& sel, const EffectFn& fn, bool* limited, std::string* error) {
    bool any = false;
    auto track = [&](const EffectResult& r) {
        any = true;
        if (limited && r.limited) *limited = true;
    };
    if (!sel.faces.empty()) {
        EffectResult r;
        if (!fn(st.activeMesh(), sel.faces, &r, error)) return false;
        sel.faces = r.faces;
        track(r);
    }
    if (!sel.instances.empty() && st.editing == kNone) {
        std::set<Id> insts;
        for (Id i : sel.instances)
            if (i >= 0 && i < (Id)st.instances.size() && st.instances[i].alive) insts.insert(i);
        makeUnique(st, insts);
        std::set<Id> doneDefs;
        for (Id i : insts) {
            if (!doneDefs.insert(st.instances[i].def).second) continue;
            openComponent(st, i);
            Mesh& m = st.activeMesh();
            std::set<Id> all;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && !m.faces[f].hidden) all.insert(f);
            EffectResult r;
            const bool ok = fn(m, all, &r, error);
            closeComponent(st);
            if (!ok) return false;
            track(r);
        }
    }
    if (!any) {
        if (error) *error = "Select faces or components first.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// De-triangulate, Hollow, Solidify
// ---------------------------------------------------------------------------

namespace {

using Tri3 = std::array<Vec3, 3>;

// Lines parallel to one axis through a set of triangles: where they cross them.
class AxisRays {
public:
    AxisRays(const std::vector<Tri3>& tris, int axis, double bucket) : tris_(tris), a_(axis), b_((axis + 1) % 3), c_((axis + 2) % 3) {
        bucket_ = std::max(bucket, 1e-9);
        for (int t = 0; t < (int)tris.size(); ++t) {
            double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
            for (const Vec3& p : tris[t]) {
                lo[0] = std::min(lo[0], p[b_]), hi[0] = std::max(hi[0], p[b_]);
                lo[1] = std::min(lo[1], p[c_]), hi[1] = std::max(hi[1], p[c_]);
            }
            for (int64_t i = cell(lo[0]); i <= cell(hi[0]); ++i)
                for (int64_t j = cell(lo[1]); j <= cell(hi[1]); ++j) grid_[key(i, j)].push_back(t);
        }
    }
    // Sorted positions along the axis where the line at (b, c) crosses a triangle.
    std::vector<double> crossings(double b, double c) const {
        std::vector<double> out;
        auto it = grid_.find(key(cell(b), cell(c)));
        if (it == grid_.end()) return out;
        for (int t : it->second) {
            const Tri3& T = tris_[t];
            const double x0 = T[0][b_] - b, y0 = T[0][c_] - c, x1 = T[1][b_] - b, y1 = T[1][c_] - c, x2 = T[2][b_] - b, y2 = T[2][c_] - c;
            const double w0 = x1 * y2 - x2 * y1, w1 = x2 * y0 - x0 * y2, w2 = x0 * y1 - x1 * y0;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) continue;
            const double sum = w0 + w1 + w2;
            if (std::fabs(sum) < 1e-30) continue; // seen edge-on
            out.push_back((w0 * T[0][a_] + w1 * T[1][a_] + w2 * T[2][a_]) / sum);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    // Odd number of crossings on the positive side.
    bool inside(const Vec3& p) const {
        int n = 0;
        for (double x : crossings(p[b_], p[c_])) n += x > p[a_];
        return n % 2 == 1;
    }

private:
    int64_t cell(double v) const { return (int64_t)std::floor(v / bucket_); }
    static uint64_t key(int64_t i, int64_t j) { return (uint64_t(uint32_t(int32_t(i))) << 32) | uint32_t(int32_t(j)); }
    const std::vector<Tri3>& tris_;
    int a_, b_, c_;
    double bucket_;
    std::unordered_map<uint64_t, std::vector<int>> grid_;
};

// Inside by the majority of three axis rays (slightly off the grid, so they
// do not run along edges).
struct InsideTest {
    explicit InsideTest(const std::vector<Tri3>& tris, double bucket)
        : x(tris, 0, bucket), y(tris, 1, bucket), z(tris, 2, bucket) {}
    bool operator()(Vec3 p) const {
        p += Vec3(1.37e-7, 2.71e-7, 3.14e-7);
        return int(x.inside(p)) + int(y.inside(p)) + int(z.inside(p)) >= 2;
    }
    AxisRays x, y, z;
};

Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    // Ericson, Real-Time Collision Detection 5.1.5.
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double den = 1.0 / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

// Triangle / box overlap (Akenine-Moller's separating axes).
bool triBoxOverlap(const Vec3& centre, const Vec3& half, const Tri3& tri) {
    const Vec3 v0 = tri[0] - centre, v1 = tri[1] - centre, v2 = tri[2] - centre;
    const Vec3 e[3] = {v1 - v0, v2 - v1, v0 - v2};
    for (int i = 0; i < 3; ++i) {
        const double lo = std::min({v0[i], v1[i], v2[i]}), hi = std::max({v0[i], v1[i], v2[i]});
        if (lo > half[i] || hi < -half[i]) return false;
    }
    const Vec3 n = e[0].cross(e[1]);
    const double r0 = half.x * std::fabs(n.x) + half.y * std::fabs(n.y) + half.z * std::fabs(n.z);
    if (std::fabs(n.dot(v0)) > r0) return false;
    for (const Vec3& ed : e)
        for (int k = 0; k < 3; ++k) {
            Vec3 axis;
            axis[k] = 1;
            axis = axis.cross(ed);
            if (axis.length2() < 1e-30) continue;
            const double p0 = axis.dot(v0), p1 = axis.dot(v1), p2 = axis.dot(v2);
            const double r = half.x * std::fabs(axis.x) + half.y * std::fabs(axis.y) + half.z * std::fabs(axis.z);
            if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r) return false;
        }
    return true;
}

std::vector<Tri3> surfaceTriangles(const Surface& s) {
    std::vector<Tri3> out;
    out.reserve(s.tris.size());
    for (const auto& t : s.tris) out.push_back({s.verts[t.v[0]].p, s.verts[t.v[1]].p, s.verts[t.v[2]].p});
    return out;
}

void triBounds(const std::vector<Tri3>& tris, Vec3& mn, Vec3& mx) {
    mn = Vec3(1e300, 1e300, 1e300);
    mx = Vec3(-1e300, -1e300, -1e300);
    for (const Tri3& t : tris)
        for (const Vec3& p : t)
            for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], p[k]), mx[k] = std::max(mx[k], p[k]);
}

// Most used front material of the faces.
Id commonMaterial(const Mesh& m, const std::set<Id>& faces) {
    std::map<Id, int> count;
    for (Id f : faces)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive) count[m.faces[f].front.material]++;
    Id best = kNone;
    int n = 0;
    for (const auto& [mat, c] : count)
        if (c > n) n = c, best = mat;
    return best;
}

} // namespace

bool effectDetriangulate(Mesh& m, const std::set<Id>& faces, const DetriangulateParams& p, EffectResult* out, std::string* error) {
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    Vec3 mn, mx;
    triBounds(surfaceTriangles(s), mn, mx);
    const double angle = std::clamp(p.angle, 0.0, 10.0) * kPi / 180;
    // Flat within the angle, and on one plane within a hair of the model's size
    // (STL coordinates are single precision).
    const double distTol = std::max(kTol, (mx - mn).length() * 2e-6);
    std::vector<int> group = faceAdjacencyFlat(s, std::cos(angle) - 1e-12, distTol);
    int groups = 0;
    for (int g : group) groups = std::max(groups, g + 1);

    if (p.quads) {
        // Pairs of the single triangles left: across their longest edge, bent
        // less than the angle, making a convex quad. Flattest pairs first.
        std::vector<int> size(groups, 0);
        for (int g : group) size[g]++;
        const int nt = (int)s.tris.size();
        std::unordered_map<uint64_t, std::vector<int>> edgeTris;
        for (int t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k) edgeTris[edgeKey(s.tris[t].v[k], s.tris[t].v[(k + 1) % 3])].push_back(t);
        auto longest = [&](int t) {
            int best = 0;
            double len = -1;
            for (int k = 0; k < 3; ++k) {
                const double l = (s.verts[s.tris[t].v[(k + 1) % 3]].p - s.verts[s.tris[t].v[k]].p).length2();
                if (l > len) len = l, best = k;
            }
            return edgeKey(s.tris[t].v[best], s.tris[t].v[(best + 1) % 3]);
        };
        const double cosQuad = std::cos(std::clamp(p.quadAngle, 0.0, 89.0) * kPi / 180);
        std::vector<std::tuple<double, int, int>> pairs;
        for (const auto& [key, list] : edgeTris) {
            if (list.size() != 2) continue;
            const int a = list[0], b = list[1];
            if (size[group[a]] != 1 || size[group[b]] != 1 || longest(a) != key || longest(b) != key) continue;
            if (s.tris[a].src != s.tris[b].src) {
                const auto &fa = s.sources[s.tris[a].src].front, &fb = s.sources[s.tris[b].src].front;
                if (fa.material != fb.material) continue;
            }
            const Vec3 na = s.triNormal(s.tris[a]).normalized(), nb = s.triNormal(s.tris[b]).normalized();
            const double c = na.dot(nb);
            if (c < cosQuad) continue;
            // Convex: the far corners lie on opposite sides of the shared edge, and
            // the edge's ends on opposite sides of the line between the far corners.
            int ea = -1, eb = -1, oa = -1, ob = -1;
            for (int k = 0; k < 3; ++k) {
                const int v = s.tris[a].v[k];
                if (v != s.tris[b].v[0] && v != s.tris[b].v[1] && v != s.tris[b].v[2]) oa = v;
                else (ea < 0 ? ea : eb) = v;
            }
            for (int v : s.tris[b].v)
                if (v != ea && v != eb) ob = v;
            if (oa < 0 || ob < 0 || eb < 0) continue;
            const Vec3 n = (na + nb).normalized();
            const Vec3 P = s.verts[oa].p, Q = s.verts[ob].p, E0 = s.verts[ea].p, E1 = s.verts[eb].p;
            const double s0 = (Q - P).cross(E0 - P).dot(n), s1 = (Q - P).cross(E1 - P).dot(n);
            if (s0 * s1 >= 0) continue;
            pairs.emplace_back(-c, a, b);
        }
        std::sort(pairs.begin(), pairs.end());
        std::vector<char> used(nt, 0);
        for (const auto& [neg, a, b] : pairs) {
            if (used[a] || used[b]) continue;
            used[a] = used[b] = 1;
            group[b] = group[a];
        }
    }
    // Renumber the groups (pairs left gaps) and build the polygons.
    std::map<int, int> renum;
    for (int& g : group) g = renum.emplace(g, (int)renum.size()).first->second;
    std::vector<int> chart(renum.size());
    std::iota(chart.begin(), chart.end(), 0);
    return s.write(m, s.clusterPolys(group, chart, nullptr), false, true, true, out);
}

bool effectHollow(Mesh& m, const std::set<Id>& faces, const HollowParams& p, EffectResult* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (!(p.thickness > 0)) return fail("The wall thickness must be above zero.");
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    {
        std::unordered_map<uint64_t, int> use;
        for (const auto& t : s.tris)
            for (int k = 0; k < 3; ++k) use[edgeKey(t.v[k], t.v[(k + 1) % 3])]++;
        for (const auto& [k, n] : use)
            if (n != 2) return fail("Hollow needs a closed solid: these faces have open edges or edges shared by more than two faces. Solidify can make one.");
    }
    const std::vector<Tri3> tris = surfaceTriangles(s);
    Vec3 mn, mx;
    triBounds(tris, mn, mx);
    const Vec3 ext = mx - mn;
    const double longest = std::max({ext.x, ext.y, ext.z});
    const double t = p.thickness;
    // Grid: `detail` cells along the longest side, and at least two per wall;
    // at most about eight million points.
    double cell = std::min(longest / std::clamp(p.detail, 8, 400), t / 2);
    bool limited = false;
    const double minCell = std::cbrt(std::max(ext.x, cell) * std::max(ext.y, cell) * std::max(ext.z, cell) / 8e6);
    if (cell < minCell) {
        cell = minCell;
        limited = true;
    }
    const int nx = (int)std::ceil(ext.x / cell) + 1, ny = (int)std::ceil(ext.y / cell) + 1, nz = (int)std::ceil(ext.z / cell) + 1;
    auto idx = [&](int i, int j, int k) { return (size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * k); };
    auto point = [&](int i, int j, int k) { return mn + Vec3(i * cell, j * cell, k * cell); };

    // Inside: rows along x, crossing the surface toggles.
    const AxisRays rays(tris, 0, cell * 4);
    std::vector<char> inside((size_t)nx * ny * nz, 0);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j) {
            const Vec3 q = point(0, j, k) + Vec3(0, 1.37e-7, 2.71e-7);
            const std::vector<double> xs = rays.crossings(q.y, q.z);
            size_t c = 0;
            for (int i = 0; i < nx; ++i) {
                const double x = mn.x + i * cell;
                while (c < xs.size() && xs[c] < x) ++c;
                inside[idx(i, j, k)] = (xs.size() - c) % 2 == 1;
            }
        }

    // Distance to the surface (only up to the wall and a little more matters).
    const double R = t + 2 * cell;
    std::unordered_map<uint64_t, std::vector<int>> bins;
    auto bin = [&](double v) { return (int64_t)std::floor(v / R); };
    auto binKey = [](int64_t a, int64_t b, int64_t c) { return (uint64_t(a & 0x1FFFFF) << 42) | (uint64_t(b & 0x1FFFFF) << 21) | uint64_t(c & 0x1FFFFF); };
    for (int ti = 0; ti < (int)tris.size(); ++ti) {
        Vec3 lo, hi;
        triBounds({tris[ti]}, lo, hi);
        for (int64_t a = bin(lo.x); a <= bin(hi.x); ++a)
            for (int64_t b = bin(lo.y); b <= bin(hi.y); ++b)
                for (int64_t c = bin(lo.z); c <= bin(hi.z); ++c) bins[binKey(a, b, c)].push_back(ti);
    }
    std::vector<float> f((size_t)nx * ny * nz, -1.0f);
    bool anyCavity = false;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) continue;
                const Vec3 q = point(i, j, k);
                double best = R * R;
                const int64_t a0 = bin(q.x), b0 = bin(q.y), c0 = bin(q.z);
                for (int64_t a = a0 - 1; a <= a0 + 1; ++a)
                    for (int64_t b = b0 - 1; b <= b0 + 1; ++b)
                        for (int64_t c = c0 - 1; c <= c0 + 1; ++c) {
                            auto it = bins.find(binKey(a, b, c));
                            if (it == bins.end()) continue;
                            for (int ti : it->second) best = std::min(best, (closestOnTriangle(q, tris[ti][0], tris[ti][1], tris[ti][2]) - q).length2());
                        }
                const double v = (std::sqrt(best) - t) / cell; // > 0 in the cavity
                f[idx(i, j, k)] = (float)v;
                anyCavity = anyCavity || v > 0;
            }
    if (!anyCavity) return fail("The walls would fill the whole object: make them thinner.");

    // The cavity's surface: marching tetrahedra (six per cube, all along the
    // cube's main diagonal, so neighbouring cubes agree).
    static const int tets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};
    std::unordered_map<uint64_t, int> edgeVert;
    std::vector<Vec3> verts;
    std::vector<std::array<int, 3>> out3;
    const size_t total = (size_t)nx * ny * nz;
    auto edgePoint = [&](size_t a, size_t b, const Vec3& pa, const Vec3& pb) {
        const uint64_t key = std::min(a, b) * total + std::max(a, b);
        auto it = edgeVert.find(key);
        if (it != edgeVert.end()) return it->second;
        const double fa = f[a], fb = f[b];
        const double w = std::clamp(fa / (fa - fb), 0.0, 1.0);
        verts.push_back(pa + (pb - pa) * w);
        edgeVert[key] = (int)verts.size() - 1;
        return (int)verts.size() - 1;
    };
    for (int k = 0; k + 1 < nz; ++k)
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                size_t id[8];
                Vec3 pos[8];
                bool anyPos = false, anyNeg = false;
                for (int c = 0; c < 8; ++c) {
                    id[c] = idx(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    pos[c] = point(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    (f[id[c]] > 0 ? anyPos : anyNeg) = true;
                }
                if (!anyPos || !anyNeg) continue;
                for (const auto& tet : tets) {
                    int posV[4], negV[4], np = 0, nn = 0;
                    for (int c : tet) (f[id[c]] > 0 ? posV[np++] : negV[nn++]) = c;
                    if (np == 0 || nn == 0) continue;
                    Vec3 cp, cn;
                    for (int q = 0; q < np; ++q) cp += pos[posV[q]];
                    for (int q = 0; q < nn; ++q) cn += pos[negV[q]];
                    const Vec3 toCavity = cp / double(np) - cn / double(nn);
                    auto E = [&](int a, int b) { return edgePoint(id[a], id[b], pos[a], pos[b]); };
                    auto emitTri = [&](int a, int b, int c) {
                        if (a == b || b == c || a == c) return;
                        const Vec3 n = (verts[b] - verts[a]).cross(verts[c] - verts[a]);
                        if (n.dot(toCavity) < 0) std::swap(b, c);
                        out3.push_back({a, b, c});
                    };
                    if (np == 1 || nn == 1) {
                        const bool single = np == 1;
                        const int lone = single ? posV[0] : negV[0];
                        const int* others = single ? negV : posV;
                        emitTri(E(lone, others[0]), E(lone, others[1]), E(lone, others[2]));
                    } else {
                        const int a = posV[0], b = posV[1], c = negV[0], d = negV[1];
                        const int ac = E(a, c), bc = E(b, c), bd = E(b, d), ad = E(a, d);
                        emitTri(ac, bc, bd);
                        emitTri(ac, bd, ad);
                    }
                }
            }
    if (out3.empty()) return fail("The walls would fill the whole object: make them thinner.");

    // Into the mesh, facing the cavity, in the faces' usual material.
    const Id mat = commonMaterial(m, faces);
    std::vector<Id> vid(verts.size(), kNone);
    std::set<Id> inner;
    for (const auto& tri : out3) {
        Id ids[3];
        for (int q = 0; q < 3; ++q) {
            if (vid[tri[q]] == kNone) vid[tri[q]] = m.addVertex(verts[tri[q]]);
            ids[q] = vid[tri[q]];
        }
        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
        const Vec3 n = (m.pos(ids[1]) - m.pos(ids[0])).cross(m.pos(ids[2]) - m.pos(ids[0]));
        if (n.length2() < 1e-30) continue;
        const Id nf = m.addFaceRaw({ids[0], ids[1], ids[2]}, {}, n);
        if (nf == kNone) continue;
        m.faces[nf].front.material = m.faces[nf].back.material = mat;
        inner.insert(nf);
    }
    // Flat parts of the wall as n-gons, curved ones smooth.
    EffectResult flat;
    if (!inner.empty() && effectDetriangulate(m, inner, DetriangulateParams{0.5, false, 30}, &flat)) inner = flat.faces;
    for (Id fi : inner)
        for (Id e : m.faceEdges(fi)) {
            const auto& fs = m.edgeFaces(e);
            if (fs.size() == 2 && m.faces[fs[0]].normal.dot(m.faces[fs[1]].normal) > 0.85) m.edges[e].soft = m.edges[e].smooth = true;
        }
    m.touch();
    if (out) {
        out->faces = inner;
        for (Id fo : faces)
            if (fo >= 0 && fo < (Id)m.faces.size() && m.faces[fo].alive) out->faces.insert(fo);
        out->limited = limited;
    }
    return true;
}

bool effectSolidify(Mesh& m, const std::set<Id>& facesIn, const SolidifyParams& p, EffectResult* out, std::string* error) {
    std::set<Id> faces;
    for (Id f : facesIn)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive) faces.insert(f);
    if (faces.empty()) return emptyError(error);

    // 1. Overlapping closed parts become one (their inner faces go).
    if (p.mergeParts) {
        // Parts: faces connected through vertices.
        std::map<Id, int> partOf;
        std::vector<std::set<Id>> parts;
        for (Id f0 : faces) {
            if (partOf.count(f0)) continue;
            parts.emplace_back();
            std::vector<Id> stack{f0};
            partOf[f0] = (int)parts.size() - 1;
            while (!stack.empty()) {
                const Id f = stack.back();
                stack.pop_back();
                parts.back().insert(f);
                for (Id v : m.faceVertices(f))
                    for (Id e : m.vertexEdges(v))
                        for (Id g : m.edgeFaces(e))
                            if (faces.count(g) && !partOf.count(g)) {
                                partOf[g] = (int)parts.size() - 1;
                                stack.push_back(g);
                            }
            }
        }
        if (parts.size() > 1) {
            Mesh merged;
            bool have = false, changed = false;
            std::set<Id> consumed;
            for (const std::set<Id>& part : parts) {
                Mesh pm;
                appendMesh(pm, m, Mat4(), &part, nullptr, false, nullptr);
                if (!isSolid(pm)) continue;
                if (!have) {
                    merged = std::move(pm);
                    have = true;
                    consumed.insert(part.begin(), part.end());
                    continue;
                }
                Mesh u;
                if (booleanMeshes(merged, pm, BoolOp::Union, u)) {
                    merged = std::move(u);
                    consumed.insert(part.begin(), part.end());
                    changed = true;
                }
            }
            if (changed) {
                std::vector<Id> edges;
                for (Id f : consumed) {
                    for (Id e : m.faceEdges(f)) edges.push_back(e);
                    m.eraseFace(f);
                    faces.erase(f);
                }
                m.removeEdgesWithoutFaces(edges);
                m.removeOrphanVertices();
                Selection created;
                appendMesh(m, merged, Mat4(), nullptr, nullptr, false, &created);
                faces.insert(created.faces.begin(), created.faces.end());
            }
        }
    }

    // 2. Only what can be seen from outside stays.
    Surface s;
    if (!s.extract(m, faces)) return emptyError(error);
    const std::vector<Tri3> tris = surfaceTriangles(s);
    Vec3 mn, mx;
    triBounds(tris, mn, mx);
    const Vec3 ext = mx - mn;
    const double longest = std::max({ext.x, ext.y, ext.z, 1e-6});
    double cell = longest / std::clamp(p.detail, 16, 400);
    // Pad by two cells so the outside goes all the way round.
    const Vec3 origin = mn - Vec3(2 * cell, 2 * cell, 2 * cell);
    const int nx = (int)std::ceil(ext.x / cell) + 5, ny = (int)std::ceil(ext.y / cell) + 5, nz = (int)std::ceil(ext.z / cell) + 5;
    const size_t total = (size_t)nx * ny * nz;
    auto idx = [&](int i, int j, int k) { return (size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * k); };
    std::vector<char> wall(total, 0), outside(total, 0);
    const Vec3 half(cell * 0.5, cell * 0.5, cell * 0.5);
    for (const Tri3& t : tris) {
        Vec3 lo, hi;
        triBounds({t}, lo, hi);
        const int i0 = std::max(0, (int)std::floor((lo.x - origin.x) / cell)), i1 = std::min(nx - 1, (int)std::floor((hi.x - origin.x) / cell));
        const int j0 = std::max(0, (int)std::floor((lo.y - origin.y) / cell)), j1 = std::min(ny - 1, (int)std::floor((hi.y - origin.y) / cell));
        const int k0 = std::max(0, (int)std::floor((lo.z - origin.z) / cell)), k1 = std::min(nz - 1, (int)std::floor((hi.z - origin.z) / cell));
        for (int k = k0; k <= k1; ++k)
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const size_t c = idx(i, j, k);
                    if (wall[c]) continue;
                    if (triBoxOverlap(origin + Vec3((i + 0.5) * cell, (j + 0.5) * cell, (k + 0.5) * cell), half, t)) wall[c] = 1;
                }
    }
    // Flood the outside from the border.
    std::vector<size_t> queue;
    auto seed = [&](int i, int j, int k) {
        const size_t c = idx(i, j, k);
        if (!wall[c] && !outside[c]) outside[c] = 1, queue.push_back(c);
    };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1) seed(i, j, k);
    for (size_t q = 0; q < queue.size(); ++q) {
        const size_t c = queue[q];
        const int i = int(c % nx), j = int((c / nx) % ny), k = int(c / ((size_t)nx * ny));
        if (i > 0) seed(i - 1, j, k);
        if (i + 1 < nx) seed(i + 1, j, k);
        if (j > 0) seed(i, j - 1, k);
        if (j + 1 < ny) seed(i, j + 1, k);
        if (k > 0) seed(i, j, k - 1);
        if (k + 1 < nz) seed(i, j, k + 1);
    }
    // From a point on a face, away along d: is the first open cell outside?
    // (-1: only walls within reach.)
    auto reachOutside = [&](const Vec3& pt, const Vec3& d) {
        for (int step = 1; step <= 8; ++step) {
            const Vec3 q = pt + d * (step * cell * 0.5);
            const int i = (int)std::floor((q.x - origin.x) / cell), j = (int)std::floor((q.y - origin.y) / cell),
                      k = (int)std::floor((q.z - origin.z) / cell);
            if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return 1;
            const size_t c = idx(i, j, k);
            if (!wall[c]) return outside[c] ? 1 : 0;
        }
        return -1;
    };
    const InsideTest insideTest(tris, cell * 4);
    const double eps = std::max(cell * 0.05, kTol * 10);
    std::vector<Id> drop, flip, keep;
    for (Id f : faces) {
        const Vec3 pt = m.faceInteriorPoint(f), n = m.faces[f].normal;
        const int fe = reachOutside(pt, n), be = reachOutside(pt, -n);
        const bool fi = insideTest(pt + n * eps), bi = insideTest(pt - n * eps);
        // Material behind: inside by the rays, or closed off from outside.
        const bool keepIt = fe == 1 && (bi || be == 0);
        const bool flipIt = !keepIt && be == 1 && (fi || fe == 0);
        if (keepIt) keep.push_back(f);
        else if (flipIt) flip.push_back(f);
        else drop.push_back(f);
    }
    if (keep.empty() && flip.empty()) {
        if (error) *error = "Nothing of this is a solid seen from outside.";
        return false;
    }
    std::vector<Id> loose;
    for (Id f : drop) {
        for (Id e : m.faceEdges(f)) loose.push_back(e);
        m.eraseFace(f);
    }
    for (Id f : flip) m.reverseFace(f);
    m.removeEdgesWithoutFaces(loose);
    m.removeOrphanVertices();
    m.touch();
    if (out) {
        out->faces.clear();
        out->faces.insert(keep.begin(), keep.end());
        out->faces.insert(flip.begin(), flip.end());
    }
    return true;
}

} // namespace gs
