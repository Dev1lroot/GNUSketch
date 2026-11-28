// SPDX-License-Identifier: GPL-3.0-or-later
#include "Solids.h"

#include "Components.h"
#include "Slicer.h"
#include "Triangulate.h"

#include <array>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace forma {

namespace {

using Tri = std::array<Vec3, 3>;

struct Seg {
    Vec3 a, b;
};

struct Box {
    Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    void grow(const Vec3& p) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], p[i]);
            mx[i] = std::max(mx[i], p[i]);
        }
    }
    bool overlaps(const Box& o, double tol) const {
        for (int i = 0; i < 3; ++i)
            if (mn[i] > o.mx[i] + tol || o.mn[i] > mx[i] + tol) return false;
        return true;
    }
    bool contains(const Vec3& p, double tol) const {
        for (int i = 0; i < 3; ++i)
            if (p[i] < mn[i] - tol || p[i] > mx[i] + tol) return false;
        return true;
    }
};

Box faceBox(const Mesh& m, Id f) {
    Box b;
    for (Id v : m.faces[f].outer) b.grow(m.pos(v));
    return b;
}

std::vector<Id> aliveFaces(const Mesh& m) {
    std::vector<Id> r;
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) r.push_back(f);
    return r;
}

void faceTriangles(const Mesh& m, Id f, std::vector<Tri>& out) {
    const Face& F = m.faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    std::vector<std::vector<Vec2>> loops;
    std::vector<Vec3> flat;
    auto add = [&](const std::vector<Id>& L) {
        loops.emplace_back();
        for (Id id : L) {
            loops.back().emplace_back(m.pos(id).dot(u), m.pos(id).dot(v));
            flat.push_back(m.pos(id));
        }
    };
    add(F.outer);
    for (const auto& h : F.holes) add(h);
    // Triangles are CCW in (u, v), and u x v is the face normal.
    for (const auto& t : triangulatePolygon(loops)) out.push_back({flat[t[0]], flat[t[1]], flat[t[2]]});
}

double signedVolume(const std::vector<Tri>& tris) {
    double v = 0;
    for (const Tri& t : tris) v += t[0].dot(t[1].cross(t[2]));
    return v / 6.0;
}

// Generalised winding number of p with respect to a closed triangle surface:
// 1 inside an outward-oriented solid, 0 outside.
double winding(const std::vector<Tri>& tris, const Vec3& p) {
    double sum = 0;
    for (const Tri& t : tris) {
        Vec3 a = t[0] - p, b = t[1] - p, c = t[2] - p;
        double la = a.length(), lb = b.length(), lc = c.length();
        double num = a.dot(b.cross(c));
        double den = la * lb * lc + a.dot(b) * lc + a.dot(c) * lb + b.dot(c) * la;
        sum += 2 * std::atan2(num, den);
    }
    return sum / (4 * kPi);
}

// Faces of a connected shell, flipped so that neighbours agree. Returns false
// if some edge is not shared by exactly two faces or the shell can't be
// oriented (Moebius-like).
bool orientShells(Mesh& m, std::vector<std::vector<Id>>& shells, std::string* why) {
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        if (!m.edges[e].alive) continue;
        int n = 0;
        for (Id f : m.edgeFaces(e)) n += m.faces[f].alive;
        if (n != 0 && n != 2) {
            if (why)
                *why = n == 1 ? "it has holes: some edges border only one face"
                              : "some edges are shared by more than two faces";
            return false;
        }
    }
    std::vector<int> shellOf(m.faces.size(), -1);
    for (Id seed : aliveFaces(m)) {
        if (shellOf[seed] >= 0) continue;
        const int s = (int)shells.size();
        shells.emplace_back();
        shellOf[seed] = s;
        std::vector<Id> queue = {seed};
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            const Id f = queue[qi];
            shells[s].push_back(f);
            std::vector<std::vector<Id>> loops = {m.faces[f].outer};
            for (const auto& h : m.faces[f].holes) loops.push_back(h);
            for (const auto& L : loops)
                for (size_t i = 0; i < L.size(); ++i) {
                    Id a = L[i], b = L[(i + 1) % L.size()];
                    Id e = m.findEdge(a, b);
                    if (e == kNone) continue;
                    for (Id g : m.edgeFaces(e)) {
                        if (g == f || !m.faces[g].alive) continue;
                        bool same = m.faceEdgeDirection(g, a, b) == 1;
                        if (shellOf[g] < 0) {
                            if (same) m.reverseFace(g);
                            shellOf[g] = s;
                            queue.push_back(g);
                        } else if (same) {
                            if (why) *why = "its faces can't be oriented consistently";
                            return false;
                        }
                    }
                }
        }
    }
    if (shells.empty()) {
        if (why) *why = "it has no faces";
        return false;
    }
    return true;
}

// Orients every shell outward, except shells nested inside an odd number of
// other shells (voids), which face inward. Returns all triangles.
bool orientSolid(Mesh& m, std::vector<Tri>* trisOut, std::string* why) {
    std::vector<std::vector<Id>> shells;
    if (!orientShells(m, shells, why)) return false;
    std::vector<std::vector<Tri>> tris(shells.size());
    for (size_t s = 0; s < shells.size(); ++s) {
        for (Id f : shells[s]) faceTriangles(m, f, tris[s]);
        if (signedVolume(tris[s]) < 0) {
            for (Id f : shells[s]) m.reverseFace(f);
            for (Tri& t : tris[s]) std::swap(t[1], t[2]);
        }
    }
    if (shells.size() > 1)
        for (size_t s = 0; s < shells.size(); ++s) {
            if (tris[s].empty()) continue;
            const Tri& t = tris[s][0];
            Vec3 p = (t[0] + t[1] + t[2]) / 3.0;
            int depth = 0;
            for (size_t o = 0; o < shells.size(); ++o)
                if (o != s && winding(tris[o], p) > 0.5) ++depth;
            if (depth % 2) {
                for (Id f : shells[s]) m.reverseFace(f);
                for (Tri& tr : tris[s]) std::swap(tr[1], tr[2]);
            }
        }
    if (trisOut)
        for (auto& t : tris) trisOut->insert(trisOut->end(), t.begin(), t.end());
    return true;
}

// Cut points are matched this closely first. Corners near the other surface
// are snapped onto it before cutting (snapNearSurface) and cut faces are flat
// (flattenFace), so the faces of an edge put its cut point in the same place
// to rounding; matching within kTol instead would join two distinct points
// of a cut running almost along a face, and the face would not split.
constexpr double kExact = 1e-9;

// Parameter intervals of the line o + t*d (d unit, in the face's plane) that
// lie in the closed face.
std::vector<std::pair<double, double>> lineInFace(const Mesh& m, Id f, const Vec3& o, const Vec3& d) {
    const Face& F = m.faces[f];
    const Vec3 w = F.normal.cross(d).normalized(); // in-plane, across the line
    std::vector<double> ts;
    auto loop = [&](const std::vector<Id>& L) {
        for (size_t i = 0; i < L.size(); ++i) {
            const Vec3& p = m.pos(L[i]);
            const Vec3& q = m.pos(L[(i + 1) % L.size()]);
            double sp = (p - o).dot(w), sq = (q - o).dot(w);
            if (std::fabs(sp) <= kTol) ts.push_back((p - o).dot(d));
            if ((sp > kTol && sq < -kTol) || (sp < -kTol && sq > kTol))
                ts.push_back((lerp(p, q, sp / (sp - sq)) - o).dot(d));
        }
    };
    loop(F.outer);
    for (const auto& h : F.holes) loop(h);
    std::sort(ts.begin(), ts.end());
    std::vector<std::pair<double, double>> r;
    for (size_t i = 0; i + 1 < ts.size(); ++i) {
        double t0 = ts[i], t1 = ts[i + 1];
        if (t1 - t0 <= kExact) continue;
        if (!m.pointInFace(f, o + d * ((t0 + t1) * 0.5), true)) continue;
        if (!r.empty() && r.back().second >= t0 - kTol) r.back().second = t1;
        else r.push_back({t0, t1});
    }
    return r;
}

