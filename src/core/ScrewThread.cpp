// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScrewThread.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

namespace gs {

namespace {

constexpr int kMaxTriangles = 400000;

// One line along the thread: at `u` (share of the pitch) the surface stands
// `h` (0 = root, 1 = crest). Rows at a corner of the profile are drawn edges.
struct Row {
    double u, h;
    bool hard;
};

std::vector<Row> profileRows(const ScrewThreadParams& p) {
    switch (p.profile) {
    case ScrewThreadParams::V: return {{0, 0, true}, {0.5, 1, true}};
    case ScrewThreadParams::Metric:
        // ISO 60°: a flat root of P/4 and a flat crest of P/8.
        return {{0, 0, true}, {0.25, 0, true}, {0.5625, 1, true}, {0.6875, 1, true}};
    case ScrewThreadParams::Square: return {{0, 0, true}, {0.5, 0, true}, {0.5, 1, true}, {1, 1, true}};
    case ScrewThreadParams::Trapezoid: return {{0, 0, true}, {0.37, 0, true}, {0.5, 1, true}, {0.87, 1, true}};
    case ScrewThreadParams::Buttress:
        return {{0, 0, true}, {0.2, 0, true}, {0.7, 1, true}, {0.82, 1, true}, {0.86, 0, true}};
    case ScrewThreadParams::Rounded: {
        std::vector<Row> r;
        const int k = std::clamp(p.roundSteps, 3, 64);
        for (int j = 0; j < k; ++j) r.push_back({double(j) / k, 0.5 - 0.5 * std::cos(2 * kPi * j / k), false});
        return r;
    }
    }
    return {{0, 0, true}, {0.5, 1, true}};
}

// The selected faces seen as a cylinder.
struct Cylinder {
    Vec3 axis, u, v, center; // center: on the axis, at axial coordinate 0
    double r = 0;
    double zLo = 0, zHi = 0;
    int sign = 1;            // +1: faces face away from the axis (a rod), -1: towards it (a hole)
    std::vector<Id> side;
    std::vector<Id> ring[2]; // the end loops (vertex ids), low and high
};

bool fitCylinder(const Mesh& m, const std::vector<Id>& side, const Vec3& axisIn, Cylinder& c) {
    c = Cylinder();
    c.axis = axisIn.normalized();
    planeBasis(c.axis, c.u, c.v);
    std::set<Id> verts;
    for (Id f : side)
        for (Id v : m.faceVertices(f)) verts.insert(v);
    if (verts.size() < 6) return false;
    // Circle through the vertices seen along the axis (algebraic fit, centred
    // on their mean for a well-conditioned system).
    double mx = 0, my = 0;
    for (Id v : verts) {
        mx += m.pos(v).dot(c.u);
        my += m.pos(v).dot(c.v);
    }
    mx /= verts.size();
    my /= verts.size();
    double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sq = 0, sxq = 0, syq = 0;
    const double n = double(verts.size());
    for (Id v : verts) {
        const double x = m.pos(v).dot(c.u) - mx, y = m.pos(v).dot(c.v) - my, q = x * x + y * y;
        sxx += x * x;
        sxy += x * y;
        syy += y * y;
        sx += x;
        sy += y;
        sq += q;
        sxq += x * q;
        syq += y * q;
    }
    // [sxx sxy sx; sxy syy sy; sx sy n] (D E F) = -(sxq syq sq)
    const double a[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n}};
    const double b[3] = {-sxq, -syq, -sq};
    auto det3 = [](const double M[3][3]) {
        return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
               M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    };
    const double d = det3(a);
    if (std::fabs(d) < 1e-30) return false;
    double sol[3];
    for (int k = 0; k < 3; ++k) {
        double t[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t[i][j] = j == k ? b[i] : a[i][j];
        sol[k] = det3(t) / d;
    }
    const double cx = -sol[0] / 2, cy = -sol[1] / 2, r2 = cx * cx + cy * cy - sol[2];
    if (!(r2 > 0)) return false;
    c.r = std::sqrt(r2);
    c.center = c.u * (cx + mx) + c.v * (cy + my);
    for (Id v : verts) {
        const Vec3 d3 = m.pos(v) - c.center;
        const Vec3 radial = d3 - c.axis * d3.dot(c.axis);
        if (std::fabs(radial.length() - c.r) > std::max(1e-7, 0.005 * c.r)) return false;
    }

    // Which way the faces face.
    int out = 0, in = 0;
    for (Id f : side) {
        const Vec3 d3 = m.faceInteriorPoint(f) - c.center;
        const double s = m.faces[f].normal.dot(d3 - c.axis * d3.dot(c.axis));
        (s > 0 ? out : in)++;
    }
    if (out && in) return false;
    c.sign = out ? 1 : -1;

    // The ends: edges used by one face of the surface, chained into loops.
    std::map<Id, int> uses;
    for (Id f : side)
        for (Id e : m.faceEdges(f)) uses[e]++;
    std::unordered_map<Id, std::vector<Id>> next; // vertex -> boundary neighbours
    for (const auto& [e, k] : uses)
        if (k == 1) {
            const Edge& E = m.edges[e];
            next[E.v[0]].push_back(E.v[1]);
            next[E.v[1]].push_back(E.v[0]);
        }
    for (const auto& [v, ns] : next)
        if (ns.size() != 2) return false;
    std::set<Id> seen;
    std::vector<std::vector<Id>> loops;
    for (const auto& [start, ns] : next) {
        if (seen.count(start)) continue;
        std::vector<Id> L = {start};
        seen.insert(start);
        Id prev = start, cur = ns[0];
        while (cur != start) {
            if (!seen.insert(cur).second) return false;
            L.push_back(cur);
            const auto& cn = next[cur];
            const Id nx = cn[0] == prev ? cn[1] : cn[0];
            prev = cur;
            cur = nx;
        }
        loops.push_back(std::move(L));
    }
    // A box is not round: at least 8 points around.
    if (loops.size() != 2 || loops[0].size() < 8 || loops[1].size() < 8) return false;
    const double tol = std::max(1e-9, 1e-7 * c.r);
    double z[2];
    for (int k = 0; k < 2; ++k) {
        z[k] = m.pos(loops[k][0]).dot(c.axis);
        double turn = 0;
        for (size_t i = 0; i < loops[k].size(); ++i) {
            const Vec3 p = m.pos(loops[k][i]), q = m.pos(loops[k][(i + 1) % loops[k].size()]);
            if (std::fabs(p.dot(c.axis) - z[k]) > tol) return false;
            const double a0 = std::atan2((p - c.center).dot(c.v), (p - c.center).dot(c.u));
            const double a1 = std::atan2((q - c.center).dot(c.v), (q - c.center).dot(c.u));
            double da = a1 - a0;
            while (da > kPi) da -= 2 * kPi;
            while (da < -kPi) da += 2 * kPi;
            turn += da;
        }
        if (std::fabs(std::fabs(turn) - 2 * kPi) > 0.1) return false; // all the way round
    }
    if (std::fabs(z[0] - z[1]) < tol) return false;
    const int lo = z[0] < z[1] ? 0 : 1;
    c.zLo = z[lo];
    c.zHi = z[1 - lo];
    c.ring[0] = loops[lo];
    c.ring[1] = loops[1 - lo];
    c.side = side;
    return true;
}

struct Cap {
    Id face = kNone;
    int loop = -1; // -1: the outline, else a hole
    int ring = 0;
};

struct ColPoint {
    Vec3 p;
    double key = 0; // row index along the profile (fractional at the ends)
    bool end = false;
    Id id = kNone;
};

} // namespace

bool screwThread(Mesh& m, const std::set<Id>& facesIn, const ScrewThreadParams& p, ScrewThreadResult* out,
                 std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    std::vector<Id> faces;
    for (Id f : facesIn)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive) faces.push_back(f);
    if (faces.empty()) return fail("Select the side of a round rod or the inside of a round hole.");
    if (!(p.pitch > 0) || !(p.depth > 0)) return fail("The pitch and the depth must be more than 0.");

