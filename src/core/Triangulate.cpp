// SPDX-License-Identifier: GPL-3.0-or-later
#include "Triangulate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gs {

namespace {

struct Ctx {
    std::vector<Vec2> pts;
    double eps = 1e-18; // area epsilon, scaled to polygon size

    double cross(int a, int b, int c) const { return (pts[b] - pts[a]).cross(pts[c] - pts[a]); }
    bool same(int a, int b) const {
        Vec2 d = pts[a] - pts[b];
        return std::fabs(d.x) <= 1e-12 && std::fabs(d.y) <= 1e-12;
    }
    // Inclusive point-in-triangle test for CCW triangle (a,b,c).
    bool inTri(int a, int b, int c, int p) const {
        return cross(a, b, p) >= -eps && cross(b, c, p) >= -eps && cross(c, a, p) >= -eps;
    }
};

double loopArea(const Ctx& c, const std::vector<int>& loop) {
    return signedArea2D((int)loop.size(), [&](int i) { return c.pts[loop[i]]; });
}

// Merge a hole (CW) into the outer polygon (CCW) through a mutually visible bridge
// (Eberly's "Triangulation by ear clipping", section 3).
void bridgeHole(const Ctx& c, std::vector<int>& poly, const std::vector<int>& hole) {
    int mi = 0;
    for (int i = 1; i < (int)hole.size(); ++i)
        if (c.pts[hole[i]].x > c.pts[hole[mi]].x) mi = i;
    Vec2 M = c.pts[hole[mi]];

    // Cast a ray towards +x, find the nearest polygon edge hit.
    double bestX = std::numeric_limits<double>::infinity();
    int bestPos = -1;
    for (int i = 0; i < (int)poly.size(); ++i) {
        Vec2 a = c.pts[poly[i]], b = c.pts[poly[(i + 1) % poly.size()]];
        if ((a.y > M.y) == (b.y > M.y) && a.y != M.y && b.y != M.y) continue;
        if (a.y == b.y) {
            if (a.y != M.y) continue;
            double x = std::min(a.x, b.x);
            if (x >= M.x && x < bestX) { bestX = x; bestPos = a.x < b.x ? i : (int)((i + 1) % poly.size()); }
            continue;
        }
        double x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x < M.x - 1e-15 || x >= bestX) continue;
        bestX = x;
        // Candidate: the endpoint with the larger x.
        bestPos = a.x > b.x ? i : (int)((i + 1) % poly.size());
        if (x == a.x && M.y == a.y) bestPos = i;
        if (x == b.x && M.y == b.y) bestPos = (int)((i + 1) % poly.size());
    }
    if (bestPos < 0) {
        // Should not happen for valid input; fall back to the closest vertex.
        double bd = std::numeric_limits<double>::infinity();
        for (int i = 0; i < (int)poly.size(); ++i) {
            double d = (c.pts[poly[i]] - M).length();
            if (d < bd) { bd = d; bestPos = i; }
        }
    }

    Vec2 I(bestX, M.y);
    Vec2 P = c.pts[poly[bestPos]];
    // If any reflex vertex lies inside triangle (M, I, P), pick the one with the
    // smallest angle to the ray instead.
    if (std::isfinite(bestX)) {
        auto inside = [&](const Vec2& q) {
            Vec2 a = M, b = I, d = P;
            double s = (b - a).cross(d - a);
            if (std::fabs(s) < 1e-300) return false;
            double c1 = (b - a).cross(q - a), c2 = (d - b).cross(q - b), c3 = (a - d).cross(q - d);
            if (s > 0) return c1 >= 0 && c2 >= 0 && c3 >= 0;
            return c1 <= 0 && c2 <= 0 && c3 <= 0;
        };
        double bestAng = std::numeric_limits<double>::infinity();
        double bestDist = std::numeric_limits<double>::infinity();
        int n = (int)poly.size();
        for (int i = 0; i < n; ++i) {
            if (i == bestPos) continue;
            int pi = poly[(i + n - 1) % n], ci = poly[i], ni = poly[(i + 1) % n];
            if (c.cross(pi, ci, ni) > 0) continue; // convex -> not reflex
            Vec2 q = c.pts[ci];
            if (q.x < M.x) continue;
            if (!inside(q)) continue;
            Vec2 d = q - M;
            double ang = std::atan2(std::fabs(d.y), d.x);
            double dist = d.length();
            if (ang < bestAng - 1e-12 || (std::fabs(ang - bestAng) <= 1e-12 && dist < bestDist)) {
                bestAng = ang;
                bestDist = dist;
                bestPos = i;
            }
        }
    }