// Vertex at p for an edit of face f: an existing vertex, one inserted into an
// edge of f (keeping the neighbouring face in sync), or a new one.
Id faceVertex(Mesh& m, Id f, const Vec3& p) {
    // Exactly: a corner of f, or a point on one of its edges.
    for (double tol : {kExact, kTol}) {
        for (Id v : m.faceVertices(f))
            if ((m.pos(v) - p).length() <= tol) return v;
        if (tol == kTol) {
            const Id v = m.findVertex(p);
            if (v != kNone) return v;
        }
        for (Id e : m.faceEdges(f)) {
            const Vec3& a = m.pos(m.edges[e].v[0]);
            const Vec3& b = m.pos(m.edges[e].v[1]);
            const double t = closestParamOnSegment(a, b, p);
            const Vec3 q = lerp(a, b, t);
            if ((q - p).length() > tol) continue;
            if ((q - a).length() <= tol) return m.edges[e].v[0];
            if ((q - b).length() <= tol) return m.edges[e].v[1];
            const Id nv = m.addVertexRaw(q);
            std::vector<Id> fs = m.edgeFaces(e);
            m.splitEdge(e, nv, &fs);
            return nv;
        }
    }
    return m.addVertexRaw(p);
}

double loopArea(const Mesh& m, const std::vector<Id>& L) {
    return newellNormal((int)L.size(), [&](int i) { return m.pos(L[i]); }).length() * 0.5;
}

Vec3 regionPoint(const Mesh& m, const Vec3& n, const Mesh::Region& r) {
    Vec3 u, v;
    planeBasis(n, u, v);
    std::vector<std::vector<Vec2>> loops;
    std::vector<Vec3> flat;
    auto add = [&](const std::vector<Id>& L) {
        loops.emplace_back();
        for (Id id : L) {
            loops.back().emplace_back(m.pos(id).dot(u), m.pos(id).dot(v));
            flat.push_back(m.pos(id));
        }
    };
    add(r.outer);
    for (const auto& h : r.holes) add(h);
    double best = -1;
    Vec3 res = flat.empty() ? Vec3() : flat[0];
    std::vector<Vec2> all;
    for (const auto& l : loops) all.insert(all.end(), l.begin(), l.end());
    for (const auto& t : triangulatePolygon(loops)) {
        double a = std::fabs((all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]));
        if (a > best) {
            best = a;
            res = (flat[t[0]] + flat[t[1]] + flat[t[2]]) / 3.0;
        }
    }
    return res;
}

// Splits face f along segments lying in its plane. New faces inherit the
// face's sides. False if the pieces don't add up to the face.
bool splitFace(Mesh& m, Id f, const std::vector<Seg>& segs) {
    const Vec3 normal = m.faces[f].normal;
    std::vector<std::pair<Id, Id>> pieces;
    for (const Seg& s : segs) {
        Id a = faceVertex(m, f, s.a), b = faceVertex(m, f, s.b);
        if (a != b) pieces.push_back({a, b});
    }
    if (pieces.empty()) return true;

    // Split the pieces where they cross or touch each other and at the face's
    // own vertices, then turn them into edges.
    std::vector<std::vector<Id>> on(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
        for (size_t j = i + 1; j < pieces.size(); ++j) {
            const Vec3 a = m.pos(pieces[i].first), b = m.pos(pieces[i].second);
            const Vec3 c = m.pos(pieces[j].first), d = m.pos(pieces[j].second);
            double s, t;
            if (!closestLineLine(a, b - a, c, d - c, s, t)) continue;
            double ei = kTol / (b - a).length(), ej = kTol / (d - c).length();
            if (s <= ei || s >= 1 - ei || t <= ej || t >= 1 - ej) continue;
            Vec3 p = lerp(a, b, s), q = lerp(c, d, t);
            if ((p - q).length() > kTol) continue;
            Id x = faceVertex(m, f, (p + q) * 0.5);
            on[i].push_back(x);
            on[j].push_back(x);
        }
    std::set<Id> cand;
    for (Id v : m.faceVertices(f)) cand.insert(v);
    for (const auto& pc : pieces) {
        cand.insert(pc.first);
        cand.insert(pc.second);
    }
    std::set<Id> cut;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Id va = pieces[i].first, vb = pieces[i].second;
        const Vec3 a = m.pos(va), b = m.pos(vb);
        const double len = (b - a).length(), eps = kTol / len;
        std::vector<std::pair<double, Id>> pts = {{0.0, va}, {1.0, vb}};
        for (Id x : on[i]) pts.push_back({closestParamOnSegment(a, b, m.pos(x)), x});
        for (Id x : cand) {
            if (x == va || x == vb) continue;
            double t = closestParamOnSegment(a, b, m.pos(x));
            if (t > eps && t < 1 - eps && (lerp(a, b, t) - m.pos(x)).length() <= kTol) pts.push_back({t, x});
        }
        std::sort(pts.begin(), pts.end());
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            if (pts[k].second == pts[k + 1].second) continue;
            Id e = m.addEdgeRaw(pts[k].second, pts[k + 1].second);
            if (e != kNone) cut.insert(e);
        }
    }

    const Plane P = m.facePlane(f);
    std::vector<Id> pe = m.faceEdges(f);
    pe.insert(pe.end(), cut.begin(), cut.end());
    std::sort(pe.begin(), pe.end());
    pe.erase(std::unique(pe.begin(), pe.end()), pe.end());
    std::vector<Mesh::Region> regions = m.computeRegions(P, pe);
    std::vector<const Mesh::Region*> keep;
    double area = 0;
    for (const auto& r : regions) {
        // The cuts are clipped to the face, so every region but the face's
        // holes is a piece of it; slivers thinner than the tolerance (a cut
        // passing just by a corner) count too, or they would leave a hole.
        if (!m.pointInFace(f, regionPoint(m, P.n, r), true)) continue;
        keep.push_back(&r);
        area += loopArea(m, r.outer);
        for (const auto& h : r.holes) area -= loopArea(m, h);
    }
    if (keep.size() <= 1) return true; // only dangling cuts: the face stays whole
    const double faceArea = m.faceArea(f);
    if (std::fabs(area - faceArea) > 1e-6 * faceArea + kTol * kTol) return false;

    const FaceSide front = m.faces[f].front, back = m.faces[f].back;
    const bool hidden = m.faces[f].hidden;
    for (const Mesh::Region* r : keep) {
        Id nf = m.addFaceRaw(r->outer, r->holes, normal);
        if (nf == kNone) return false;
        m.faces[nf].front = front;
        m.faces[nf].back = back;
        m.faces[nf].hidden = hidden;
    }
    m.eraseFace(f);
    return true;
}

enum Where { Outside, Inside, SharedSame, SharedOpposite };

