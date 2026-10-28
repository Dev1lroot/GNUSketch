// SPDX-License-Identifier: GPL-3.0-or-later
#include "Solids.h"

#include "Components.h"
#include "Triangulate.h"

#include <array>
#include <map>

namespace gs {

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
        if (t1 - t0 <= kTol) continue;
        if (!m.pointInFace(f, o + d * ((t0 + t1) * 0.5), true)) continue;
        if (!r.empty() && r.back().second >= t0 - kTol) r.back().second = t1;
        else r.push_back({t0, t1});
    }
    return r;
}

// Vertex at p for an edit of face f: an existing vertex, one inserted into an
// edge of f (keeping the neighbouring face in sync), or a new one.
Id faceVertex(Mesh& m, Id f, const Vec3& p) {
    Id v = m.findVertex(p);
    if (v != kNone) return v;
    for (Id e : m.faceEdges(f)) {
        const Vec3& a = m.pos(m.edges[e].v[0]);
        const Vec3& b = m.pos(m.edges[e].v[1]);
        double t = closestParamOnSegment(a, b, p);
        Vec3 q = lerp(a, b, t);
        if ((q - p).length() > kTol) continue;
        Id nv = m.addVertexRaw(q);
        std::vector<Id> fs = m.edgeFaces(e);
        m.splitEdge(e, nv, &fs);
        return nv;
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
        if (!m.pointInFace(f, regionPoint(m, P.n, r), false)) continue;
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
                        if (hi - lo <= kTol) continue;
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

} // namespace gs