    // ---- the cylinder ------------------------------------------------------------
    // Its axis is square to the side faces' normals: try the cross product of
    // the two most different normals, then (selected caps included) the
    // normals of the largest faces.
    std::vector<Vec3> axes;
    {
        double best = 0;
        Vec3 a;
        const size_t lim = std::min<size_t>(faces.size(), 400);
        for (size_t i = 0; i < lim; ++i)
            for (size_t j = i + 1; j < lim; ++j) {
                const Vec3 c = m.faces[faces[i]].normal.cross(m.faces[faces[j]].normal);
                if (c.length() > best + 1e-9) {
                    best = c.length();
                    a = c;
                }
            }
        if (best > 0.05) axes.push_back(a.normalized());
        std::vector<Id> big = faces;
        std::sort(big.begin(), big.end(), [&](Id x, Id y) { return m.faceArea(x) > m.faceArea(y); });
        for (size_t i = 0; i < big.size() && i < 8; ++i) axes.push_back(m.faces[big[i]].normal);
    }
    Cylinder cyl;
    bool found = false;
    std::vector<Vec3> tried;
    for (const Vec3& a : axes) {
        if (std::any_of(tried.begin(), tried.end(), [&](const Vec3& t) { return std::fabs(t.dot(a)) > 0.9999; })) continue;
        tried.push_back(a);
        std::vector<Id> side;
        for (Id f : faces)
            if (std::fabs(m.faces[f].normal.dot(a)) < 0.02) side.push_back(f);
        if (side.size() < 3) continue;
        Cylinder c;
        if (fitCylinder(m, side, a, c) && (!found || c.side.size() > cyl.side.size())) {
            cyl = c;
            found = true;
        }
    }
    if (!found)
        return fail("Select the side of a round rod or the inside of a round hole: faces on one cylinder, all the way round, "
                    "with flat ends square to its axis.");
    const std::set<Id> sideSet(cyl.side.begin(), cyl.side.end());

