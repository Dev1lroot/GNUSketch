// SPDX-License-Identifier: GPL-3.0-or-later
#include "FollowMe.h"

#include <algorithm>
#include <map>
#include <optional>

namespace gs {

std::vector<Id> edgeChain(const Mesh& m, Id start) {
    if (start < 0 || start >= (Id)m.edges.size() || !m.edges[start].alive) return {};
    auto aliveEdges = [&](Id v) {
        std::vector<Id> r;
        for (Id e : m.vertexEdges(v))
            if (m.edges[e].alive) r.push_back(e);
        return r;
    };
    std::vector<Id> chain = {start};
    std::set<Id> used = {start};
    // Walk away from each end of the start edge; the second walk prepends.
    for (int side = 0; side < 2; ++side) {
        Id e = start, v = m.edges[start].v[side == 0 ? 1 : 0];
        for (;;) {
            std::vector<Id> es = aliveEdges(v);
            if (es.size() != 2) break;
            Id next = es[0] == e ? es[1] : es[0];
            if (used.count(next)) break; // closed loop
            used.insert(next);
            if (side == 0) chain.push_back(next);
            else chain.insert(chain.begin(), next);
            v = m.edges[next].other(v);
            e = next;
        }
    }
    return chain;
}

bool followMe(Mesh& m, Id profile, const std::set<Id>& pathIn, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (profile < 0 || profile >= (Id)m.faces.size() || !m.faces[profile].alive) return fail("Pick a face as the profile.");
    const Face P = m.faces[profile];
    std::set<Id> own;
    for (Id e : m.faceEdges(profile)) own.insert(e);
    std::set<Id> path;
    for (Id e : pathIn)
        if (e >= 0 && e < (Id)m.edges.size() && m.edges[e].alive && !own.count(e)) path.insert(e);
    if (path.empty()) return fail("Select the path edges (a line, arc or outline) other than the profile's own edges.");

    // ---- order the path ------------------------------------------------------
    std::map<Id, std::vector<Id>> vadj;
    for (Id e : path)
        for (Id v : m.edges[e].v) vadj[v].push_back(e);
    std::vector<Id> ends;
    for (const auto& [v, es] : vadj) {
        if (es.size() > 2) return fail("The path branches; select a single chain of edges.");
        if (es.size() == 1) ends.push_back(v);
    }
    if (!ends.empty() && ends.size() != 2) return fail("The path is not connected; select a single chain of edges.");
    const bool closed = ends.empty();
    std::vector<Id> verts, segEdges;
    {
        Id cur = closed ? vadj.begin()->first : ends[0], prev = kNone;
        verts.push_back(cur);
        for (;;) {
            Id next = kNone;
            for (Id e : vadj[cur])
                if (e != prev && (segEdges.empty() || e != segEdges.back())) next = e;
            if (next == kNone || (closed && !segEdges.empty() && next == segEdges.front())) break;
            segEdges.push_back(next);
            prev = next;
            cur = m.edges[next].other(cur);
            if (closed && cur == verts.front()) break;
            verts.push_back(cur);
        }
        if (segEdges.size() != path.size()) return fail("The path is not connected; select a single chain of edges.");
    }

    // Start where the profile is. An open path starts at its nearer end; a
    // closed path is cut open where the profile plane crosses it (nearest to
    // the profile), so the sweep starts and ends exactly on the profile.
    Vec3 centroid;
    for (Id v : P.outer) centroid += m.pos(v);
    centroid = centroid / double(P.outer.size());
    const Plane pp(P.normal, m.pos(P.outer[0]));
    std::vector<Vec3> pts;
    std::vector<Id> segs; // path edge of each segment (for smoothing)
    if (closed) {
        const int n = (int)verts.size();
        int best = -1;
        double bestD = 0, bestT = 0;
        for (int i = 0; i < n; ++i) {
            Vec3 a = m.pos(verts[i]), b = m.pos(verts[(i + 1) % n]);
            double da = pp.distance(a), db = pp.distance(b);
            if ((da > kTol && db > kTol) || (da < -kTol && db < -kTol) || std::fabs(da - db) < 1e-15) continue;
            double t = std::clamp(da / (da - db), 0.0, 1.0);
            double d = (lerp(a, b, t) - centroid).length();
            if (best < 0 || d < bestD) {
                best = i;
                bestD = d;
                bestT = t;
            }
        }
        if (best < 0) return fail("The profile face must cross the closed path.");
        Vec3 X = lerp(m.pos(verts[best]), m.pos(verts[(best + 1) % n]), bestT);
        pts.push_back(X);
        segs.push_back(segEdges[best]);
        for (int k = 1; k <= n; ++k) {
            pts.push_back(m.pos(verts[(best + k) % n]));
            segs.push_back(segEdges[(best + k) % n]);
        }
        pts.push_back(X); // segs already holds the closing segment's edge
    } else {
        if ((m.pos(verts.back()) - centroid).length() < (m.pos(verts.front()) - centroid).length()) {
            std::reverse(verts.begin(), verts.end());
            std::reverse(segEdges.begin(), segEdges.end());
        }
        for (Id v : verts) pts.push_back(m.pos(v));
        segs = segEdges;
    }
    // Drop zero-length segments (the cut landing on a path vertex).
    {
        std::vector<Vec3> p2 = {pts[0]};
        std::vector<Id> s2;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (pts[i].nearlyEquals(p2.back())) continue;
            p2.push_back(pts[i]);
            s2.push_back(segs[i - 1]);
        }
        pts = std::move(p2);
        segs = std::move(s2);
    }