// Original edges lying between two coplanar faces that face the same way:
// lines the user drew on a flat surface, which must survive the clean-up.
void flatEdges(const Mesh& m, std::vector<Seg>& out) {
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        if (!m.edges[e].alive) continue;
        std::vector<Id> fs;
        for (Id f : m.edgeFaces(e))
            if (m.faces[f].alive) fs.push_back(f);
        if (fs.size() != 2 || m.faces[fs[0]].normal.dot(m.faces[fs[1]].normal) < 1 - 1e-9) continue;
        out.push_back({m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1])});
    }
}

// Inserts vertices lying inside edges into those edges (T-junctions left by
// joining the pieces of two solids).
void splitTJunctions(Mesh& m) {
    std::vector<Id> vs;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v)
        if (m.vertices[v].alive) vs.push_back(v);
    std::sort(vs.begin(), vs.end(), [&](Id a, Id b) { return m.pos(a).x < m.pos(b).x; });
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        if (!m.edges[e].alive) continue;
        Box b;
        b.grow(m.pos(m.edges[e].v[0]));
        b.grow(m.pos(m.edges[e].v[1]));
        auto lo = std::lower_bound(vs.begin(), vs.end(), b.mn.x - kTol,
                                   [&](Id v, double x) { return m.pos(v).x < x; });
        std::vector<std::pair<double, Id>> hits;
        const Vec3 a = m.pos(m.edges[e].v[0]), c = m.pos(m.edges[e].v[1]);
        const double eps = kTol / (c - a).length();
        for (auto it = lo; it != vs.end() && m.pos(*it).x <= b.mx.x + kTol; ++it) {
            const Id v = *it;
            if (m.edges[e].has(v) || !b.contains(m.pos(v), kTol)) continue;
            double t = closestParamOnSegment(a, c, m.pos(v));
            if (t > eps && t < 1 - eps && (lerp(a, c, t) - m.pos(v)).length() <= kTol) hits.push_back({t, v});
        }
        if (hits.empty()) continue;
        std::sort(hits.begin(), hits.end());
        // Split from the far end so `e` keeps running from v[0] to the next hit.
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            std::vector<Id> fs = m.edgeFaces(e);
            m.splitEdge(e, it->second, &fs);
        }
    }
}

// Faces bent by more than this count as warped: any less is rounding.
constexpr double kFlat = 1e-9;

// How far face f's corners stray from its plane (0 for a flat face).
double faceWarp(const Mesh& m, Id f) {
    const Plane P = m.facePlane(f);
    double w = 0;
    for (Id v : m.faceVertices(f)) w = std::max(w, std::fabs(P.distance(m.pos(v))));
    return w;
}

// Replaces face f by flat triangles over the same corners. The new inner
// edges are soft and smooth, so the face still looks like one surface. Cuts
// are worked out per face in its plane: on a warped face (imported and
// sculpted quads usually are, slightly) the two faces of an edge would put
// the same cut point in different places and the pieces would not join.
// Even a bend far below the tolerance does that where an edge runs almost
// along the cut (the points drift apart by the bend over the angle).
void flattenFace(Mesh& m, Id f) {
    const Face F = m.faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    std::vector<std::vector<Vec2>> loops;
    std::vector<Id> ids;
    auto add = [&](const std::vector<Id>& L) {
        loops.emplace_back();
        for (Id id : L) {
            loops.back().emplace_back(m.pos(id).dot(u), m.pos(id).dot(v));
            ids.push_back(id);
        }
    };
    add(F.outer);
    for (const auto& h : F.holes) add(h);
    std::vector<std::array<int, 3>> tris;
    if (F.holes.empty() && F.outer.size() == 4) {
        // A quad: the diagonal that gives the less skinny pair of triangles.
        auto quality = [&](int a, int b, int c) {
            const Vec3 p = m.pos(ids[a]), q = m.pos(ids[b]), r = m.pos(ids[c]);
            const double l = (q - p).length2() + (r - q).length2() + (p - r).length2();
            return l > 0 ? (q - p).cross(r - p).dot(F.normal) / l : -1.0;
        };
        const double d02 = std::min(quality(0, 1, 2), quality(0, 2, 3));
        const double d13 = std::min(quality(1, 2, 3), quality(1, 3, 0));
        if (d02 >= d13) tris = {{0, 1, 2}, {0, 2, 3}};
        else tris = {{1, 2, 3}, {1, 3, 0}};
    } else {
        tris = triangulatePolygon(loops);
    }
    if (tris.empty()) return;
    std::set<Id> outline;
    for (Id e : m.faceEdges(f)) outline.insert(e);
    m.eraseFace(f);
    for (const auto& t : tris) {
        const Id nf = m.addFaceRaw({ids[t[0]], ids[t[1]], ids[t[2]]}, {}, F.normal);
        if (nf == kNone) continue;
        m.faces[nf].front = F.front;
        m.faces[nf].back = F.back;
        m.faces[nf].hidden = F.hidden;
        for (int k = 0; k < 3; ++k) {
            const Id e = m.findEdge(ids[t[k]], ids[t[(k + 1) % 3]]);
            if (e != kNone && !outline.count(e)) m.edges[e].soft = m.edges[e].smooth = true;
        }
    }
}

bool sameSides(const Face& a, const Face& b) {
    return a.front.material == b.front.material && a.back.material == b.back.material && a.hidden == b.hidden;
}

class Boolean {
public:
    Boolean(const Mesh& a, const Mesh& b) {
        work_[0] = a;
        work_[1] = b;
    }

    bool run(std::string* error) {
        const char* names[2] = {"The first solid", "The second solid"};
        for (int s = 0; s < 2; ++s) {
            std::string why;
            if (!orientSolid(work_[s], nullptr, &why)) {
                if (error) *error = std::string(names[s]) + " is not a solid: " + why + ".";
                return false;
            }
        }
        snapNearSurface(0);
        snapNearSurface(1);
        flattenCutFaces();
        for (int s = 0; s < 2; ++s) {
            std::string why;
            Mesh& m = work_[s];
            if (!orientSolid(m, &tris_[s], &why)) {
                if (error) *error = std::string(names[s]) + " is not a solid: " + why + ".";
                return false;
            }
            flatEdges(m, flat_);
            for (Id v = 0; v < (Id)m.vertices.size(); ++v)
                if (m.vertices[v].alive) {
                    originalVerts_.addVertex(m.pos(v));
                    bounds_[s].grow(m.pos(v));
                }
        }
        intersect();
        for (int s = 0; s < 2; ++s)
            for (auto& [f, segs] : segs_[s])
                if (!splitFace(work_[s], f, segs)) {
                    if (error) *error = "The solids touch in a way that is too small or too complex to cut cleanly.";
                    return false;
                }
        classify();
        return true;
    }

