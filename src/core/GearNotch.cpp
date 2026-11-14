// SPDX-License-Identifier: GPL-3.0-or-later
#include "GearNotch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace gs {

namespace {

constexpr int kMaxTeeth = 5000;

// One cell of the pattern (u along it 0..1, h up 0..1), the tooth centred.
std::vector<std::pair<double, double>> profilePoints(const GearNotchParams& p) {
    const double w = std::clamp(p.toothWidth, 0.02, 1.0);
    const double bw = w / 2, tw = bw * std::clamp(p.tipWidth, 0.0, 1.0);
    const int seg = std::clamp(p.curveSegments, 2, 64);
    std::vector<std::pair<double, double>> k = {{0, 0}, {0.5 - bw, 0}};
    switch (p.profile) {
    case GearNotchParams::Square:
        k.insert(k.end(), {{0.5 - bw, 1}, {0.5 + bw, 1}});
        break;
    case GearNotchParams::Trapezoid:
        k.insert(k.end(), {{0.5 - tw, 1}, {0.5 + tw, 1}});
        break;
    case GearNotchParams::Triangle:
        k.push_back({0.5, 1});
        break;
    case GearNotchParams::Sawtooth:
        k.push_back({0.5 + bw, 1});
        break;
    case GearNotchParams::Rounded:
        for (int i = 1; i < seg; ++i) {
            const double a = kPi * (1 - double(i) / seg);
            k.push_back({0.5 + bw * std::cos(a), std::sin(a)});
        }
        break;
    case GearNotchParams::Gear: {
        // Flanks bulging outwards like an involute tooth's, narrowing to the tip.
        auto half = [&](double h) { return bw - (bw - tw) * h * h; };
        for (int i = 1; i <= seg; ++i) {
            const double h = double(i) / seg;
            k.push_back({0.5 - half(h), h});
        }
        for (int i = seg; i >= 1; --i) {
            const double h = double(i) / seg;
            k.push_back({0.5 + half(h), h});
        }
        break;
    }
    }
    k.insert(k.end(), {{0.5 + bw, 0}, {1, 0}});
    return k;
}

Vec3 newellNormal(const std::vector<Vec3>& pts) {
    Vec3 n;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec3& a = pts[i];
        const Vec3& b = pts[(i + 1) % pts.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

// No two segments that are not neighbours touch (2D in the plane).
bool simplePath(const std::vector<Vec3>& pts, bool closed, const Vec3& normal) {
    const size_t n = pts.size();
    const size_t segs = closed ? n : n - 1;
    if (n < 2) return true;
    const Vec3 N = normal.normalized();
    Vec3 u = std::fabs(N.x) < 0.9 ? Vec3(1, 0, 0).cross(N) : Vec3(0, 1, 0).cross(N);
    u = u.normalized();
    const Vec3 v = N.cross(u);
    std::vector<std::pair<double, double>> q(n);
    double total = 0;
    for (size_t i = 0; i < n; ++i) q[i] = {pts[i].dot(u), pts[i].dot(v)};
    for (size_t i = 0; i < segs; ++i) total += (pts[(i + 1) % n] - pts[i]).length();
    const double cell = std::max(1e-9, 2 * total / std::max<size_t>(1, segs));
    // Segments by grid cell.
    std::unordered_map<long long, std::vector<size_t>> grid;
    auto key = [](long long x, long long y) { return (x * 73856093LL) ^ (y * 19349663LL); };
    for (size_t i = 0; i < segs; ++i) {
        const auto& a = q[i];
        const auto& b = q[(i + 1) % n];
        const long long x0 = (long long)std::floor(std::min(a.first, b.first) / cell), x1 = (long long)std::floor(std::max(a.first, b.first) / cell);
        const long long y0 = (long long)std::floor(std::min(a.second, b.second) / cell), y1 = (long long)std::floor(std::max(a.second, b.second) / cell);
        for (long long x = x0; x <= x1; ++x)
            for (long long y = y0; y <= y1; ++y) grid[key(x, y)].push_back(i);
    }
    auto cross = [](std::pair<double, double> o, std::pair<double, double> a, std::pair<double, double> b) {
        return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first);
    };
    const double eps = 1e-12 * std::max(1.0, total * total);
    for (auto& [k, list] : grid) {
        for (size_t x = 0; x < list.size(); ++x)
            for (size_t y = x + 1; y < list.size(); ++y) {
                size_t i = list[x], j = list[y];
                if (i == j) continue;
                const size_t d = i > j ? i - j : j - i;
                if (d == 1 || (closed && d == segs - 1)) continue; // neighbours share a point
                const auto &a = q[i], &b = q[(i + 1) % n], &c = q[j], &e = q[(j + 1) % n];
                const double d1 = cross(c, e, a), d2 = cross(c, e, b), d3 = cross(a, b, c), d4 = cross(a, b, e);
                if (((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps)))
                    return false;
                // Touching (a point on the other segment) counts too.
                auto onSeg = [&](std::pair<double, double> p, std::pair<double, double> s0, std::pair<double, double> s1, double cr) {
                    return std::fabs(cr) <= eps && p.first >= std::min(s0.first, s1.first) - 1e-12 && p.first <= std::max(s0.first, s1.first) + 1e-12 &&
                           p.second >= std::min(s0.second, s1.second) - 1e-12 && p.second <= std::max(s0.second, s1.second) + 1e-12;
                };
                if (onSeg(a, c, e, d1) || onSeg(b, c, e, d2) || onSeg(c, a, b, d3) || onSeg(e, a, b, d4)) return false;
            }
    }
    return true;
}

} // namespace

std::vector<NotchPoint> notchPath(const std::vector<Vec3>& pts, bool closed, const std::vector<Vec3>& side,
                                  const GearNotchParams& p, int* teeth) {
    std::vector<NotchPoint> out;
    if (teeth) *teeth = 0;
    const int n = (int)pts.size();
    const int M = closed ? n : n - 1; // segments
    if (n < 2 || M < 1 || (int)side.size() < M) return out;
    std::vector<double> s(n + 1, 0.0);
    std::vector<Vec3> t(M);
    for (int i = 0; i < M; ++i) {
        const Vec3 d = pts[(i + 1) % n] - pts[i];
        s[i + 1] = s[i] + d.length();
        t[i] = d.normalized();
    }
    const double L = s[M];
    if (L < kTol) return out;

    // Corners: where the path turns sharper than the corner angle.
    const double cosCorner = std::cos(std::clamp(p.cornerAngle, 0.0, 180.0) * kPi / 180);
    std::vector<bool> corner(n, false);
    for (int j = 0; j < n; ++j) {
        if (!closed && (j == 0 || j == n - 1)) continue;
        const int in = (j - 1 + M) % M, outSeg = j % M;
        corner[j] = t[in].dot(t[outSeg]) < cosCorner - 1e-12;
    }
    // Direction teeth grow at each end of each segment: averaged across smooth
    // vertices, the segment's own at corners and path ends.
    auto endSide = [&](int seg, bool atEnd) {
        const int v = atEnd ? (seg + 1) % n : seg;
        const bool end = !closed && (v == 0 || v == n - 1);
        if (end || corner[v]) return side[seg];
        const int other = atEnd ? (seg + 1) % M : (seg - 1 + M) % M;
        return (side[seg] + side[other]).normalized();
    };
    const double dir = p.inward ? -1.0 : 1.0;
    auto at = [&](double sv, double h) {
        if (closed) {
            sv = std::fmod(sv, L);
            if (sv < 0) sv += L;
        }
        sv = std::clamp(sv, 0.0, L);
        int i = int(std::upper_bound(s.begin(), s.begin() + M + 1, sv) - s.begin()) - 1;
        i = std::clamp(i, 0, M - 1);
        const double len = s[i + 1] - s[i];
        const double f = len > 0 ? (sv - s[i]) / len : 0;
        const Vec3 base = pts[i] + (pts[(i + 1) % n] - pts[i]) * f;
        if (h == 0) return base;
        const Vec3 nrm = (endSide(i, false) * (1 - f) + endSide(i, true) * f).normalized();
        return base + nrm * (h * p.depth * dir);
    };

    // Runs between corners, each with a whole number of teeth.
    std::vector<std::pair<double, double>> runs;
    std::vector<double> cuts;
    for (int j = 0; j < n; ++j)
        if (corner[j]) cuts.push_back(s[j]);
    if (!closed) {
        double a = 0;
        for (double c : cuts) {
            runs.push_back({a, c});
            a = c;
        }
        runs.push_back({a, L});
    } else if (cuts.empty()) {
        runs.push_back({0, L});
    } else {
        for (size_t i = 0; i < cuts.size(); ++i) runs.push_back({cuts[i], i + 1 < cuts.size() ? cuts[i + 1] : cuts[0] + L});
    }
    const double pitch = p.byCount ? L / std::max(1, p.count) : std::max(1e-6, p.pitch);
    std::vector<int> per;
    long long total = 0;
    for (const auto& [a, b] : runs) {
        const int k = std::max(1, (int)std::lround((b - a) / pitch));
        per.push_back(k);
        total += k;
    }
    if (teeth) *teeth = (int)std::min<long long>(total, 1 << 30);
    if (total > kMaxTeeth) return out;

    // Key points of the teeth (s along the path, h up).
    const auto prof = profilePoints(p);
    const bool shift = closed && cuts.empty();
    std::vector<std::pair<double, double>> keys;
    for (size_t r = 0; r < runs.size(); ++r) {
        const double a = runs[r].first, c = (runs[r].second - a) / per[r];
        const double off = shift ? std::fmod(std::fmod(p.phase, 1.0) + 1.0, 1.0) * c : 0;
        for (int k = 0; k < per[r]; ++k)
            for (const auto& [u, h] : prof) keys.push_back({a + off + (k + u) * c, h});
    }

    // Path vertices along the stretches at one height (the root between teeth,
    // flat tips) keep the path's shape there, a circle stays round.
    auto emit = [&](double sv, double h) {
        const Vec3 q = at(sv, h);
        if (!out.empty() && (out.back().p - q).length() < 1e-9) return;
        double sn = sv;
        if (closed) {
            sn = std::fmod(sv, L);
            if (sn < 0) sn += L;
            if (L - sn < 1e-12) sn = 0;
        }
        out.push_back({q, sn});
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        emit(keys[i].first, keys[i].second);
        if (i + 1 == keys.size()) break;
        const auto [s1, h1] = keys[i];
        const auto [s2, h2] = keys[i + 1];
        if (std::fabs(h1 - h2) > 1e-12 || s2 - s1 < 1e-12) continue;
        // Path vertices strictly between (also past the start of a closed path).
        for (int lap = 0; lap < (closed ? 2 : 1); ++lap)
            for (int j = 0; j < (closed ? n : n); ++j) {
                const double sj = s[j] + lap * L;
                if (sj > s1 + 1e-12 && sj < s2 - 1e-12) emit(sj, h1);
            }
    }
    // A closed path ends where it began.
    if (closed && out.size() > 1 && (out.back().p - out.front().p).length() < 1e-9) out.pop_back();
    // Points keep path order by s (a closed path may start part way round).
    if (closed) {
        auto first = std::min_element(out.begin(), out.end(), [](const NotchPoint& a, const NotchPoint& b) { return a.s < b.s; });
        std::rotate(out.begin(), first, out.end());
    }
    return out;
}

bool gearNotch(Mesh& m, const std::set<Id>& faces, const std::set<Id>& edgesIn, const GearNotchParams& p,
               GearNotchResult* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (p.depth <= 0) return fail("The tooth depth must be more than 0.");

    // ---- the edges to put teeth on ---------------------------------------------
    std::set<Id> E;
    for (Id e : edgesIn)
        if (e >= 0 && e < (Id)m.edges.size() && m.edges[e].alive) E.insert(e);
    for (Id f : faces) {
        if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) continue;
        const Face& F = m.faces[f];
        std::vector<const std::vector<Id>*> loops = {&F.outer};
        if (p.holes)
            for (const auto& h : F.holes) loops.push_back(&h);
        for (const auto* L : loops)
            for (size_t i = 0; i < L->size(); ++i) {
                Id e = m.findEdge((*L)[i], (*L)[(i + 1) % L->size()]);
                if (e != kNone) E.insert(e);
            }
    }
    if (E.empty()) return fail("Select faces, curves or lines to put teeth on.");