    std::vector<int> out;
    out.reserve(poly.size() + hole.size() + 2);
    for (int i = 0; i <= bestPos; ++i) out.push_back(poly[i]);
    for (int k = 0; k <= (int)hole.size(); ++k) out.push_back(hole[(mi + k) % hole.size()]);
    out.push_back(poly[bestPos]);
    for (int i = bestPos + 1; i < (int)poly.size(); ++i) out.push_back(poly[i]);
    poly.swap(out);
}

} // namespace

std::vector<std::array<int, 3>> triangulatePolygon(const std::vector<std::vector<Vec2>>& loops) {
    std::vector<std::array<int, 3>> tris;
    if (loops.empty() || loops[0].size() < 3) return tris;

    Ctx c;
    std::vector<std::vector<int>> idx;
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto& l : loops) {
        std::vector<int> li;
        for (const Vec2& p : l) {
            li.push_back((int)c.pts.size());
            c.pts.push_back(p);
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
        idx.push_back(std::move(li));
    }
    double scale = std::max(maxX - minX, maxY - minY);
    c.eps = scale * scale * 1e-13;

    std::vector<int> poly = idx[0];
    if (loopArea(c, poly) < 0) std::reverse(poly.begin(), poly.end());

    std::vector<std::vector<int>> holes;
    for (size_t i = 1; i < idx.size(); ++i) {
        if (idx[i].size() < 3) continue;
        std::vector<int> h = idx[i];
        if (loopArea(c, h) > 0) std::reverse(h.begin(), h.end());
        holes.push_back(std::move(h));
    }
    std::sort(holes.begin(), holes.end(), [&](const std::vector<int>& a, const std::vector<int>& b) {
        double ax = -1e300, bx = -1e300;
        for (int i : a) ax = std::max(ax, c.pts[i].x);
        for (int i : b) bx = std::max(bx, c.pts[i].x);
        return ax > bx;
    });
    for (const auto& h : holes) bridgeHole(c, poly, h);

    // Ear clipping.
    std::vector<int> v = poly;
    int guard = 0;
    while (v.size() > 3 && guard < 100000) {
        ++guard;
        int n = (int)v.size();
        bool clipped = false;
        for (int i = 0; i < n; ++i) {
            int a = v[(i + n - 1) % n], b = v[i], d = v[(i + 1) % n];
            double cr = c.cross(a, b, d);
            if (cr <= c.eps) continue;
            bool ear = true;
            for (int k = 0; k < n; ++k) {
                int p = v[k];
                if (p == a || p == b || p == d) continue;
                if (c.same(p, a) || c.same(p, b) || c.same(p, d)) continue;
                // Only reflex vertices can be inside an ear.
                int pp = v[(k + n - 1) % n], pn = v[(k + 1) % n];
                if (c.cross(pp, p, pn) > c.eps) continue;
                if (c.inTri(a, b, d, p)) { ear = false; break; }
            }
            if (!ear) continue;
            tris.push_back({a, b, d});
            v.erase(v.begin() + i);
            clipped = true;
            break;
        }
        if (clipped) continue;
        // Remove degenerate (collinear / spike) vertices.
        bool removed = false;
        for (int i = 0; i < n; ++i) {
            int a = v[(i + n - 1) % n], b = v[i], d = v[(i + 1) % n];
            if (std::fabs(c.cross(a, b, d)) <= c.eps || c.same(a, b) || c.same(b, d)) {
                v.erase(v.begin() + i);
                removed = true;
                break;
            }
        }
        if (removed) continue;
        // Last resort for self-intersecting input: clip the most convex vertex.
        int best = -1;
        double bc = -1e300;
        for (int i = 0; i < n; ++i) {
            double cr = c.cross(v[(i + n - 1) % n], v[i], v[(i + 1) % n]);
            if (cr > bc) { bc = cr; best = i; }
        }
        if (bc > 0) tris.push_back({v[(best + n - 1) % n], v[best], v[(best + 1) % n]});
        v.erase(v.begin() + best);
    }
    if (v.size() == 3 && c.cross(v[0], v[1], v[2]) > c.eps) tris.push_back({v[0], v[1], v[2]});
    return tris;
}

} // namespace gs