    // ---- what joins it: flat caps at the ends, nothing else -----------------------
    std::vector<Cap> caps;
    std::set<Id> ringEdges[2];
    for (int k = 0; k < 2; ++k) {
        const auto& R = cyl.ring[k];
        for (size_t i = 0; i < R.size(); ++i) ringEdges[k].insert(m.findEdge(R[i], R[(i + 1) % R.size()]));
    }
    std::set<Id> capFaces;
    for (Id f : cyl.side)
        for (Id e : m.faceEdges(f)) {
            const int ring = ringEdges[0].count(e) ? 0 : ringEdges[1].count(e) ? 1 : -1;
            for (Id g : m.edgeFaces(e)) {
                if (!m.faces[g].alive || sideSet.count(g)) continue;
                if (ring < 0) return fail("Something else is joined to the round surface: only its two ends may join other faces.");
                if (capFaces.count(g)) continue;
                const Face& G = m.faces[g];
                if (std::fabs(G.normal.dot(cyl.axis)) < 0.9999)
                    return fail("The ends of the round surface must be flat faces square to its axis (or open).");
                const std::set<Id> ringSet(cyl.ring[ring].begin(), cyl.ring[ring].end());
                auto isRing = [&](const std::vector<Id>& L) {
                    if (L.size() != ringSet.size()) return false;
                    for (Id v : L)
                        if (!ringSet.count(v)) return false;
                    return true;
                };
                Cap c;
                c.face = g;
                c.ring = ring;
                if (isRing(G.outer)) c.loop = -1;
                else {
                    c.loop = -2;
                    for (size_t h = 0; h < G.holes.size(); ++h)
                        if (isRing(G.holes[h])) c.loop = int(h);
                }
                if (c.loop == -2)
                    return fail("The ends of the round surface must be whole outlines (or holes) of flat faces.");
                capFaces.insert(g);
                caps.push_back(c);
            }
        }

    // ---- the thread ------------------------------------------------------------
    const std::vector<Row> rows = profileRows(p);
    const int M = int(rows.size());
    const int S = std::clamp(p.starts, 1, 32);
    const int hand = p.leftHand ? -1 : 1;
    const int N = std::clamp(p.segments, 6, 720);
    const double P = p.pitch, lead = S * P, H = cyl.zHi - cyl.zLo;
    // Where the surface goes from the cylinder: carving keeps the crest on it,
    // raising keeps the root on it; along the faces' normals.
    auto offset = [&](double h) { return cyl.sign * (p.inward ? -p.depth * (1 - h) : p.depth * h); };
    if (cyl.sign * (p.inward ? -1 : 1) < 0 && p.depth >= 0.9 * cyl.r)
        return fail("The thread is deeper than the radius: less depth.");
    const double estRows = (H / P + S + 2) * M + 2;
    if (2.0 * N * estRows > kMaxTriangles)
        return fail("The thread would have too many polygons: a larger pitch, fewer segments, or a shorter surface.");