    const int S = (int)segs.size(); // segments
    const int nv = S + 1;           // path points (closed: last == first)
    if (S < 1) return fail("The path is too short.");
    std::vector<Vec3> dir(S);
    for (int i = 0; i < S; ++i) dir[i] = (pts[i + 1] - pts[i]).normalized();
    const double facing = P.normal.dot(dir[0]);
    if (std::fabs(facing) < 1e-3) return fail("The profile face lies along the path; it must cross it.");

    // Plane the profile is carried onto at path point i: the mitre between the
    // segments, the profile plane at the ends of a closed path, and planes
    // perpendicular to the path at the end of an open one.
    auto mitre = [&](int i) -> std::optional<Plane> {
        if (closed && (i == 0 || i == nv - 1)) return pp;
        if (i == nv - 1) return Plane(dir[S - 1], pts[i]);
        Vec3 n = dir[i - 1] + dir[i];
        if (n.length() < 1e-6) return std::nullopt; // path doubles back
        return Plane(n.normalized(), pts[i]);
    };
    auto carry = [&](const std::vector<Vec3>& ring, const Vec3& d, const Plane& pl) -> std::optional<std::vector<Vec3>> {
        double dn = pl.n.dot(d);
        if (std::fabs(dn) < 1e-9) return std::nullopt;
        std::vector<Vec3> r;
        for (const Vec3& q : ring) r.push_back(q + d * ((pl.d - pl.n.dot(q)) / dn));
        return r;
    };

    // ---- rings: the profile loops at every path point ------------------------------
    std::vector<std::vector<Id>> loops = {P.outer};
    for (const auto& h : P.holes) loops.push_back(h);
    std::vector<std::vector<std::vector<Vec3>>> rings(nv); // [point][loop][vertex]
    for (const auto& L : loops) {
        std::vector<Vec3> r;
        for (Id v : L) r.push_back(m.pos(v));
        rings[0].push_back(r);
    }
    for (int i = 1; i < nv; ++i) {
        auto pl = mitre(i);
        if (!pl) return fail("The path doubles back on itself.");
        for (const auto& r : rings[i - 1]) {
            auto c = carry(r, dir[i - 1], *pl);
            if (!c) return fail("The profile cannot follow this path.");
            rings[i].push_back(*c);
        }
    }
    if (closed) rings[nv - 1] = rings[0]; // lands back on the profile