    // ---- chained into paths ------------------------------------------------------
    std::map<Id, std::vector<Id>> at;
    for (Id e : E)
        for (Id v : m.edges[e].v) at[v].push_back(e);
    struct Path {
        std::vector<Id> verts;
        bool closed = false;
    };
    std::vector<Path> paths;
    std::set<Id> used;
    auto walk = [&](Id start, Id firstEdge) {
        Path P;
        P.verts.push_back(start);
        Id v = start, e = firstEdge;
        while (true) {
            used.insert(e);
            v = m.edges[e].other(v);
            if (v == start) {
                P.closed = true;
                break;
            }
            P.verts.push_back(v);
            if (at[v].size() != 2) break;
            Id next = at[v][0] == e ? at[v][1] : at[v][0];
            if (used.count(next)) break;
            e = next;
        }
        paths.push_back(std::move(P));
    };
    for (const auto& [v, es] : at)
        if (es.size() != 2)
            for (Id e : es)
                if (!used.count(e)) walk(v, e);
    for (Id e : E)
        if (!used.count(e)) walk(m.edges[e].v[0], e);

    // ---- each path's teeth -------------------------------------------------------
    struct Plan {
        std::vector<NotchPoint> pts;
        std::vector<Vec3> outline; // all points in order (open: from the first vertex)
        bool closed = false;
        bool loose = false;
    };
    std::vector<Plan> plans;
    // Directed path edge (a, b) -> the new points from just after a up to b.
    std::map<std::pair<Id, Id>, std::vector<Vec3>> repl;
    std::set<Id> rebuild;
    int teethTotal = 0;
    for (const Path& P : paths) {
        const int n = (int)P.verts.size();
        const int M = P.closed ? n : n - 1;
        if (n < 2 || (P.closed && n < 3)) continue;
        std::vector<Vec3> pts;
        for (Id v : P.verts) pts.push_back(m.pos(v));
        std::vector<Id> pathEdges;
        std::set<Id> bordering;
        for (int i = 0; i < M; ++i) {
            const Id e = m.findEdge(P.verts[i], P.verts[(i + 1) % n]);
            pathEdges.push_back(e);
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive) bordering.insert(f);
        }
        // The plane, and which side the teeth go.
        Vec3 N;
        double sign = 1;
        bool loose = bordering.empty();
        if (!loose) {
            const Id f0 = *bordering.begin();
            N = m.faces[f0].normal;
            for (Id f : bordering) {
                const Plane pl = m.facePlane(f);
                const bool flat = std::fabs(m.faces[f].normal.dot(N)) > 1 - 1e-6;
                for (const Vec3& q : pts)
                    if (!flat || !pl.contains(q, kTol * 10))
                        return fail("Gear Notches work on flat outlines: a selected edge is the corner of a solid. "
                                    "Put the teeth on the flat shape first, then push/pull it.");
            }
            // Away from the face the path bounds.
            const Vec3 a = pts[0], b = pts[1 % n];
            const Vec3 t = (b - a).normalized();
            const Vec3 d = t.cross(N).normalized();
            const Vec3 mid = (a + b) * 0.5;
            const double eps = std::max(kTol * 20, std::min(1e-3, (b - a).length() * 1e-3));
            int faceSides = 0;
            for (Id f : m.edgeFaces(pathEdges[0])) faceSides += m.faces[f].alive ? 1 : 0;
            if (faceSides == 1) sign = m.pointInFace(*std::find_if(m.edgeFaces(pathEdges[0]).begin(), m.edgeFaces(pathEdges[0]).end(),
                                                                   [&](Id f) { return m.faces[f].alive; }),
                                                     mid + d * eps, false)
                                           ? -1
                                           : 1;
            else sign = p.flip ? -1 : 1;
        } else {
            N = newellNormal(pts);
            if (N.length() < 1e-12) {
                const Vec3 t = (pts.back() - pts.front()).normalized();
                N = std::fabs(t.z) < 0.99 ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
            }
            N = N.normalized();
            for (const Vec3& q : pts)
                if (std::fabs((q - pts[0]).dot(N)) > kTol * 10) return fail("A selected line is not flat: teeth need a flat path.");
            if (p.flip) sign = -1;
        }
        std::vector<Vec3> side(M);
        for (int i = 0; i < M; ++i) side[i] = (pts[(i + 1) % n] - pts[i]).cross(N).normalized() * sign;
        int teeth = 0;
        Plan plan;
        plan.pts = notchPath(pts, P.closed, side, p, &teeth);
        teethTotal += teeth;
        if (teethTotal > kMaxTeeth) return fail("Too many teeth (more than 5000): fewer teeth or a larger spacing.");
        if (plan.pts.size() < 2) continue;
        plan.closed = P.closed;
        plan.loose = loose;
        for (const NotchPoint& q : plan.pts) plan.outline.push_back(q.p);
        if (!simplePath(plan.outline, P.closed, N)) return fail("The teeth overlap: less depth, fewer or narrower teeth.");