    // Keeps the pieces of each solid where `keep(solid, where)` returns
    // +1 (as is) or -1 (flipped: a cut surface seen from the other side).
    template <typename Keep>
    Mesh assemble(Keep keep) const {
        Mesh out;
        std::map<std::pair<int, Id>, Id> curves;
        for (int s = 0; s < 2; ++s) {
            const Mesh& m = work_[s];
            for (Id f : aliveFaces(m)) {
                int k = keep(s, where_[s][f]);
                if (!k) continue;
                const Face& F = m.faces[f];
                auto mapLoop = [&](const std::vector<Id>& L) {
                    std::vector<Id> r;
                    for (Id v : L) {
                        Id nv = out.addVertex(m.pos(v));
                        if (out.vertices[nv].paint.empty()) out.vertices[nv].paint = m.vertices[v].paint;
                        r.push_back(nv);
                    }
                    return r;
                };
                std::vector<Id> outer = mapLoop(F.outer);
                std::vector<std::vector<Id>> holes;
                for (const auto& h : F.holes) holes.push_back(mapLoop(h));
                Id nf = out.addFaceRaw(outer, holes, k > 0 ? F.normal : -F.normal);
                if (nf == kNone) continue;
                out.faces[nf].front = F.front;
                out.faces[nf].back = F.back;
                out.faces[nf].hidden = F.hidden;
                // Carry soft/smooth/hidden edges and curves over.
                std::vector<std::vector<Id>> src = {F.outer}, dst = {outer};
                for (size_t h = 0; h < F.holes.size(); ++h) {
                    src.push_back(F.holes[h]);
                    dst.push_back(holes[h]);
                }
                for (size_t l = 0; l < src.size(); ++l)
                    for (size_t i = 0; i < src[l].size(); ++i) {
                        size_t j = (i + 1) % src[l].size();
                        Id se = m.findEdge(src[l][i], src[l][j]);
                        Id de = out.findEdge(dst[l][i], dst[l][j]);
                        if (se == kNone || de == kNone) continue;
                        const Edge& S = m.edges[se];
                        Edge& D = out.edges[de];
                        D.soft = D.soft || S.soft;
                        D.smooth = D.smooth || S.smooth;
                        D.hidden = D.hidden || S.hidden;
                        if (S.curve != kNone && D.curve == kNone) {
                            auto it = curves.find({s, S.curve});
                            if (it == curves.end()) it = curves.emplace(std::make_pair(s, S.curve), out.newCurveId()).first;
                            D.curve = it->second;
                        }
                    }
            }
        }
        cleanUp(out);
        return out;
    }

private:
    // Corners of solid s lying a hair off the other solid's surface (within
    // twice the tolerance) move onto it. Otherwise the cut runs right past
    // them and leaves pieces thinner than the tolerance, which some steps see
    // and others don't (spurs, slivers lost, holes). The cut then goes through
    // the corner itself. Faces bent by this get flattened next.
    void snapNearSurface(int s) {
        Mesh& m = work_[s];
        const Mesh& o = work_[1 - s];
        const double band = 2 * kTol;
        std::vector<Id> of = aliveFaces(o);
        std::vector<Box> ob;
        std::vector<Plane> op;
        Box all;
        for (Id g : of) {
            ob.push_back(faceBox(o, g));
            op.push_back(o.facePlane(g));
            all.mn = Vec3(std::min(all.mn.x, ob.back().mn.x), std::min(all.mn.y, ob.back().mn.y), std::min(all.mn.z, ob.back().mn.z));
            all.mx = Vec3(std::max(all.mx.x, ob.back().mx.x), std::max(all.mx.y, ob.back().mx.y), std::max(all.mx.z, ob.back().mx.z));
        }
        std::set<Id> moved;
        for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
            if (!m.vertices[v].alive || !all.contains(m.pos(v), band)) continue;
            for (size_t k = 0; k < of.size(); ++k) {
                const Vec3 p = m.pos(v);
                if (!ob[k].contains(p, band)) continue;
                const double d = op[k].distance(p);
                if (d == 0 || std::fabs(d) > band) continue;
                const Vec3 q = p - op[k].n * d;
                if (!o.pointInFace(of[k], q, true)) continue;
                m.vertices[v].p = q;
                moved.insert(v);
            }
        }
        if (moved.empty()) return;
        std::set<Id> faces;
        for (Id v : moved)
            for (Id e : m.vertexEdges(v))
                for (Id f : m.edgeFaces(e))
                    if (m.faces[f].alive) faces.insert(f);
        for (Id f : faces) m.recomputeNormal(f);
        m.markGeometryMoved();
    }

    // Warped faces the other solid's surface passes through become flat
    // triangles (see flattenFace); the rest stay as they are.
    void flattenCutFaces() {
        std::vector<Id> fs[2];
        std::vector<Box> boxes[2];
        std::vector<Plane> planes[2];
        Box all[2];
        for (int s = 0; s < 2; ++s) {
            fs[s] = aliveFaces(work_[s]);
            for (Id f : fs[s]) {
                boxes[s].push_back(faceBox(work_[s], f));
                planes[s].push_back(work_[s].facePlane(f));
                for (Id v : work_[s].faces[f].outer) all[s].grow(work_[s].pos(v));
            }
        }
        for (int s = 0; s < 2; ++s) {
            const Mesh& m = work_[s];
            std::vector<Id> warped;
            for (size_t i = 0; i < fs[s].size(); ++i) {
                const Id f = fs[s][i];
                if (!boxes[s][i].overlaps(all[1 - s], kTol) || faceWarp(m, f) <= kFlat) continue;
                bool crossed = false;
                for (size_t j = 0; j < fs[1 - s].size() && !crossed; ++j) {
                    if (!boxes[s][i].overlaps(boxes[1 - s][j], kTol)) continue;
                    bool pos = false, neg = false;
                    for (Id v : m.faces[f].outer) {
                        const double d = planes[1 - s][j].distance(m.pos(v));
                        pos = pos || d > -kTol;
                        neg = neg || d < kTol;
                    }
                    crossed = pos && neg;
                }
                if (crossed) warped.push_back(f);
            }
            for (Id f : warped) flattenFace(work_[s], f);
            if (!warped.empty()) work_[s].touch();
        }
    }

    void intersect() {
        Mesh& A = work_[0];
        Mesh& B = work_[1];
        std::vector<Id> fa = aliveFaces(A), fb = aliveFaces(B);
        std::vector<Box> ba, bb;
        for (Id f : fa) ba.push_back(faceBox(A, f));
        for (Id f : fb) bb.push_back(faceBox(B, f));
        std::vector<Plane> pa, pb;
        for (Id f : fa) pa.push_back(A.facePlane(f));
        for (Id f : fb) pb.push_back(B.facePlane(f));
        auto straddles = [](const Mesh& m, Id f, const Plane& P) {
            bool pos = false, neg = false;
            for (Id v : m.faces[f].outer) {
                double d = P.distance(m.pos(v));
                pos = pos || d > -kTol;
                neg = neg || d < kTol;
            }
            return pos && neg;
        };
        auto clip = [](const Mesh& m, Id f, const Vec3& p, const Vec3& q, std::vector<Seg>& out) {
            double len = (q - p).length();
            if (len <= kTol) return;
            Vec3 d = (q - p) / len;
            for (auto [t0, t1] : lineInFace(m, f, p, d)) {
                t0 = std::max(t0, 0.0);
                t1 = std::min(t1, len);
                if (t1 - t0 > kTol) out.push_back({p + d * t0, p + d * t1});
            }
        };
        auto clipLoops = [&](const Mesh& from, Id g, const Mesh& to, Id f, std::vector<Seg>& out) {
            std::vector<std::vector<Id>> loops = {from.faces[g].outer};
            for (const auto& h : from.faces[g].holes) loops.push_back(h);
            for (const auto& L : loops)
                for (size_t i = 0; i < L.size(); ++i) clip(to, f, from.pos(L[i]), from.pos(L[(i + 1) % L.size()]), out);
        };
        for (size_t i = 0; i < fa.size(); ++i) {
            if (!ba[i].overlaps(bounds_[1], kTol)) continue;
            for (size_t j = 0; j < fb.size(); ++j) {
                if (!ba[i].overlaps(bb[j], kTol)) continue;
                const Plane &P = pa[i], &Q = pb[j];
                Vec3 dir = P.n.cross(Q.n);
                if (dir.length() < 1e-9) {
                    if (!P.coincident(Q)) continue;
                    // Coplanar: each face gets the other's outline where it overlaps.
                    clipLoops(B, fb[j], A, fa[i], segs_[0][fa[i]]);
                    clipLoops(A, fa[i], B, fb[j], segs_[1][fb[j]]);
                    continue;
                }
                if (!straddles(A, fa[i], Q) || !straddles(B, fb[j], P)) continue;
                dir = dir.normalized();
                // Point on both planes (closest to the origin).
                double c = P.n.dot(Q.n), det = 1 - c * c;
                Vec3 o = P.n * ((P.d - Q.d * c) / det) + Q.n * ((Q.d - P.d * c) / det);
                auto ia = lineInFace(A, fa[i], o, dir);
                if (ia.empty()) continue;
                auto ib = lineInFace(B, fb[j], o, dir);
                for (auto [a0, a1] : ia)
                    for (auto [b0, b1] : ib) {
                        double lo = std::max(a0, b0), hi = std::min(a1, b1);
                        if (hi - lo <= kExact) continue;
                        Seg s{o + dir * lo, o + dir * hi};
                        segs_[0][fa[i]].push_back(s);
                        segs_[1][fb[j]].push_back(s);
                    }
            }
        }
    }

    void classify() {
        for (int s = 0; s < 2; ++s) {
            const Mesh& m = work_[s];
            const Mesh& other = work_[1 - s];
            std::vector<Id> of = aliveFaces(other);
            std::vector<Box> ob;
            for (Id g : of) ob.push_back(faceBox(other, g));
            where_[s].assign(m.faces.size(), Outside);
            for (Id f : aliveFaces(m)) {
                const Vec3 p = m.faceInteriorPoint(f);
                const Vec3 n = m.faces[f].normal;
                Where w = Outside;
                bool shared = false;
                for (size_t k = 0; k < of.size() && !shared; ++k) {
                    const Id g = of[k];
                    if (!ob[k].contains(p, kTol)) continue;
                    const Vec3 gn = other.faces[g].normal;
                    if (std::fabs(gn.dot(n)) < 1 - 1e-9) continue;
                    if (std::fabs(other.facePlane(g).distance(p)) > kTol) continue;
                    if (!other.pointInFace(g, p, true)) continue;
                    shared = true;
                    w = gn.dot(n) > 0 ? SharedSame : SharedOpposite;
                }
                if (!shared && bounds_[1 - s].contains(p, kTol) && winding(tris_[1 - s], p) > 0.5) w = Inside;
                where_[s][f] = w;
            }
        }
    }

    void cleanUp(Mesh& m) const {
        splitTJunctions(m);
        // A sliver (corners almost in a line, as left by triangulating) has
        // its middle corner put into its long edge: nothing is left of it.
        m.removeDegenerateFaces();
        auto onFlatEdge = [&](const Vec3& p) {
            for (const Seg& s : flat_)
                if (distancePointSegment(s.a, s.b, p) <= kTol) return true;
            return false;
        };
        // Merge coplanar faces split only by the operation.
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            for (Id e = 0; e < (Id)m.edges.size(); ++e) {
                if (!m.edges[e].alive) continue;
                std::vector<Id> fs;
                for (Id f : m.edgeFaces(e))
                    if (m.faces[f].alive) fs.push_back(f);
                if (fs.size() != 2) continue;
                const Face &F = m.faces[fs[0]], &G = m.faces[fs[1]];
                if (F.normal.dot(G.normal) < 1 - 1e-9 || !sameSides(F, G)) continue;
                if (!m.facePlane(fs[0]).coincident(m.facePlane(fs[1]), kTol * 10)) continue;
                if (onFlatEdge((m.pos(m.edges[e].v[0]) + m.pos(m.edges[e].v[1])) * 0.5)) continue;
                m.eraseEdge(e, true);
                changed = true;
            }
            if (!changed) break;
        }
        // Merging can leave edges that no longer bound a face.
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            bool used = false;
            for (Id f : m.edgeFaces(e)) used = used || m.faces[f].alive;
            if (!used) m.eraseEdge(e, false);
        }
        // Drop vertices the cuts left in the middle of straight edges.
        for (Id v = 0; v < (Id)m.vertices.size(); ++v)
            if (m.vertices[v].alive && originalVerts_.findVertex(m.pos(v)) == kNone) m.mergeCollinearAt(v);
        m.removeOrphanVertices();
        m.touch();
    }

    Mesh work_[2];
    std::vector<Tri> tris_[2];
    Box bounds_[2];
    std::map<Id, std::vector<Seg>> segs_[2];
    std::vector<Where> where_[2];
    std::vector<Seg> flat_;
    Mesh originalVerts_;
};