    // ---- build ------------------------------------------------------------------
    FaceSide front = P.front, back = P.back;
    front.tex.valid = back.tex.valid = false;
    front.axis = back.axis = Vec3();
    std::vector<std::vector<std::vector<Id>>> ids(nv);
    for (int i = 0; i < nv; ++i)
        for (const auto& r : rings[i]) {
            std::vector<Id> l;
            for (const Vec3& q : r) l.push_back(m.addVertex(q));
            ids[i].push_back(std::move(l));
        }
    auto addFace = [&](std::vector<Id> loop, const Vec3& hint) {
        std::vector<Id> l;
        for (Id v : loop)
            if (l.empty() || l.back() != v) l.push_back(v);
        while (l.size() > 1 && l.front() == l.back()) l.pop_back();
        if (l.size() < 3) return;
        Id f = m.addFaceRaw(l, {}, hint);
        if (f == kNone) return;
        m.faces[f].front = front;
        m.faces[f].back = back;
    };
    const double sgn = facing > 0 ? 1.0 : -1.0;
    for (int i = 0; i < S; ++i) {
        int a = i, b = i + 1;
        for (size_t li = 0; li < loops.size(); ++li) {
            const auto &A = ids[a][li], &B = ids[b][li];
            const size_t k = A.size();
            for (size_t j = 0; j < k; ++j) {
                size_t j2 = (j + 1) % k;
                Vec3 hint = (m.pos(A[j2]) - m.pos(A[j])).cross(dir[i]) * sgn;
                // Quads between two carried rings are planar for mitred
                // joints; split the rare twisted one (closed 3D paths).
                Vec3 n = (m.pos(A[j2]) - m.pos(A[j])).cross(m.pos(B[j2]) - m.pos(A[j]));
                bool planar = n.length() < 1e-12 ||
                              std::fabs((m.pos(B[j]) - m.pos(A[j])).dot(n.normalized())) <= kTol;
                if (planar) {
                    addFace({A[j], A[j2], B[j2], B[j]}, hint);
                } else {
                    addFace({A[j], A[j2], B[j2]}, hint);
                    addFace({A[j], B[j2], B[j]}, hint);
                    Id d = m.findEdge(A[j], B[j2]);
                    if (d != kNone) m.edges[d].soft = m.edges[d].smooth = true;
                }
            }
        }
    }
    if (closed) {
        m.eraseFace(profile);
    } else {
        if (m.faces[profile].normal.dot(dir[0]) > 0) m.reverseFace(profile);
        std::vector<std::vector<Id>> holes(ids[nv - 1].begin() + 1, ids[nv - 1].end());
        Id cap = m.addFaceRaw(ids[nv - 1][0], holes, dir[S - 1]);
        if (cap != kNone) {
            m.faces[cap].front = front;
            m.faces[cap].back = back;
        }
    }

    // ---- smoothing: curved paths and curved profile edges ------------------------
    auto soften = [&](Id a, Id b) {
        Id e = m.findEdge(a, b);
        if (e != kNone) m.edges[e].soft = m.edges[e].smooth = true;
    };
    for (int i = 0; i < nv - 1; ++i) {
        if (!closed && i == 0) continue;
        // At the start of a closed path the joint is between the last and first segment.
        Id ein = segs[i == 0 ? S - 1 : i - 1], eout = segs[i];
        if (m.edges[ein].curve == kNone || m.edges[ein].curve != m.edges[eout].curve) continue;
        for (const auto& l : ids[i])
            for (size_t j = 0; j < l.size(); ++j) soften(l[j], l[(j + 1) % l.size()]);
    }
    for (size_t li = 0; li < loops.size(); ++li) {
        const auto& L = loops[li];
        const size_t k = L.size();
        for (size_t j = 0; j < k; ++j) {
            Id ein = m.findEdge(L[(j + k - 1) % k], L[j]), eout = m.findEdge(L[j], L[(j + 1) % k]);
            if (ein == kNone || eout == kNone || m.edges[ein].curve == kNone || m.edges[ein].curve != m.edges[eout].curve)
                continue;
            for (int i = 0; i < S; ++i) soften(ids[i][li][j], ids[i + 1][li][j]);
        }
    }
    m.removeOrphanVertices();
    m.touch();
    m.markGeometryMoved();
    return true;
}

} // namespace gs
