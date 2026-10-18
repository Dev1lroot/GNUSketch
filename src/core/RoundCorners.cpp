// SPDX-License-Identifier: GPL-3.0-or-later
#include "RoundCorners.h"

#include <map>
#include <optional>

namespace gs {

namespace {

struct Line {
    Vec3 p, d;
};

std::optional<Vec3> intersectLines(const Line& a, const Line& b) {
    double s, t;
    if (!closestLineLine(a.p, a.d, b.p, b.d, s, t)) return std::nullopt;
    Vec3 pa = a.p + a.d * s, pb = b.p + b.d * t;
    if ((pa - pb).length() > kTol * 10) return std::nullopt;
    return (pa + pb) * 0.5;
}

// Solves rows[i] . x = rhs[i].
std::optional<Vec3> solve3(const Vec3 r[3], const double rhs[3]) {
    Vec3 c12 = r[1].cross(r[2]), c20 = r[2].cross(r[0]), c01 = r[0].cross(r[1]);
    double det = r[0].dot(c12);
    if (std::fabs(det) < 1e-12) return std::nullopt;
    return (c12 * rhs[0] + c20 * rhs[1] + c01 * rhs[2]) / det;
}

Vec3 slerp(const Vec3& a, const Vec3& b, double phi, double t) {
    if (phi < 1e-9) return a;
    return (a * std::sin((1 - t) * phi) + b * std::sin(t * phi)) / std::sin(phi);
}

Vec3 polyNormal(const std::vector<Vec3>& L) {
    return newellNormal((int)L.size(), [&](int i) { return L[i]; });
}

void dedupe(std::vector<Vec3>& L) {
    std::vector<Vec3> r;
    for (const Vec3& p : L)
        if (r.empty() || !r.back().nearlyEquals(p)) r.push_back(p);
    while (r.size() > 1 && r.front().nearlyEquals(r.back())) r.pop_back();
    L = std::move(r);
}

// True if the loop, projected along n, does not cross itself.
bool simpleLoop(const std::vector<Vec3>& L, const Vec3& n) {
    Vec3 u, v;
    planeBasis(n, u, v);
    std::vector<Vec2> p;
    for (const Vec3& q : L) p.emplace_back(q.dot(u), q.dot(v));
    const int k = (int)p.size();
    auto orient = [](const Vec2& a, const Vec2& b, const Vec2& c) { return (b - a).cross(c - a); };
    const double eps = kTol * kTol;
    for (int i = 0; i < k; ++i) {
        Vec2 a = p[i], b = p[(i + 1) % k];
        for (int j = i + 2; j < k; ++j) {
            if (i == 0 && j == k - 1) continue; // adjacent through the wrap
            Vec2 c = p[j], d = p[(j + 1) % k];
            double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
            if (((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) &&
                ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps)))
                return false;
        }
    }
    return true;
}

struct NewFace {
    std::vector<std::vector<Vec3>> loops; // outer first
    Vec3 hint;
    FaceSide front, back;
    bool hidden = false;
};

// A rounded 3D edge: cylinder of the given radius tangent to both faces.
struct Fillet {
    Id e, a, b;
    Vec3 dir;           // unit, a -> b
    Id f[2];            // the two faces
    Vec3 n[2];          // their normals
    Vec3 dn[2];         // sigma * n: directions from the axis to the tangent lines
    double sigma = 1;   // +1 convex edge, -1 concave edge
    double phi = 0;     // angle between dn[0] and dn[1]
    Vec3 c;             // axis point (level with a)
    std::vector<Vec3> prof[2]; // profiles at a and b; index 0 lies on face f[0]

    int side(Id face) const { return face == f[0] ? 0 : 1; }
    Vec3 ruling(double R, double t) const { return c + slerp(dn[0], dn[1], phi, t) * R; }
    Line offsetLine(double R, Id face) const { return {c + dn[side(face)] * R, dir}; }
};

struct Corner {
    Id f, ein, eout; // the face's loop arrives at the vertex via ein and leaves via eout
};

} // namespace