// Rules for keeping the pieces of solid s (0 = a, 1 = b).
int keepUnion(int, Where w) { return w == Outside ? 1 : 0; }
int keepIntersect(int, Where w) { return w == Inside ? 1 : 0; }
int keepAMinusB(int s, Where w) { return s == 0 ? (w == Outside || w == SharedOpposite) : (w == Inside ? -1 : 0); }
int keepBMinusA(int s, Where w) { return s == 1 ? (w == Outside || w == SharedOpposite) : (w == Inside ? -1 : 0); }

bool aliveInstance(const ModelState& st, Id i) {
    return i >= 0 && i < (Id)st.instances.size() && st.instances[i].alive && st.instances[i].def >= 0 &&
           st.instances[i].def < (Id)st.defs.size();
}

Mesh worldMesh(const ModelState& st, Id inst) {
    Mesh m;
    appendMesh(m, st.defs[st.instances[inst].def].mesh, instanceWorldXf(st, inst), nullptr, nullptr, true);
    return m;
}

// Puts `world` into instance `inst` (in its own axes). Definitions used by
// other instances are copied first; an empty result deletes the instance.
void placeResult(ModelState& st, Id inst, const Mesh& world, const std::string& name) {
    Instance& I = st.instances[inst];
    if (world.aliveFaceCount() == 0) {
        I.alive = false;
        return;
    }
    if (instanceCount(st, I.def) > 1 || !name.empty()) {
        ComponentDef d;
        d.name = uniqueComponentName(st, name.empty() ? st.defs[I.def].name : name);
        d.uid = newComponentUid();
        st.defs.push_back(std::move(d));
        I.def = (Id)st.defs.size() - 1;
    }
    Mesh local;
    appendMesh(local, world, I.xf.inverted());
    local.compact();
    st.defs[I.def].mesh = std::move(local);
}

} // namespace

const char* solidOpName(SolidOp op) {
    switch (op) {
    case SolidOp::Union: return "Union";
    case SolidOp::Intersect: return "Intersect";
    case SolidOp::Subtract: return "Subtract";
    case SolidOp::Trim: return "Trim";
    case SolidOp::Split: return "Split";
    }
    return "";
}

bool isSolid(const Mesh& m, std::string* why) {
    Mesh c = m;
    return orientSolid(c, nullptr, why);
}

bool isSolidCached(const Mesh& m) {
    static std::mutex lock;
    static std::unordered_map<uint64_t, bool> known; // by mesh revision (copies share it, with the same content)
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = known.find(m.revision);
        if (it != known.end()) return it->second;
    }
    const bool solid = m.aliveFaceCount() > 0 && isSolid(m);
    std::lock_guard<std::mutex> g(lock);
    if (known.size() > 4096) known.clear();
    known[m.revision] = solid;
    return solid;
}

double solidVolume(const Mesh& m) {
    Mesh c = m;
    std::vector<Tri> tris;
    if (!orientSolid(c, &tris, nullptr)) return 0;
    return signedVolume(tris);
}