        // New points per path segment.
        std::vector<double> s(n + 1, 0.0);
        for (int i = 0; i < M; ++i) s[i + 1] = s[i] + (pts[(i + 1) % n] - pts[i]).length();
        const double L = s[M];
        // (s along the path, point), sorted along the path per segment.
        std::vector<std::vector<std::pair<double, Vec3>>> fwd(M), bwd(M);
        for (const NotchPoint& q : plan.pts) {
            // forward: s_i < s <= s_i+1 (closed: s = 0 is the end, L)
            double sf = q.s;
            if (P.closed && sf <= 1e-12) sf = L;
            if (sf > 1e-12) {
                int i = int(std::lower_bound(s.begin(), s.begin() + M + 1, sf - 1e-12) - s.begin()) - 1;
                fwd[std::clamp(i, 0, M - 1)].push_back({sf, q.p});
            }
            // backward: s_i <= s < s_i+1
            if (q.s < L - 1e-12) {
                int i = int(std::upper_bound(s.begin(), s.begin() + M + 1, q.s + 1e-12) - s.begin()) - 1;
                bwd[std::clamp(i, 0, M - 1)].push_back({q.s, q.p});
            }
        }
        auto bySpot = [](const std::pair<double, Vec3>& a, const std::pair<double, Vec3>& b) { return a.first < b.first; };
        for (int i = 0; i < M; ++i) {
            std::stable_sort(fwd[i].begin(), fwd[i].end(), bySpot);
            std::stable_sort(bwd[i].begin(), bwd[i].end(), bySpot);
        }
        for (int i = 0; i < M; ++i) {
            const Id a = P.verts[i], b = P.verts[(i + 1) % n];
            std::vector<Vec3>& f = repl[{a, b}];
            for (const auto& x : fwd[i]) f.push_back(x.second);
            std::vector<Vec3>& r = repl[{b, a}];
            for (auto it = bwd[i].rbegin(); it != bwd[i].rend(); ++it) r.push_back(it->second);
        }
        rebuild.insert(bordering.begin(), bordering.end());
        plans.push_back(std::move(plan));
    }
    if (plans.empty()) return fail("Nothing to put teeth on: select faces, curves or lines.");

    // ---- faces along the paths, rebuilt with the teeth --------------------------
    struct NewFace {
        std::vector<std::vector<Vec3>> loops;
        Vec3 hint;
        FaceSide front, back;
        bool hidden = false;
    };
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
        for (size_t li = 0; li < loops.size(); ++li) {
            const auto& L = *loops[li];
            std::vector<Vec3> q;
            for (size_t i = 0; i < L.size(); ++i) {
                const Id v = L[i], w = L[(i + 1) % L.size()];
                auto it = repl.find({v, w});
                if (it != repl.end()) {
                    for (const Vec3& x : it->second)
                        if (q.empty() || (q.back() - x).length() > 1e-9) q.push_back(x);
                } else if (q.empty() || (q.back() - m.pos(w)).length() > 1e-9) {
                    q.push_back(m.pos(w));
                }
            }
            while (q.size() > 1 && (q.back() - q.front()).length() <= 1e-9) q.pop_back();
            if (q.size() < 3 || !simplePath(q, true, F.normal)) return fail("The teeth overlap: less depth, fewer or narrower teeth.");
            if (li == 0 && newellNormal(q).dot(F.normal) <= 0) return fail("The notches are deeper than the shape: less depth.");
            nf.loops.push_back(std::move(q));
        }
        rebuilt.push_back(std::move(nf));
    }

    // ---- apply ------------------------------------------------------------------
    std::set<Id> pathEdges;
    for (const auto& [ab, pts] : repl) {
        Id e = m.findEdge(ab.first, ab.second);
        if (e != kNone) pathEdges.insert(e);
    }
    for (Id f : rebuild) m.eraseFace(f);
    for (Id e : pathEdges) m.eraseEdge(e, false);
    GearNotchResult res;
    for (const NewFace& nf : rebuilt) {
        std::vector<std::vector<Id>> ids;
        for (const auto& L : nf.loops) {
            std::vector<Id> l;
            for (const Vec3& q : L) {
                const Id v = m.addVertex(q);
                if (l.empty() || l.back() != v) l.push_back(v);
            }
            while (l.size() > 1 && l.front() == l.back()) l.pop_back();
            ids.push_back(std::move(l));
        }
        if (ids.empty() || ids[0].size() < 3) continue;
        const std::vector<std::vector<Id>> holes(ids.begin() + 1, ids.end());
        const Id f = m.addFaceRaw(ids[0], holes, nf.hint);
        if (f == kNone) continue;
        m.faces[f].front = nf.front;
        m.faces[f].back = nf.back;
        m.faces[f].hidden = nf.hidden;
        res.faces.insert(f);
    }
    for (const Plan& pl : plans) {
        // The path's own edges (lines bounding no face come only from here).
        // Stretches bending gently (curved flanks, rounded tips, the circle
        // between teeth) are one curve each, so push/pull makes smooth walls
        // there; corners of the teeth stay sharp.
        const size_t k = pl.outline.size();
        const size_t segs = pl.closed ? k : k - 1;
        const double cosSmooth = std::cos(35.0 * kPi / 180);
        Id curve = kNone;
        for (size_t i = 0; i < segs; ++i) {
            const Vec3 a = pl.outline[i], b = pl.outline[(i + 1) % k];
            bool smooth = false;
            if (i > 0) {
                const Vec3 before = (a - pl.outline[i - 1]).normalized(), here = (b - a).normalized();
                smooth = before.dot(here) > cosSmooth;
            }
            if (!smooth) curve = kNone;
            const Id va = m.addVertex(a), vb = m.addVertex(b);
            if (va == vb) continue;
            const Id e = m.addEdgeRaw(va, vb);
            if (e == kNone) continue;
            if (smooth) {
                if (curve == kNone) {
                    curve = m.newCurveId();
                    // The segment before starts the curve.
                    const Id prev = m.findEdge(m.addVertex(pl.outline[i - 1]), va);
                    if (prev != kNone) m.edges[prev].curve = curve;
                }
                m.edges[e].curve = curve;
            } else {
                m.edges[e].curve = kNone;
            }
            res.edges.insert(e);
        }
    }
    m.removeOrphanVertices();
    m.touch();
    m.markGeometryMoved();
    res.paths = (int)plans.size();
    res.teeth = teethTotal;
    if (out) *out = std::move(res);
    return true;
}

} // namespace gs
