// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageTrace.h"

#include <unordered_map>

namespace gs {

namespace {

// Douglas-Peucker on a closed loop; returns the kept indices in order.
std::vector<int> simplifyIndices(const std::vector<Vec2>& L, double tol) {
    const int n = (int)L.size();
    std::vector<int> all(n);
    for (int i = 0; i < n; ++i) all[i] = i;
    if (n < 4 || tol <= 0) return all;
    int far = 0;
    double best = -1;
    for (int i = 1; i < n; ++i) {
        double d = (L[i] - L[0]).length();
        if (d > best) { best = d; far = i; }
    }
    std::vector<bool> keep(n, false);
    keep[0] = keep[far] = true;
    std::vector<std::pair<int, int>> stack = {{0, far}, {far, n}};
    while (!stack.empty()) {
        auto [a, b] = stack.back();
        stack.pop_back();
        if (b - a < 2) continue;
        Vec2 A = L[a], B = L[b % n], AB = B - A;
        double len = AB.length();
        int idx = -1;
        double dmax = tol;
        for (int i = a + 1; i < b; ++i) {
            Vec2 AP = L[i] - A;
            double d = len > 1e-12 ? std::fabs(AB.cross(AP)) / len : AP.length();
            if (d > dmax) { dmax = d; idx = i; }
        }
        if (idx < 0) continue;
        keep[idx] = true;
        stack.push_back({a, idx});
        stack.push_back({idx, b});
    }
    std::vector<int> r;
    for (int i = 0; i < n; ++i)
        if (keep[i]) r.push_back(i);
    return r;
}

// Least-squares line through the loop points from index `from` forward to
// `to`, ignoring points within `skip` of either end (marching squares bevels
// sharp corners by about half a pixel).
bool fitLine(const std::vector<Vec2>& L, int from, int to, double skip, Vec2& c, Vec2& d) {
    const int n = (int)L.size();
    std::vector<Vec2> pts;
    for (int i = from;; i = (i + 1) % n) {
        if ((L[i] - L[from]).length() > skip && (L[i] - L[to]).length() > skip) pts.push_back(L[i]);
        if (i == to) break;
    }
    if (pts.size() < 2) return false;
    c = Vec2();
    for (const Vec2& p : pts) c = c + p;
    c = c * (1.0 / pts.size());
    double sxx = 0, sxy = 0, syy = 0;
    for (const Vec2& p : pts) {
        Vec2 q = p - c;
        sxx += q.x * q.x;
        sxy += q.x * q.y;
        syy += q.y * q.y;
    }
    double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
    d = Vec2(std::cos(ang), std::sin(ang));
    return true;
}

// Simplifies a contour and restores sharp corners: a vertex where the outline
// turns sharply moves to the intersection of lines fitted to its two sides.
std::vector<Vec2> simplifyLoop(const std::vector<Vec2>& L, double tol) {
    std::vector<int> k = simplifyIndices(L, tol);
    const int m = (int)k.size();
    std::vector<Vec2> r;
    for (int i = 0; i < m; ++i) {
        Vec2 p = L[k[i]];
        if (tol > 0 && m >= 3) {
            Vec2 c1, d1, c2, d2;
            if (fitLine(L, k[(i + m - 1) % m], k[i], 1.5, c1, d1) && fitLine(L, k[i], k[(i + 1) % m], 1.5, c2, d2)) {
                double s = d1.cross(d2);
                if (std::fabs(s) > std::sin(60.0 * kPi / 180)) { // turns by 60..120 degrees
                    double t = (c2 - c1).cross(d2) / s;
                    Vec2 x = c1 + d1 * t;
                    if ((x - p).length() < 1.5) p = x;
                }
            }
        }
        r.push_back(p);
    }
    return r;
}

double loopArea(const std::vector<Vec2>& L) {
    return signedArea2D((int)L.size(), [&](int i) { return L[i]; });
}

bool insideAny(const std::vector<std::vector<Vec2>>& loops, const Vec2& p) {
    int count = 0;
    for (const auto& L : loops)
        if (pointInPolygon2D((int)L.size(), [&](int i) { return L[i]; }, p)) ++count;
    return count % 2 == 1;
}

} // namespace

std::vector<std::vector<Vec2>> traceContours(const ScalarImage& img, const TraceOptions& opt) {
    const int W = img.width, H = img.height;
    if (W <= 0 || H <= 0 || (int)img.values.size() < W * H) return {};
    const double thr = opt.threshold;
    // Grid of pixel centres with a ring of "outside" samples around the image.
    const int GW = W + 2, GH = H + 2;
    const float outside = (float)std::min(0.0, thr - 1.0);
    auto G = [&](int i, int j) -> float {
        if (i < 1 || j < 1 || i > W || j > H) return outside;
        return img.at(i - 1, j - 1);
    };
    auto in = [&](int i, int j) { return G(i, j) > thr; };
    auto gpos = [](int i, int j) { return Vec2(i - 0.5, j - 0.5); };

    // Crossing points, keyed by grid edge: horizontal (i,j)-(i+1,j) and
    // vertical (i,j)-(i,j+1).
    std::unordered_map<int64_t, Vec2> point;
    auto hId = [&](int i, int j) { return (int64_t(j) * GW + i) * 2; };
    auto vId = [&](int i, int j) { return (int64_t(j) * GW + i) * 2 + 1; };
    auto cross = [&](int64_t id, int i0, int j0, int i1, int j1) {
        if (point.count(id)) return id;
        float a = G(i0, j0), b = G(i1, j1);
        auto pad = [&](int i, int j) { return i < 1 || j < 1 || i > W || j > H; };
        // Against the outside ring the outline runs exactly along the image border.
        double t = pad(i0, j0) || pad(i1, j1) ? 0.5
                   : std::fabs(b - a) > 1e-12 ? std::clamp((thr - a) / (b - a), 0.0, 1.0)
                                              : 0.5;
        Vec2 p = gpos(i0, j0) + (gpos(i1, j1) - gpos(i0, j0)) * t;
        p.x = std::clamp(p.x, 0.0, double(W));
        p.y = std::clamp(p.y, 0.0, double(H));
        point[id] = p;
        return id;
    };

    std::vector<std::pair<int64_t, int64_t>> segs;
    for (int j = 0; j < GH - 1; ++j)
        for (int i = 0; i < GW - 1; ++i) {
            int c = (in(i, j) ? 8 : 0) | (in(i + 1, j) ? 4 : 0) | (in(i + 1, j + 1) ? 2 : 0) | (in(i, j + 1) ? 1 : 0);
            if (c == 0 || c == 15) continue;
            auto T = [&] { return cross(hId(i, j), i, j, i + 1, j); };
            auto B = [&] { return cross(hId(i, j + 1), i, j + 1, i + 1, j + 1); };
            auto L = [&] { return cross(vId(i, j), i, j, i, j + 1); };
            auto R = [&] { return cross(vId(i + 1, j), i + 1, j, i + 1, j + 1); };
            bool centre = (G(i, j) + G(i + 1, j) + G(i + 1, j + 1) + G(i, j + 1)) * 0.25 > thr;
            switch (c) {
            case 1: case 14: segs.emplace_back(L(), B()); break;
            case 2: case 13: segs.emplace_back(B(), R()); break;
            case 3: case 12: segs.emplace_back(L(), R()); break;
            case 4: case 11: segs.emplace_back(T(), R()); break;
            case 6: case 9: segs.emplace_back(T(), B()); break;
            case 7: case 8: segs.emplace_back(T(), L()); break;
            case 5:
                if (centre) { segs.emplace_back(T(), L()); segs.emplace_back(B(), R()); }
                else { segs.emplace_back(T(), R()); segs.emplace_back(L(), B()); }
                break;
            case 10:
                if (centre) { segs.emplace_back(T(), R()); segs.emplace_back(L(), B()); }
                else { segs.emplace_back(T(), L()); segs.emplace_back(B(), R()); }
                break;
            }
        }

    // Link segments into loops: every crossing point joins exactly two segments.
    std::unordered_map<int64_t, std::vector<int>> at;
    for (int s = 0; s < (int)segs.size(); ++s) {
        at[segs[s].first].push_back(s);
        at[segs[s].second].push_back(s);
    }
    std::vector<bool> used(segs.size(), false);
    std::vector<std::vector<Vec2>> loops;
    for (int s0 = 0; s0 < (int)segs.size(); ++s0) {
        if (used[s0]) continue;
        std::vector<Vec2> loop;
        int s = s0;
        int64_t start = segs[s0].first, cur = segs[s0].second;
        loop.push_back(point[start]);
        used[s] = true;
        while (cur != start) {
            loop.push_back(point[cur]);
            int next = -1;
            for (int t : at[cur])
                if (!used[t]) { next = t; break; }
            if (next < 0) break; // open chain (cannot happen with the padded border)
            used[next] = true;
            cur = segs[next].first == cur ? segs[next].second : segs[next].first;
            s = next;
        }
        // Drop repeated points (crossings that landed on the same spot).
        std::vector<Vec2> clean;
        for (const Vec2& p : loop)
            if (clean.empty() || (p - clean.back()).length() > 1e-9) clean.push_back(p);
        while (clean.size() > 1 && (clean.front() - clean.back()).length() <= 1e-9) clean.pop_back();
        clean = simplifyLoop(clean, opt.simplify);
        if (clean.size() >= 3 && std::fabs(loopArea(clean)) >= opt.minArea) loops.push_back(std::move(clean));
    }
    return loops;
}

TexMap effectiveTexMap(const Face& f, bool frontSide, const Material* m) {
    const FaceSide& s = frontSide ? f.front : f.back;
    if (s.tex.valid) return s.tex;
    Vec3 n = frontSide ? f.normal : -f.normal;
    Vec3 u, v;
    planeBasis(n, u, v);
    double w = 1, h = 1;
    if (m && m->hasTexture()) {
        w = m->texture.worldWidth > 1e-9 ? m->texture.worldWidth : 1;
        h = m->texture.worldHeight > 1e-9 ? m->texture.worldHeight : 1;
    }
    TexMap t;
    t.valid = true;
    t.origin = Vec3();
    t.u = u / w;
    t.v = v / h;
    return t;
}

bool traceImageOnFace(Mesh& m, Id f, const TexMap& map, const ScalarImage& img, const TraceOptions& opt,
                      std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) return fail("Select a face that shows an image.");
    std::vector<std::vector<Vec2>> loops = traceContours(img, opt);
    if (loops.empty()) return fail("Nothing to trace at this threshold.");