bool booleanMeshes(const Mesh& a, const Mesh& b, BoolOp op, Mesh& out, std::string* error) {
    out = Mesh();
    Boolean bo(a, b);
    if (!bo.run(error)) return false;
    switch (op) {
    case BoolOp::Union:
        out = bo.assemble([](int s, Where w) { return s == 0 && w == SharedSame ? 1 : keepUnion(s, w); });
        break;
    case BoolOp::Intersect:
        out = bo.assemble([](int s, Where w) { return s == 0 && w == SharedSame ? 1 : keepIntersect(s, w); });
        break;
    case BoolOp::Difference: out = bo.assemble(keepAMinusB); break;
    }
    return true;
}

bool splitMeshes(const Mesh& a, const Mesh& b, Mesh& aOnly, Mesh& both, Mesh& bOnly, std::string* error) {
    aOnly = both = bOnly = Mesh();
    Boolean bo(a, b);
    if (!bo.run(error)) return false;
    aOnly = bo.assemble(keepAMinusB);
    both = bo.assemble([](int s, Where w) { return s == 0 && w == SharedSame ? 1 : keepIntersect(s, w); });
    bOnly = bo.assemble(keepBMinusA);
    return true;
}

bool instanceIsSolid(const ModelState& st, Id inst, std::string* why) {
    if (!aliveInstance(st, inst)) return false;
    return isSolid(st.defs[st.instances[inst].def].mesh, why);
}

bool solidOperation(ModelState& st, SolidOp op, Id first, Id second, Selection* result, std::string* error) {
    if (!aliveInstance(st, first) || !aliveInstance(st, second) || first == second) {
        if (error) *error = "Pick two different components.";
        return false;
    }
    ModelState work = st;
    closeComponent(work);
    const Mesh a = worldMesh(work, first), b = worldMesh(work, second);
    if (op == SolidOp::Split) {
        Mesh aOnly, both, bOnly;
        if (!splitMeshes(a, b, aOnly, both, bOnly, error)) return false;
        if (both.aliveFaceCount() == 0) {
            if (error) *error = "The solids don't overlap, so there is nothing to split.";
            return false;
        }
        // The shared part starts as a copy of the second instance and gets its
        // own definition before the second one is changed.
        Id shared = cloneInstance(work, second);
        placeResult(work, shared, both, work.defs[work.instances[second].def].name + " Intersection");
        placeResult(work, first, aOnly, "");
        placeResult(work, second, bOnly, "");
        if (result) {
            result->clear();
            for (Id i : {first, shared, second})
                if (work.instances[i].alive) result->instances.insert(i);
        }
    } else {
        Mesh r;
        BoolOp bop = op == SolidOp::Union ? BoolOp::Union : op == SolidOp::Intersect ? BoolOp::Intersect : BoolOp::Difference;
        // Subtract and Trim cut the first solid out of the second.
        if (!booleanMeshes(bop == BoolOp::Difference ? b : a, bop == BoolOp::Difference ? a : b, bop, r, error)) return false;
        if (op == SolidOp::Intersect && r.aliveFaceCount() == 0) {
            if (error) *error = "The solids don't overlap, so they have nothing in common.";
            return false;
        }
        placeResult(work, second, r, "");
        if (op != SolidOp::Trim) work.instances[first].alive = false;
        if (result) {
            result->clear();
            if (work.instances[second].alive) result->instances.insert(second);
        }
    }
    st = std::move(work);
    return true;
}

// ---------------------------------------------------------------------------
// Knife
// ---------------------------------------------------------------------------

bool knifeIsFlat(const Mesh& knife, Plane* plane, std::string* why) {
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    Id biggest = kNone;
    double area = 0;
    for (Id f = 0; f < (Id)knife.faces.size(); ++f)
        if (knife.faces[f].alive) {
            const double a = knife.faceArea(f);
            if (biggest == kNone || a > area) biggest = f, area = a;
        }
    if (biggest == kNone || area <= 0) return fail("the knife has no face");
    const Plane P(knife.faces[biggest].normal, knife.pos(knife.faces[biggest].outer[0]));
    Vec3 mn, mx;
    bool any = false;
    knife.bounds(mn, mx, any);
    const double tol = std::max(kTol, (mx - mn).length() * 1e-7);
    for (Id f = 0; f < (Id)knife.faces.size(); ++f)
        if (knife.faces[f].alive)
            for (Id v : knife.faceVertices(f))
                if (std::fabs(P.distance(knife.pos(v))) > tol) return fail("the knife must be flat: all its faces in one plane");
    if (plane) *plane = P;
    return true;
}