    // The profile at w pitches along a helix row: height and row key.
    auto at = [&](double w, double& h, double& key) {
        const double k = std::floor(w), f = w - k;
        int i = 0;
        for (int j = 0; j < M; ++j)
            if (rows[j].u <= f) i = j;
        const double u1 = i + 1 < M ? rows[i + 1].u : 1 + rows[0].u;
        const double h1 = i + 1 < M ? rows[i + 1].h : rows[0].h;
        const double t = u1 - rows[i].u > 1e-12 ? (f - rows[i].u) / (u1 - rows[i].u) : 0.5;
        h = rows[i].h + (h1 - rows[i].h) * t;
        key = k * M + i + t;
    };
    const double eps = std::max(1e-9, 1e-6 * P);
    std::vector<std::vector<ColPoint>> cols(N);
    for (int i = 0; i < N; ++i) {
        const double th = 2 * kPi * i / N;
        const Vec3 dir = cyl.u * std::cos(th) + cyl.v * std::sin(th);
        const double ci = hand * lead * i / N; // helix rows shift along the axis turning round
        auto point = [&](double z, double h) { return cyl.center + cyl.axis * z + dir * (cyl.r + offset(h)); };
        auto& C = cols[i];
        ColPoint e0;
        double h;
        at(-ci / P, h, e0.key);
        e0.p = point(cyl.zLo, h);
        e0.end = true;
        C.push_back(e0);
        const long k0 = long(std::floor(-ci / P)) - 1, k1 = long(std::ceil((H - ci) / P)) + 1;
        for (long k = k0; k <= k1; ++k)
            for (int j = 0; j < M; ++j) {
                const double z = (k + rows[j].u) * P + ci;
                if (z <= eps || z >= H - eps) continue;
                ColPoint q;
                q.key = double(k * M + j);
                q.p = point(cyl.zLo + z, rows[j].h);
                C.push_back(q);
            }
        ColPoint e1;
        at((H - ci) / P, h, e1.key);
        e1.p = point(cyl.zHi, h);
        e1.end = true;
        C.push_back(e1);
    }

    // Strips between neighbouring columns, zipped row by row.
    std::vector<std::array<std::pair<int, int>, 3>> tris;
    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> hard; // edges drawn
    std::vector<int> hardCurve; // curve class of each hard edge (-1: low end, -2: high end)
    const int shiftLast = -hand * S * M; // keys of column 0 seen as column N
    for (int i = 0; i < N; ++i) {
        const int j = (i + 1) % N;
        const auto& A = cols[i];
        const auto& B = cols[j];
        const double sh = j == 0 ? shiftLast : 0;
        auto kb = [&](size_t b) { return B[b].key + sh; };
        size_t a = 0, b = 0;
        hard.push_back({{i, 0}, {j, 0}});
        hardCurve.push_back(-1);
        while (a + 1 < A.size() || b + 1 < B.size()) {
            const bool advA = b + 1 >= B.size() || (a + 1 < A.size() && A[a + 1].key < kb(b + 1));
            const bool both = a + 1 < A.size() && b + 1 < B.size() && !A[a + 1].end && !B[b + 1].end &&
                              A[a + 1].key == kb(b + 1);
            if (both) {
                const Vec3 &pa = A[a].p, &pb = B[b].p, &pb1 = B[b + 1].p, &pa1 = A[a + 1].p;
                if ((pa - pb1).length() <= (pb - pa1).length()) {
                    tris.push_back({{{i, int(a)}, {j, int(b)}, {j, int(b + 1)}}});
                    tris.push_back({{{i, int(a)}, {j, int(b + 1)}, {i, int(a + 1)}}});
                } else {
                    tris.push_back({{{i, int(a)}, {j, int(b)}, {i, int(a + 1)}}});
                    tris.push_back({{{j, int(b)}, {j, int(b + 1)}, {i, int(a + 1)}}});
                }
                const long R = long(A[a + 1].key);
                const int row = int(((R % M) + M) % M);
                if (rows[row].hard) {
                    hard.push_back({{i, int(a + 1)}, {j, int(b + 1)}});
                    const long cls = S * M;
                    hardCurve.push_back(int(((R % cls) + cls) % cls));
                }
                ++a;
                ++b;
            } else if (advA) {
                tris.push_back({{{i, int(a)}, {j, int(b)}, {i, int(a + 1)}}});
                ++a;
            } else {
                tris.push_back({{{i, int(a)}, {j, int(b)}, {j, int(b + 1)}}});
                ++b;
            }
        }
        hard.push_back({{i, int(A.size() - 1)}, {j, int(B.size() - 1)}});
        hardCurve.push_back(-2);
    }
    if ((int)tris.size() > kMaxTriangles)
        return fail("The thread would have too many polygons: a larger pitch, fewer segments, or a shorter surface.");