    // uv -> plane point: p = origin + U * u + V * v with U, V dual to the map axes.
    const Vec3 &a = map.u, &b = map.v;
    double aa = a.dot(a), ab = a.dot(b), bb = b.dot(b), det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-18) return fail("The face's texture mapping is degenerate.");
    const Vec3 U = (a * bb - b * ab) / det, V = (b * aa - a * ab) / det;
    const double W = img.width, H = img.height;
    auto toWorld = [&](const Vec2& p) { return map.origin + U * (p.x / W) + V * (1.0 - p.y / H); };
    auto toPixel = [&](const Vec3& p) {
        Vec3 d = p - map.origin;
        return Vec2(d.dot(a) * W, (1.0 - d.dot(b)) * H);
    };

    const Plane P = m.facePlane(f);
    std::vector<std::pair<Vec3, Vec3>> segs;
    for (const auto& L : loops) {
        bool onFace = true;
        for (const Vec2& p : L)
            if (!m.pointInFace(f, toWorld(p), true)) { onFace = false; break; }
        if (!onFace) continue;
        for (size_t i = 0; i < L.size(); ++i) segs.emplace_back(toWorld(L[i]), toWorld(L[(i + 1) % L.size()]));
    }
    if (segs.empty()) return fail("The traced outline does not lie on the face; fit the image to the face first.");

    m.drawSegments(segs);

    if (opt.removeBackground) {
        std::vector<Id> loose;
        for (Id g = 0; g < (Id)m.faces.size(); ++g) {
            const Face& F = m.faces[g];
            if (!F.alive || std::fabs(F.normal.dot(P.n)) < 1 - 1e-6) continue;
            bool coplanar = true;
            for (Id v : m.faceVertices(g))
                if (!P.contains(m.pos(v))) { coplanar = false; break; }
            if (!coplanar) continue;
            Vec2 px = toPixel(m.faceInteriorPoint(g));
            if (px.x < 0 || px.y < 0 || px.x > W || px.y > H) continue; // outside the image
            if (insideAny(loops, px)) continue;
            for (Id e : m.faceEdges(g)) loose.push_back(e);
            m.eraseFace(g);
        }
        for (Id e : loose) {
            if (!m.edges[e].alive) continue;
            bool used = false;
            for (Id g : m.edgeFaces(e)) used = used || m.faces[g].alive;
            if (!used) m.eraseEdge(e, false);
        }
    }
    m.touch();
    return true;
}

} // namespace gs