bool knifeMesh(const Mesh& targetIn, const Mesh& knife, std::vector<Mesh>& pieces, std::string* error) {
    pieces.clear();
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        pieces.clear();
        return false;
    };
    Plane P;
    std::string why;
    if (!knifeIsFlat(knife, &P, &why)) return fail("The knife can't be used: " + why + ".");
    if (targetIn.aliveFaceCount() == 0) return fail("There is nothing to cut: the component has no faces.");
    std::vector<Id> knifeFaces;
    for (Id f = 0; f < (Id)knife.faces.size(); ++f)
        if (knife.faces[f].alive) knifeFaces.push_back(f);
    auto inKnife = [&](const Vec3& q) {
        const Vec3 on = P.project(q);
        for (Id f : knifeFaces)
            if (knife.pointInFace(f, on, true)) return true;
        return false;
    };
    const bool solid = isSolid(targetIn);
    Mesh m = targetIn;
    const Mesh before = targetIn;
    auto snap = [&](double d) { return std::fabs(d) <= kTol ? 0.0 : d; };
    auto inPlane = [&](const Mesh& mm, Id f) {
        for (Id v : mm.faceVertices(f))
            if (snap(P.distance(mm.pos(v))) != 0) return false;
        return true;
    };
    std::vector<Id> coplanarBefore;
    for (Id f = 0; f < (Id)before.faces.size(); ++f)
        if (before.faces[f].alive && inPlane(before, f)) coplanarBefore.push_back(f);

    Vec3 kmn, kmx;
    bool kany = false;
    knife.bounds(kmn, kmx, kany);

    const char* notThrough = "The knife doesn't cut all the way through: make it larger than the cross-section, so the part comes off.";
    auto sideOf = [&](const Vec3& q) { return snap(P.distance(q)) >= 0; }; // on the plane counts as above

    // 1. A point wherever an edge crosses the knife's plane (within the
    //    knife's reach), shared by the faces on both sides of the edge.
    //    Faces need not be exactly flat: imported and sculpted quads are often
    //    slightly warped, and they are cut all the same.
    auto nearKnife = [&](const Vec3& q) {
        for (int i = 0; i < 3; ++i)
            if (q[i] < kmn[i] - kTol || q[i] > kmx[i] + kTol) return false;
        return true;
    };
    {
        const Id ne = (Id)m.edges.size();
        for (Id e = 0; e < ne; ++e) {
            if (!m.edges[e].alive) continue;
            const Id a = m.edges[e].v[0], b = m.edges[e].v[1];
            const double da = snap(P.distance(m.pos(a))), db = snap(P.distance(m.pos(b)));
            if (da == 0 || db == 0 || (da > 0) == (db > 0)) continue;
            const Vec3 X = m.pos(a) + (m.pos(b) - m.pos(a)) * (da / (da - db));
            if (!nearKnife(X) || !inKnife(X)) continue;
            const std::vector<Id> fs = m.edgeFaces(e);
            m.splitEdge(e, m.addVertexRaw(X), &fs);
        }
    }

    // 2. Each face crossing the plane is split along chords between those
    //    points (paired in order along the crossing line), where the whole
    //    chord lies within the knife.
    std::vector<std::pair<Id, Id>> chords;
    const Id nf0 = (Id)m.faces.size();
    for (Id f = 0; f < nf0; ++f) {
        if (!m.faces[f].alive) continue;
        const Face F = m.faces[f];
        const Vec3 dir = P.n.cross(F.normal);
        if (dir.length() < 1e-9) continue;
        const Vec3 u = dir.normalized();
        std::vector<const std::vector<Id>*> loops{&F.outer};
        for (const auto& h : F.holes) loops.push_back(&h);
        std::vector<std::pair<double, Id>> cross;
        bool above = false, below = false;
        for (const auto* L : loops)
            for (size_t k = 0; k < L->size(); ++k) {
                const Id a = (*L)[k], b = (*L)[(k + 1) % L->size()];
                (sideOf(m.pos(a)) ? above : below) = true;
                if (sideOf(m.pos(a)) != sideOf(m.pos(b))) {
                    // After step 1 the crossing is a point on the plane, unless
                    // it lies outside the knife (then this pair is not cut).
                    const Id x = sideOf(m.pos(a)) ? a : b;
                    if (snap(P.distance(m.pos(x))) == 0) {
                        cross.push_back({m.pos(x).dot(u), x});
                    } else {
                        const double da = P.distance(m.pos(a)), db = P.distance(m.pos(b));
                        const Vec3 X = m.pos(a) + (m.pos(b) - m.pos(a)) * (da / (da - db));
                        cross.push_back({X.dot(u), kNone});
                    }
                }
            }
        if (!above || !below || cross.size() < 2) continue;
        std::sort(cross.begin(), cross.end());
        std::vector<std::pair<Id, Id>> mine;
        for (size_t k = 0; k + 1 < cross.size(); k += 2) {
            const Id a = cross[k].second, b = cross[k + 1].second;
            if (a == kNone || b == kNone || a == b || m.findEdge(a, b) != kNone) continue;
            // The whole chord within the knife.
            bool inside = true;
            for (int q = 1; q < 8 && inside; ++q) inside = inKnife(m.pos(a) + (m.pos(b) - m.pos(a)) * (q / 8.0));
            if (inside) mine.push_back({a, b});
        }
        if (mine.empty()) continue;
        // The face's outline and the chords, as a flat arrangement seen along
        // the face's normal: its bounded regions are the new faces.
        std::vector<Id> es;
        for (const auto* L : loops)
            for (size_t k = 0; k < L->size(); ++k) es.push_back(m.findEdge((*L)[k], (*L)[(k + 1) % L->size()]));
        for (const auto& [a, b] : mine) es.push_back(m.addEdgeRaw(a, b));
        es.erase(std::remove(es.begin(), es.end(), kNone), es.end());
        const Plane FP(F.normal, m.pos(F.outer[0]));
        std::vector<Mesh::Region> regions = m.computeRegions(FP, es);
        Vec3 bu, bv;
        planeBasis(F.normal, bu, bv);
        std::vector<Mesh::Region> keep;
        for (auto& r : regions) {
            // Inside the original face (not one of its holes)?
            std::vector<std::vector<Vec2>> l2;
            std::vector<Vec2> all;
            auto add = [&](const std::vector<Id>& L) {
                l2.emplace_back();
                for (Id x : L) l2.back().push_back(Vec2(m.pos(x).dot(bu), m.pos(x).dot(bv))), all.push_back(l2.back().back());
            };
            add(r.outer);
            for (const auto& h : r.holes) add(h);
            Vec2 c = all[0];
            double best = -1;
            for (const auto& t : triangulatePolygon(l2)) {
                const double a = std::fabs((all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]));
                if (a > best) best = a, c = (all[t[0]] + all[t[1]] + all[t[2]]) * (1.0 / 3);
            }
            if (m.pointInFace(f, bu * c.x + bv * c.y, false)) keep.push_back(std::move(r));
        }
        if (keep.size() < 2) {
            for (const auto& [a, b] : mine) {
                const Id e = m.findEdge(a, b);
                bool used = false;
                for (Id g : m.edgeFaces(e)) used = used || m.faces[g].alive;
                if (e != kNone && !used) m.edges[e].alive = false;
            }
            m.touch();
            continue;
        }
        m.eraseFace(f);
        for (const auto& r : keep) {
            const Id nf = m.addFaceRaw(r.outer, r.holes, F.normal);
            if (nf == kNone) continue;
            m.faces[nf].front = F.front;
            m.faces[nf].back = F.back;
            m.faces[nf].hidden = F.hidden;
        }
        chords.insert(chords.end(), mine.begin(), mine.end());
    }
    m.touch();
    for (const auto& [a, b] : chords) {
        const Id e = m.findEdge(a, b);
        if (e != kNone) m.edges[e].soft = m.edges[e].smooth = m.edges[e].hidden = false;
    }

    // 3. Openings of a closed solid: regions the cut edges enclose in the
    //    knife's plane, inside the solid (not across a hole through it).
    std::vector<char> cap;
    if (solid) {
        std::vector<Id> planeEdges;
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            const Vec3 a = m.pos(m.edges[e].v[0]), b = m.pos(m.edges[e].v[1]);
            if (snap(P.distance(a)) == 0 && snap(P.distance(b)) == 0 && inKnife((a + b) * 0.5)) planeEdges.push_back(e);
        }
        Vec3 bu, bv;
        planeBasis(P.n, bu, bv);
        for (const auto& r : m.computeRegions(P, planeEdges)) {
            std::vector<std::vector<Vec2>> l2;
            std::vector<Vec2> all;
            auto add = [&](const std::vector<Id>& L) {
                l2.emplace_back();
                for (Id x : L) l2.back().push_back(Vec2(m.pos(x).dot(bu), m.pos(x).dot(bv))), all.push_back(l2.back().back());
            };
            add(r.outer);
            for (const auto& h : r.holes) add(h);
            Vec2 c2 = all[0];
            double best = -1;
            for (const auto& t : triangulatePolygon(l2)) {
                const double a = std::fabs((all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]));
                if (a > best) best = a, c2 = (all[t[0]] + all[t[1]] + all[t[2]]) * (1.0 / 3);
            }
            const Vec3 c = P.n * P.d + bu * c2.x + bv * c2.y;
            bool old = false;
            for (Id g : coplanarBefore) old = old || before.pointInFace(g, c, true);
            if (old || !inKnife(c) || !pointInSolid(before, c)) continue;
            const Id nf = m.addFaceRaw(r.outer, r.holes, P.n);
            if (nf == kNone) continue;
            cap.resize(m.faces.size(), 0);
            cap[nf] = 1;
        }
    }
    cap.resize(m.faces.size(), 0);

    // Side of the knife each face lies on.
    std::vector<int> side(m.faces.size(), 0);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && !cap[f]) {
            const double d = P.distance(m.faceInteriorPoint(f));
            side[f] = d > kTol ? 1 : d < -kTol ? -1 : 0;
        }
    // Edges the knife went along: faces on both sides of it meet there.
    auto isCut = [&](Id e) {
        const Vec3 a = m.pos(m.edges[e].v[0]), b = m.pos(m.edges[e].v[1]);
        if (snap(P.distance(a)) != 0 || snap(P.distance(b)) != 0 || !inKnife((a + b) * 0.5)) return false;
        bool pos = false, neg = false;
        for (Id f : m.edgeFaces(e))
            if (m.faces[f].alive && !cap[f]) {
                pos = pos || side[f] > 0;
                neg = neg || side[f] < 0;
            }
        return pos && neg;
    };
    // Parts: faces joined across any edge but a cut one.
    std::vector<int> part(m.faces.size(), -1);
    int parts = 0;
    for (Id seed = 0; seed < (Id)m.faces.size(); ++seed) {
        if (!m.faces[seed].alive || cap[seed] || part[seed] >= 0) continue;
        std::vector<Id> stack{seed};
        part[seed] = parts;
        while (!stack.empty()) {
            const Id f = stack.back();
            stack.pop_back();
            for (Id e : m.faceEdges(f)) {
                if (isCut(e)) continue;
                for (Id g : m.edgeFaces(e))
                    if (m.faces[g].alive && !cap[g] && part[g] < 0) part[g] = parts, stack.push_back(g);
            }
        }
        ++parts;
    }
    // Parts the knife never touched stay together with the first cut part
    // (the component keeps them); only what was cut apart is separated.
    {
        std::vector<char> touched(parts, 0);
        for (Id e = 0; e < (Id)m.edges.size(); ++e)
            if (m.edges[e].alive && isCut(e))
                for (Id f : m.edgeFaces(e))
                    if (m.faces[f].alive && !cap[f]) touched[part[f]] = 1;
        int firstTouched = -1, n = 0;
        std::vector<int> renum(parts, -1);
        for (int k = 0; k < parts; ++k)
            if (touched[k]) {
                if (firstTouched < 0) firstTouched = k;
                ++n;
            }
        if (n < 2) {
            // Nothing cut at all: is the knife even near it?
            Vec3 tmn, tmx;
            bool tany = false;
            targetIn.bounds(tmn, tmx, tany);
            bool overlap = tany;
            for (int i = 0; i < 3; ++i) overlap = overlap && kmx[i] >= tmn[i] - kTol && kmn[i] <= tmx[i] + kTol;
            return fail(overlap || n > 0 ? notThrough : "The knife doesn't touch it: place the flat knife so it passes through the component.");
        }
        int next = 1;
        for (int k = 0; k < parts; ++k) renum[k] = k == firstTouched || !touched[k] ? 0 : next++;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (part[f] >= 0) part[f] = renum[part[f]];
        parts = n;
    }

    // Caps go to the parts on either side of them, facing out of each.
    std::vector<std::vector<std::pair<Id, Vec3>>> partCaps(parts); // cap face, normal hint
    std::vector<Id> capMaterial(m.faces.size(), kNone);
    if (solid)
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive || !cap[f]) continue;
            int above = -1, below = -1;
            bool mixed = false;
            for (Id e : m.faceEdges(f))
                for (Id g : m.edgeFaces(e)) {
                    if (!m.faces[g].alive || cap[g] || side[g] == 0) continue;
                    int& slot = side[g] > 0 ? above : below;
                    if (slot >= 0 && slot != part[g]) mixed = true;
                    slot = part[g];
                    if (capMaterial[f] == kNone) capMaterial[f] = m.faces[g].front.material;
                }
            if (above < 0 || below < 0 || above == below || mixed) continue;
            partCaps[above].push_back({f, -P.n});
            partCaps[below].push_back({f, P.n});
        }

    // Loose edges go with the part on their side.
    std::vector<int> partSide(parts, 0);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && !cap[f] && side[f] != 0 && partSide[part[f]] == 0) partSide[part[f]] = side[f];
    for (int k = 0; k < parts; ++k) {
        Mesh piece = m;
        std::vector<Id> loose;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            if (cap[f] || part[f] != k) {
                for (Id e : piece.faceEdges(f)) loose.push_back(e);
                piece.eraseFace(f);
            }
        }
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            bool hasFace = false;
            for (Id g : m.edgeFaces(e)) hasFace = hasFace || m.faces[g].alive;
            if (hasFace) continue;
            const double d = P.distance((m.pos(m.edges[e].v[0]) + m.pos(m.edges[e].v[1])) * 0.5);
            const int s = d >= 0 ? 1 : -1;
            int owner = 0;
            for (int q = 0; q < parts; ++q)
                if (partSide[q] == s) { owner = q; break; }
            if (owner != k) piece.edges[e].alive = false;
        }
        for (const auto& [f, hint] : partCaps[k]) {
            const Id nf = piece.addFaceRaw(m.faces[f].outer, m.faces[f].holes, hint);
            if (nf != kNone) piece.faces[nf].front.material = piece.faces[nf].back.material = capMaterial[f];
        }
        piece.touch();
        piece.removeEdgesWithoutFaces(loose);
        // The cut edges stay drawn on the cut.
        for (Id e = 0; e < (Id)piece.edges.size(); ++e)
            if (piece.edges[e].alive && e < (Id)m.edges.size() && isCut(e)) piece.edges[e].soft = piece.edges[e].smooth = piece.edges[e].hidden = false;
        piece.removeOrphanVertices();
        piece.compact();
        pieces.push_back(std::move(piece));
    }
    return true;
}