bool roundCorners(Mesh& m, const std::set<Id>& edgeSet, double R, int N, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (R <= kTol) return fail("The radius must be greater than zero.");
    N = std::clamp(N, 1, 256);

    auto aliveFaces = [&](Id e) {
        std::vector<Id> r;
        for (Id g : m.edgeFaces(e))
            if (m.faces[g].alive) r.push_back(g);
        return r;
    };
    auto aliveEdges = [&](Id v) {
        std::vector<Id> r;
        for (Id e : m.vertexEdges(v))
            if (m.edges[e].alive) r.push_back(e);
        return r;
    };
    auto edgeLine = [&](Id e) {
        Vec3 a = m.pos(m.edges[e].v[0]);
        return Line{a, (m.pos(m.edges[e].v[1]) - a).normalized()};
    };
    auto cornersAt = [&](Id v) {
        std::vector<Corner> r;
        std::set<Id> seen;
        for (Id e : aliveEdges(v))
            for (Id f : aliveFaces(e)) {
                if (!seen.insert(f).second) continue;
                const Face& F = m.faces[f];
                std::vector<const std::vector<Id>*> loops = {&F.outer};
                for (const auto& h : F.holes) loops.push_back(&h);
                for (const auto* L : loops) {
                    const size_t k = L->size();
                    for (size_t i = 0; i < k; ++i)
                        if ((*L)[i] == v)
                            r.push_back({f, m.findEdge((*L)[(i + k - 1) % k], v), m.findEdge(v, (*L)[(i + 1) % k])});
                }
            }
        return r;
    };

    // ---- classify the selection --------------------------------------------
    std::map<Id, Fillet> fillets; // 3D edges
    std::set<Id> flat;            // other selected edges (2D candidates)
    for (Id e : edgeSet) {
        if (e < 0 || e >= (Id)m.edges.size() || !m.edges[e].alive) continue;
        std::vector<Id> fs = aliveFaces(e);
        if (fs.size() > 2) return fail("An edge used by more than two faces cannot be rounded.");
        if (fs.size() == 2 && std::fabs(m.faces[fs[0]].normal.dot(m.faces[fs[1]].normal)) < 1 - 1e-6) {
            Fillet F;
            F.e = e;
            F.a = m.edges[e].v[0];
            F.b = m.edges[e].v[1];
            F.dir = (m.pos(F.b) - m.pos(F.a)).normalized();
            Vec3 in[2];
            for (int i = 0; i < 2; ++i) {
                F.f[i] = fs[i];
                F.n[i] = m.faces[fs[i]].normal;
                // Loops run CCW around the normal (holes CW), so n x travel
                // direction points into the face.
                int d = m.faceEdgeDirection(fs[i], F.a, F.b);
                in[i] = F.n[i].cross(F.dir * double(d));
            }
            bool convex0 = in[1].dot(F.n[0]) < 0, convex1 = in[0].dot(F.n[1]) < 0;
            if (convex0 != convex1) return fail("The faces next to a selected edge are inconsistently oriented; reverse faces first.");
            F.sigma = convex0 ? 1.0 : -1.0;
            for (int i = 0; i < 2; ++i) F.dn[i] = F.n[i] * F.sigma;
            F.phi = std::acos(std::clamp(F.dn[0].dot(F.dn[1]), -1.0, 1.0));
            const Vec3 A = m.pos(F.a);
            Vec3 rows[3] = {F.dir, F.n[0], F.n[1]};
            double rhs[3] = {F.dir.dot(A), F.n[0].dot(A) - F.sigma * R, F.n[1].dot(A) - F.sigma * R};
            auto c = solve3(rows, rhs);
            if (!c) return fail("Could not compute the rounding of an edge.");
            F.c = *c;
            fillets[e] = F;
        } else {
            flat.insert(e);
        }
    }

    std::map<std::pair<Id, Id>, std::vector<Vec3>> repl; // (face, vertex) -> replacement points
    std::set<Id> rebuild;                                // faces to rebuild
    std::set<Id> deadVerts;                              // vertices that disappear
    std::vector<NewFace> extra;                          // strips, mitres, patches
    std::vector<std::pair<Vec3, Vec3>> softPairs;
    std::vector<std::vector<Vec3>> arcs;                 // 2D arcs (one curve each)
    std::vector<Vec3> splitPts;                          // points to insert into collinear face edges

    // ---- 3D corners ---------------------------------------------------------
    std::set<Id> corners3;
    for (const auto& [e, F] : fillets) {
        corners3.insert(F.a);
        corners3.insert(F.b);
    }
    for (Id V : corners3) {
        std::vector<Id> es = aliveEdges(V);
        std::vector<Corner> cs = cornersAt(V);
        if (es.size() != 3 || cs.size() != 3)
            return fail("Rounded edges must end at corners where exactly three faces meet.");
        std::vector<Id> rounded;
        for (Id e : es)
            if (fillets.count(e)) rounded.push_back(e);
        const int k = (int)rounded.size();
        const Vec3 P = m.pos(V);

        auto faceOf = [&](Id ea, Id eb) -> const Corner* {
            for (const Corner& c : cs)
                if ((c.ein == ea && c.eout == eb) || (c.ein == eb && c.eout == ea)) return &c;
            return nullptr;
        };
        // Corner point of every face that touches a rounded edge.
        std::map<Id, Vec3> cornerPt;
        for (const Corner& c : cs) {
            bool ri = fillets.count(c.ein) > 0, ro = fillets.count(c.eout) > 0;
            if (!ri && !ro) continue;
            Line li = ri ? fillets[c.ein].offsetLine(R, c.f) : edgeLine(c.ein);
            Line lo = ro ? fillets[c.eout].offsetLine(R, c.f) : edgeLine(c.eout);
            auto q = intersectLines(li, lo);
            if (!q) return fail("Could not compute the rounding at a corner.");
            cornerPt[c.f] = *q;
            repl[{c.f, V}] = {*q};
        }
        for (const Corner& c : cs) rebuild.insert(c.f);
        deadVerts.insert(V);

        // Termination plane of each rounded edge's strip at this corner.
        std::map<Id, Plane> stop;
        Vec3 O;
        if (k == 1) {
            const Fillet& F = fillets[rounded[0]];
            for (const Corner& c : cs)
                if (c.f != F.f[0] && c.f != F.f[1]) stop[F.e] = Plane(m.faces[c.f].normal, P);
        } else if (k == 2) {
            Id plain = kNone;
            for (Id e : es)
                if (!fillets.count(e)) plain = e;
            const Corner* both = faceOf(rounded[0], rounded[1]);
            if (!both) return fail("Could not compute the rounding at a corner.");
            Vec3 axis = (m.pos(m.edges[plain].other(V)) - P).normalized();
            Vec3 n = axis.cross(cornerPt[both->f] - P);
            if (n.length() < 1e-12) return fail("The radius is too large for a corner.");
            for (Id e : rounded) stop[e] = Plane(n, P);
        } else {
            Vec3 rows[3];
            double rhs[3];
            for (int i = 0; i < 3; ++i) {
                if (fillets[rounded[i]].sigma < 0)
                    return fail("Three rounded edges meeting at a concave corner are not supported.");
                rows[i] = m.faces[cs[i].f].normal;
                rhs[i] = rows[i].dot(P) - R;
            }
            auto o = solve3(rows, rhs);
            if (!o) return fail("Could not compute the rounding at a corner.");
            O = *o;
            for (Id e : rounded) {
                const Fillet& F = fillets[e];
                Vec3 n = (cornerPt[F.f[0]] - O).cross(cornerPt[F.f[1]] - O);
                if (n.length() < 1e-14) return fail("Could not compute the rounding at a corner.");
                stop[e] = Plane(n, O);
            }
        }

        for (Id e : rounded) {
            Fillet& F = fillets[e];
            const Plane& pl = stop[e];
            double dn = pl.n.dot(F.dir);
            if (std::fabs(dn) < 1e-9) return fail("Could not compute the rounding at a corner.");
            std::vector<Vec3> prof;
            for (int i = 0; i <= N; ++i) {
                Vec3 q = F.ruling(R, double(i) / N);
                prof.push_back(q + F.dir * ((pl.d - pl.n.dot(q)) / dn));
            }
            prof.front() = cornerPt[F.f[0]];
            prof.back() = cornerPt[F.f[1]];
            F.prof[V == F.a ? 0 : 1] = prof;
        }

        if (k == 1) {
            // The end face takes the profile in place of the corner.
            const Fillet& F = fillets[rounded[0]];
            std::vector<Vec3> prof = F.prof[V == F.a ? 0 : 1];
            for (const Corner& c : cs) {
                if (c.f == F.f[0] || c.f == F.f[1]) continue;
                Line li = edgeLine(c.ein);
                auto distTo = [&](const Vec3& p) { return ((p - li.p) - li.d * (p - li.p).dot(li.d)).length(); };
                if (distTo(prof.front()) > distTo(prof.back())) std::reverse(prof.begin(), prof.end());
                repl[{c.f, V}] = prof;
            }
        } else if (k == 2) {
            // Both strips end in the mitre plane. Where they do not meet exactly
            // the leftover gap is a planar face in that plane.
            const Corner* both = faceOf(rounded[0], rounded[1]);
            const Vec3 Q = cornerPt[both->f];
            std::vector<Vec3> p0 = fillets[rounded[0]].prof[V == fillets[rounded[0]].a ? 0 : 1];
            std::vector<Vec3> p1 = fillets[rounded[1]].prof[V == fillets[rounded[1]].a ? 0 : 1];
            if (!p0.front().nearlyEquals(Q)) std::reverse(p0.begin(), p0.end());
            if (!p1.front().nearlyEquals(Q)) std::reverse(p1.begin(), p1.end());
            for (int i = 0; i < N; ++i) {
                softPairs.emplace_back(p0[i], p0[i + 1]);
                softPairs.emplace_back(p1[i], p1[i + 1]);
            }
            splitPts.push_back(p0.back());
            splitPts.push_back(p1.back());
            std::vector<Vec3> gap = p0;
            for (int i = N; i >= 1; --i) gap.push_back(p1[i]);
            dedupe(gap);
            Vec3 gn = gap.size() >= 3 ? polyNormal(gap) : Vec3();
            if (gn.length() > 1e-9) {
                NewFace nf;
                nf.loops = {gap};
                Vec3 out;
                for (const Corner& c : cs) out += m.faces[c.f].normal;
                nf.hint = gn.dot(out) >= 0 ? gn : -gn;
                nf.front = m.faces[both->f].front;
                nf.back = m.faces[both->f].back;
                nf.front.tex.valid = nf.back.tex.valid = false;
                extra.push_back(std::move(nf));
            }
        } else {
            // Spherical patch spanned by the three corner points.
            const Vec3 Qa = cornerPt[cs[0].f], Qb = cornerPt[cs[1].f], Qc = cornerPt[cs[2].f];
            auto between = [&](Id fa, Id fb, const Vec3& from) {
                for (Id e : rounded) {
                    const Fillet& F = fillets[e];
                    if ((F.f[0] == fa && F.f[1] == fb) || (F.f[0] == fb && F.f[1] == fa)) {
                        std::vector<Vec3> p = F.prof[V == F.a ? 0 : 1];
                        if (!p.front().nearlyEquals(from)) std::reverse(p.begin(), p.end());
                        return p;
                    }
                }
                return std::vector<Vec3>();
            };
            std::vector<Vec3> AB = between(cs[0].f, cs[1].f, Qa), BC = between(cs[1].f, cs[2].f, Qb),
                              CA = between(cs[2].f, cs[0].f, Qc);
            if (AB.empty() || BC.empty() || CA.empty()) return fail("Could not compute the rounding at a corner.");
            // G(i, j): barycentric (i, j, N - i - j) towards (Qa, Qb, Qc).
            auto G = [&](int i, int j) {
                int l = N - i - j;
                if (l == 0) return AB[j];
                if (i == 0) return BC[l];
                if (j == 0) return CA[i];
                Vec3 d = (Qa - O).normalized() * i + (Qb - O).normalized() * j + (Qc - O).normalized() * l;
                return O + d.normalized() * R;
            };
            auto tri = [&](const Vec3& p, const Vec3& q, const Vec3& r) {
                NewFace nf;
                nf.loops = {{p, q, r}};
                nf.hint = (p + q + r) / 3.0 - O;
                nf.front = m.faces[cs[0].f].front;
                nf.back = m.faces[cs[0].f].back;
                nf.front.tex.valid = nf.back.tex.valid = false;
                extra.push_back(std::move(nf));
                softPairs.emplace_back(p, q);
                softPairs.emplace_back(q, r);
                softPairs.emplace_back(r, p);
            };
            for (int i = 0; i < N; ++i)
                for (int j = 0; i + j < N; ++j) {
                    tri(G(i, j), G(i + 1, j), G(i, j + 1));
                    if (i + j < N - 1) tri(G(i + 1, j), G(i + 1, j + 1), G(i, j + 1));
                }
        }
    }

    // ---- strips along the rounded edges ---------------------------------------
    for (const auto& [e, F] : fillets) {
        const auto &pa = F.prof[0], &pb = F.prof[1];
        for (int i = 0; i <= N; ++i)
            if ((pb[i] - pa[i]).dot(F.dir) <= kTol) return fail("The radius is too large for the selected edges.");
        for (int i = 0; i < N; ++i) {
            const Vec3 hint = F.n[0] * (1 - (i + 0.5) / N) + F.n[1] * ((i + 0.5) / N);
            NewFace nf;
            nf.hint = hint;
            nf.front = m.faces[F.f[0]].front;
            nf.back = m.faces[F.f[0]].back;
            nf.front.tex.valid = nf.back.tex.valid = false;
            std::vector<Vec3> quad = {pa[i], pa[i + 1], pb[i + 1], pb[i]};
            Vec3 qn = polyNormal({pa[i], pa[i + 1], pb[i + 1]}).normalized();
            if (std::fabs((pb[i] - pa[i]).dot(qn)) > kTol) {
                // Not planar (uneven ends): split into two triangles.
                nf.loops = {{pa[i], pa[i + 1], pb[i + 1]}};
                extra.push_back(nf);
                nf.loops = {{pa[i], pb[i + 1], pb[i]}};
                extra.push_back(nf);
                softPairs.emplace_back(pa[i], pb[i + 1]);
            } else {
                nf.loops = {quad};
                extra.push_back(nf);
            }
        }
        for (int i = 0; i <= N; ++i) softPairs.emplace_back(pa[i], pb[i]);
    }

    // ---- 2D corners -----------------------------------------------------------
    // Trimmed end of a flat edge at a vertex (only set for rounded 2D vertices).
    std::map<std::pair<Id, Id>, Vec3> trimmed;
    std::set<Id> flatTouched;
    for (Id e : flat)
        for (Id V : m.edges[e].v) {
            if (trimmed.count({e, V}) || corners3.count(V)) continue;
            std::vector<Id> es = aliveEdges(V);
            if (es.size() != 2 || !flat.count(es[0]) || !flat.count(es[1])) continue;
            const Vec3 P = m.pos(V);
            Vec3 u[2];
            for (int i = 0; i < 2; ++i) u[i] = (m.pos(m.edges[es[i]].other(V)) - P).normalized();
            double cosT = std::clamp(u[0].dot(u[1]), -1.0, 1.0);
            if (cosT < -1 + 1e-9) continue; // collinear: nothing to round
            double half = std::acos(cosT) * 0.5;
            double d = R / std::tan(half);
            Vec3 T[2] = {P + u[0] * d, P + u[1] * d};
            Vec3 C = P + (u[0] + u[1]).normalized() * (R / std::sin(half));
            Vec3 r0 = (T[0] - C).normalized(), r1 = (T[1] - C).normalized();
            double phi = std::acos(std::clamp(r0.dot(r1), -1.0, 1.0));
            std::vector<Vec3> arc;
            for (int i = 0; i <= N; ++i) arc.push_back(C + slerp(r0, r1, phi, double(i) / N) * R);
            arc.front() = T[0];
            arc.back() = T[1];
            arcs.push_back(arc);
            trimmed[{es[0], V}] = T[0];
            trimmed[{es[1], V}] = T[1];
            flatTouched.insert(es[0]);
            flatTouched.insert(es[1]);
            deadVerts.insert(V);
            for (const Corner& c : cornersAt(V)) {
                std::vector<Vec3> pts = arc;
                if (c.ein != es[0]) std::reverse(pts.begin(), pts.end());
                repl[{c.f, V}] = pts;
                rebuild.insert(c.f);
            }
        }
    std::vector<std::pair<Vec3, Vec3>> lineSegs; // remaining straight parts of 2D edges
    for (Id e : flatTouched) {
        Id a = m.edges[e].v[0], b = m.edges[e].v[1];
        Vec3 pa = trimmed.count({e, a}) ? trimmed[{e, a}] : m.pos(a);
        Vec3 pb = trimmed.count({e, b}) ? trimmed[{e, b}] : m.pos(b);
        if ((pb - pa).dot(m.pos(b) - m.pos(a)) <= kTol) return fail("The radius is too large for the selected edges.");
        lineSegs.emplace_back(pa, pb);
    }

    if (deadVerts.empty()) return fail("Select edges of a shape to round: edges between two faces, or connected lines.");

    // ---- rebuilt faces ----------------------------------------------------------
    std::vector<NewFace> rebuilt;
    for (Id f : rebuild) {
        const Face& F = m.faces[f];
        NewFace nf;
        nf.hint = F.normal;
        nf.front = F.front;
        nf.back = F.back;
        nf.hidden = F.hidden;
        std::vector<const std::vector<Id>*> loops = {&F.outer};
        for (const auto& h : F.holes) loops.push_back(&h);
        for (const auto* L : loops) {
            std::vector<Vec3> pts;
            for (Id v : *L) {
                auto it = repl.find({f, v});
                if (it != repl.end()) pts.insert(pts.end(), it->second.begin(), it->second.end());
                else pts.push_back(m.pos(v));
            }
            // Mitre points that land on a straight edge of this face split it.
            std::vector<Vec3> withSplits;
            for (size_t i = 0; i < pts.size(); ++i) {
                const Vec3 &p = pts[i], &q = pts[(i + 1) % pts.size()];
                withSplits.push_back(p);
                std::vector<std::pair<double, Vec3>> on;
                for (const Vec3& s : splitPts) {
                    double t = closestParamOnSegment(p, q, s);
                    double len = (q - p).length();
                    if (len < kTol || t * len <= kTol || (1 - t) * len <= kTol) continue;
                    if ((lerp(p, q, t) - s).length() <= kTol) on.emplace_back(t, s);
                }
                std::sort(on.begin(), on.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                for (const auto& o : on) withSplits.push_back(o.second);
            }
            dedupe(withSplits);
            Vec3 nOld = polyNormal([&] {
                std::vector<Vec3> o;
                for (Id v : *L) o.push_back(m.pos(v));
                return o;
            }());
            if (withSplits.size() < 3 || polyNormal(withSplits).dot(nOld) <= 0 || !simpleLoop(withSplits, F.normal))
                return fail("The radius is too large for the selected edges.");
            nf.loops.push_back(std::move(withSplits));
        }
        rebuilt.push_back(std::move(nf));
    }

    // ---- apply ------------------------------------------------------------------
    for (Id f : rebuild) m.eraseFace(f);
    std::set<Id> deadEdges;
    for (Id V : deadVerts)
        for (Id e : aliveEdges(V)) deadEdges.insert(e);
    for (Id e : deadEdges) m.eraseEdge(e, false);

    auto build = [&](const NewFace& nf) {
        std::vector<std::vector<Id>> ids;
        for (const auto& L : nf.loops) {
            std::vector<Id> l;
            for (const Vec3& p : L) {
                Id v = m.addVertex(p);
                if (l.empty() || l.back() != v) l.push_back(v);
            }
            while (l.size() > 1 && l.front() == l.back()) l.pop_back();
            ids.push_back(std::move(l));
        }
        if (ids.empty() || ids[0].size() < 3) return;
        std::vector<std::vector<Id>> holes(ids.begin() + 1, ids.end());
        Id nf2 = m.addFaceRaw(ids[0], holes, nf.hint);
        if (nf2 == kNone) return;
        m.faces[nf2].front = nf.front;
        m.faces[nf2].back = nf.back;
        m.faces[nf2].hidden = nf.hidden;
    };
    for (const NewFace& nf : rebuilt) build(nf);
    for (const NewFace& nf : extra) build(nf);
    for (const auto& [a, b] : lineSegs) m.addEdgeRaw(m.addVertex(a), m.addVertex(b));
    for (const auto& arc : arcs) {
        Id curve = m.newCurveId();
        for (size_t i = 0; i + 1 < arc.size(); ++i) {
            Id e = m.addEdgeRaw(m.addVertex(arc[i]), m.addVertex(arc[i + 1]));
            if (e != kNone) m.edges[e].curve = curve;
        }
    }
    for (const auto& [a, b] : softPairs) {
        Id va = m.findVertex(a), vb = m.findVertex(b);
        if (va == kNone || vb == kNone) continue;
        Id e = m.findEdge(va, vb);
        if (e == kNone) continue;
        m.edges[e].soft = true;
        m.edges[e].smooth = true;
    }
    m.removeOrphanVertices();
    m.touch();
    m.markGeometryMoved();
    return true;
}

} // namespace gs