    // ---- apply ------------------------------------------------------------------
    struct CapCopy {
        std::vector<Id> outer;
        std::vector<std::vector<Id>> holes;
        Vec3 normal;
        FaceSide front, back;
        bool hidden = false;
        int loop = -1, ring = 0;
    };
    std::vector<CapCopy> capCopies;
    for (const Cap& c : caps) {
        const Face& G = m.faces[c.face];
        capCopies.push_back({G.outer, G.holes, G.normal, G.front, G.back, G.hidden, c.loop, c.ring});
    }
    struct Look {
        FaceSide front, back;
        bool hidden = false;
    } look{m.faces[cyl.side[0]].front, m.faces[cyl.side[0]].back, m.faces[cyl.side[0]].hidden};
    std::set<Id> oldEdges;
    for (Id f : cyl.side)
        for (Id e : m.faceEdges(f)) oldEdges.insert(e);
    for (Id f : cyl.side) m.eraseFace(f);
    for (Id g : capFaces) m.eraseFace(g);
    for (Id e : oldEdges) m.eraseEdge(e, false);

    for (auto& C : cols)
        for (ColPoint& q : C) q.id = m.addVertexRaw(q.p);
    auto vid = [&](const std::pair<int, int>& c) { return cols[c.first][c.second].id; };
    ScrewThreadResult res;
    for (const auto& t : tris) {
        const Id a = vid(t[0]), b = vid(t[1]), c = vid(t[2]);
        if (a == b || b == c || a == c) continue;
        // The strips are zipped counter-clockwise in (angle, height), which
        // faces away from the axis; a hole's wall faces the other way. (The
        // radial direction would not do: a square thread's flanks face along
        // the axis.)
        const Vec3 hint = (m.pos(b) - m.pos(a)).cross(m.pos(c) - m.pos(a)) * double(cyl.sign);
        const Id f = m.addFaceRaw({a, b, c}, {}, hint);
        if (f == kNone) continue;
        // The thread takes the look of the surface it replaces.
        m.faces[f].front = look.front;
        m.faces[f].back = look.back;
        m.faces[f].hidden = look.hidden;
        m.faces[f].front.tex.valid = m.faces[f].back.tex.valid = false;
        res.faces.insert(f);
    }
    std::set<Id> newEdges;
    for (Id f : res.faces)
        for (Id e : m.faceEdges(f)) newEdges.insert(e);
    for (Id e : newEdges) {
        Edge& E = m.edges[e];
        E.soft = E.smooth = true;
        E.hidden = false;
        E.curve = kNone;
    }
    std::map<int, Id> curves;
    for (size_t k = 0; k < hard.size(); ++k) {
        const Id e = m.findEdge(vid(hard[k].first), vid(hard[k].second));
        if (e == kNone) continue;
        Edge& E = m.edges[e];
        E.soft = E.smooth = false;
        auto it = curves.find(hardCurve[k]);
        if (it == curves.end()) it = curves.emplace(hardCurve[k], m.newCurveId()).first;
        E.curve = it->second;
    }
    // The ends, reshaped to meet the thread.
    for (const CapCopy& c : capCopies) {
        std::vector<Id> ring;
        for (int i = 0; i < N; ++i) ring.push_back(c.ring == 0 ? cols[i].front().id : cols[i].back().id);
        std::vector<Id> outer = c.outer;
        std::vector<std::vector<Id>> holes = c.holes;
        if (c.loop < 0) outer = ring;
        else holes[c.loop] = ring;
        const Id g = m.addFaceRaw(outer, holes, c.normal);
        if (g == kNone) continue;
        m.faces[g].front = c.front;
        m.faces[g].back = c.back;
        m.faces[g].hidden = c.hidden;
    }
    m.removeOrphanVertices();
    m.touch();
    m.markGeometryMoved();
    res.radius = cyl.r;
    res.length = H;
    res.turns = H / lead;
    if (out) *out = std::move(res);
    return true;
}

} // namespace gs