bool knifeOperation(ModelState& st, Id knifeInstance, Id knifeFace, Id target, Selection* result, std::string* error) {
    if (!aliveInstance(st, target) || target == knifeInstance) {
        if (error) *error = "Pick the knife, then a different component to cut.";
        return false;
    }
    ModelState work = st;
    closeComponent(work);
    Mesh knife;
    if (knifeInstance != kNone) {
        if (!aliveInstance(work, knifeInstance)) {
            if (error) *error = "The knife is gone: pick it again.";
            return false;
        }
        knife = worldMesh(work, knifeInstance);
    } else {
        const Mesh& root = work.mesh;
        if (knifeFace < 0 || knifeFace >= (Id)root.faces.size() || !root.faces[knifeFace].alive) {
            if (error) *error = "The knife face is gone: pick it again.";
            return false;
        }
        // The face and the flat faces joined to it in its plane.
        const Plane FP(root.faces[knifeFace].normal, root.pos(root.faces[knifeFace].outer[0]));
        std::set<Id> region{knifeFace};
        std::vector<Id> stack{knifeFace};
        while (!stack.empty()) {
            const Id f = stack.back();
            stack.pop_back();
            for (Id e : root.faceEdges(f))
                for (Id g : root.edgeFaces(e)) {
                    if (!root.faces[g].alive || region.count(g) || std::fabs(root.faces[g].normal.dot(FP.n)) < 1 - 1e-9) continue;
                    bool flat = true;
                    for (Id v : root.faceVertices(g)) flat = flat && FP.contains(root.pos(v));
                    if (flat) region.insert(g), stack.push_back(g);
                }
        }
        appendMesh(knife, root, Mat4(), &region);
    }
    std::vector<Mesh> pieces;
    if (!knifeMesh(worldMesh(work, target), knife, pieces, error)) return false;
    const std::string name = work.defs[work.instances[target].def].name;
    std::vector<Id> made{target};
    for (size_t k = 1; k < pieces.size(); ++k) {
        const Id c = cloneInstance(work, target);
        placeResult(work, c, pieces[k], name + " Part " + std::to_string(k + 1));
        made.push_back(c);
    }
    placeResult(work, target, pieces[0], "");
    if (result) {
        result->clear();
        for (Id i : made)
            if (work.instances[i].alive) result->instances.insert(i);
    }
    st = std::move(work);
    return true;
}

} // namespace forma
