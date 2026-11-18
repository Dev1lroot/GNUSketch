// SPDX-License-Identifier: GPL-3.0-or-later
#include "Slicer.h"

#include "Components.h"
#include "Solids.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace gs {

namespace {

constexpr int kMaxPieces = 512;

// A closed box (faces outward).
Mesh solidBox(const Vec3& lo, const Vec3& hi) {
    Mesh m;
    Id v[8];
    for (int i = 0; i < 8; ++i) v[i] = m.addVertex(Vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
    auto quad = [&](int a, int b, int c, int d, const Vec3& n) { m.addFaceRaw({v[a], v[b], v[c], v[d]}, {}, n); };
    quad(0, 2, 3, 1, Vec3(0, 0, -1));
    quad(4, 5, 7, 6, Vec3(0, 0, 1));
    quad(0, 1, 5, 4, Vec3(0, -1, 0));
    quad(2, 6, 7, 3, Vec3(0, 1, 0));
    quad(0, 4, 6, 2, Vec3(-1, 0, 0));
    quad(1, 3, 7, 5, Vec3(1, 0, 0));
    return m;
}

// A straight prism: the outline (in the plane, around `axis`) from a to b along it.
Mesh prism(const std::vector<Vec3>& outline, const Vec3& axis, double a, double b) {
    Mesh m;
    const size_t n = outline.size();
    std::vector<Id> lo(n), hi(n);
    for (size_t i = 0; i < n; ++i) {
        lo[i] = m.addVertex(outline[i] + axis * a);
        hi[i] = m.addVertex(outline[i] + axis * b);
    }
    // Outline counter-clockwise around the axis?
    Vec3 nrm;
    for (size_t i = 0; i < n; ++i) nrm = nrm + outline[i].cross(outline[(i + 1) % n]);
    const bool ccw = nrm.dot(axis) > 0;
    std::vector<Id> bottom(lo.rbegin(), lo.rend()), top(hi.begin(), hi.end());
    if (!ccw) std::swap(bottom, top), std::reverse(bottom.begin(), bottom.end()), std::reverse(top.begin(), top.end());
    m.addFaceRaw(bottom, {}, -axis);
    m.addFaceRaw(top, {}, axis);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const Vec3 side = (outline[j] - outline[i]).cross(axis) * (ccw ? 1.0 : -1.0);
        m.addFaceRaw({lo[i], lo[j], hi[j], hi[i]}, {}, side.normalized());
    }
    return m;
}

// Inside/outside tests against one closed mesh, with per-face boxes.
struct SolidTester {
    const Mesh& m;
    std::vector<std::pair<Vec3, Vec3>> boxes;
    double slack = 0;
    explicit SolidTester(const Mesh& mesh) : m(mesh) {
        Vec3 mn, mx;
        bool any = false;
        m.bounds(mn, mx, any);
        slack = (mx - mn).length() * 0.004 + 1e-9;
        boxes.resize(m.faces.size());
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            Vec3 a(1e300, 1e300, 1e300), b(-1e300, -1e300, -1e300);
            for (Id v : m.faces[f].outer)
                for (int k = 0; k < 3; ++k) {
                    a[k] = std::min(a[k], m.pos(v)[k]);
                    b[k] = std::max(b[k], m.pos(v)[k]);
                }
            boxes[f] = {a, b};
        }
    }
    // Rays run nearly along +axis (tilted a little so they miss edges).
    int hits(const Vec3& p, int axis) const {
        Vec3 d;
        d[axis] = 1;
        d[(axis + 1) % 3] = 0.00123;
        d[(axis + 2) % 3] = 0.00271;
        const Ray r{p, d.normalized()};
        const int u = (axis + 1) % 3, w = (axis + 2) % 3;
        int n = 0;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            const auto& [a, b] = boxes[f];
            if (b[axis] < p[axis] || a[u] > p[u] + slack || b[u] < p[u] - slack || a[w] > p[w] + slack || b[w] < p[w] - slack) continue;
            double t;
            if (m.rayFace(f, r, t) && t > 1e-12) ++n;
        }
        return n;
    }
    bool inside(const Vec3& p) const {
        int votes = 0;
        for (int a = 0; a < 3; ++a) votes += hits(p, a) % 2;
        return votes >= 2;
    }
};

struct Piece {
    Mesh m;
    int idx[3] = {0, 0, 0};
};

bool boundsOf(const Mesh& m, Vec3& mn, Vec3& mx) {
    bool any = false;
    m.bounds(mn, mx, any);
    return any;
}

} // namespace

bool pointInSolid(const Mesh& m, const Vec3& p) { return SolidTester(m).inside(p); }

void slicerGrid(const Vec3& mn, const Vec3& mx, const SlicerParams& p, int n[3], Vec3& cell) {
    for (int a = 0; a < 3; ++a) {
        const double len = std::max(0.0, mx[a] - mn[a]);
        const double want = std::max(1e-6, p.size[a]);
        n[a] = std::max(1, (int)std::ceil(len / want - 1e-9));
        cell[a] = p.equal ? (len > 0 ? len / n[a] : want) : want;
    }
}

bool sliceSelection(ModelState& st, Selection& sel, const SlicerParams& p, SlicerResult* out, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    // ---- the solid to cut, in world coordinates --------------------------------
    Mesh src;
    const Mesh& active = st.activeMesh();
    std::set<Id> faces;
    for (Id f : sel.faces)
        if (f >= 0 && f < (Id)active.faces.size() && active.faces[f].alive) faces.insert(f);
    if (!faces.empty()) {
        const std::set<Id> noEdges;
        appendMesh(src, active, Mat4(), &faces, &noEdges, true);
    }
    std::string baseName;
    std::set<Id> insts;
    for (Id i : sel.instances)
        if (i >= 0 && i < (Id)st.instances.size() && st.instances[i].alive) insts.insert(i);
    for (const PlacedInstance& pi : placedInstances(st))
        if (insts.count(pi.root)) appendMesh(src, st.defs[pi.def].mesh, pi.xf, nullptr, nullptr, true);
    if (insts.size() == 1 && faces.empty()) baseName = st.defs[st.instances[*insts.begin()].def].name;
    if (baseName.empty()) baseName = "Part";
    if (src.aliveFaceCount() == 0) return fail("Select a solid (its faces, or a component) to slice.");
    std::string why;
    if (!isSolid(src, &why))
        return fail("The Slicer needs closed solids: " + why +
                    ". Effects > Solid > Solidify closes small holes; larger openings need faces drawn over them.");

    Vec3 mn, mx;
    boundsOf(src, mn, mx);
    int n[3];
    Vec3 cell;
    slicerGrid(mn, mx, p, n, cell);
    if ((long long)n[0] * n[1] * n[2] > kMaxPieces)
        return fail("That would be more than " + std::to_string(kMaxPieces) + " pieces: use a larger slice size.");

    // ---- cut along x, then y, then z -----------------------------------------
    std::array<std::vector<double>, 3> cuts;
    std::vector<Piece> pieces(1);
    pieces[0].m = src;
    const Vec3 far = (mx - mn) + Vec3(1, 1, 1);
    for (int a = 0; a < 3; ++a) {
        for (int k = 1; k < n[a]; ++k) cuts[a].push_back(mn[a] + k * cell[a]);
        std::vector<Piece> next;
        for (Piece& pc : pieces) {
            Piece rest = std::move(pc);
            for (int k = 0; k < (int)cuts[a].size(); ++k) {
                Vec3 lo, hi;
                if (!boundsOf(rest.m, lo, hi)) break;
                const double c = cuts[a][k];
                if (hi[a] <= c + 1e-9) break;      // all below: done with this one
                if (lo[a] >= c - 1e-9) {           // all above
                    rest.idx[a] = k + 1;
                    continue;
                }
                // Everything at and above the plane, as a box.
                Mesh below, above, unused;
                bool ok = false;
                std::string err;
                for (double nudge : {0.0, 1e-6, -1e-6, 3e-6}) {
                    Vec3 bl = mn - far, bh = mx + far;
                    bl[a] = c + nudge;
                    if (!splitMeshes(rest.m, solidBox(bl, bh), below, above, unused, &err)) continue;
                    // Both pieces must be closed solids again, or the next cut
                    // (and the print) fails; a nudged plane may cut cleanly.
                    std::string why;
                    if ((below.aliveFaceCount() > 0 && !isSolid(below, &why)) ||
                        (above.aliveFaceCount() > 0 && !isSolid(above, &why))) {
                        err = "a piece would not be closed: " + why;
                        continue;
                    }
                    ok = true;
                    cuts[a][k] = c + nudge;
                    break;
                }
                if (!ok) {
                    char at[64];
                    std::snprintf(at, sizeof at, "%c = %.4g m", "xyz"[a], c);
                    return fail("Could not cut the model at " + std::string(at) + ": " + err +
                                ". Try a slightly different slice size.");
                }
                if (below.aliveFaceCount() > 0) {
                    Piece b;
                    b.m = std::move(below);
                    std::copy(rest.idx, rest.idx + 3, b.idx);
                    next.push_back(std::move(b));
                }
                rest.m = std::move(above);
                rest.idx[a] = k + 1;
                if (rest.m.aliveFaceCount() == 0) break;
            }
            if (rest.m.aliveFaceCount() > 0) next.push_back(std::move(rest));
        }
        pieces = std::move(next);
    }

    // ---- connectors on the inner cuts -------------------------------------------
    int connectors = 0;
    if (p.connector != SlicerParams::None && p.connectorSize > 0 && p.connectorLength > 0) {
        const SolidTester inside(src);
        const double half = p.connectorSize / 2, L = p.connectorLength, clr = std::max(0.0, p.clearance);
        // Room for a connector of this footprint (around q, in the cut plane)
        // in the solid on both sides of the cut.
        auto roomFor = [&](const std::vector<Vec3>& ring, const Vec3& axis) {
            const double reach = L + clr + p.margin;
            for (double d : {-reach, -L * 0.5, -1e-5, 1e-5, L * 0.5, reach})
                for (const Vec3& q : ring)
                    if (!inside.inside(q + axis * d)) return false;
            return true;
        };
        for (size_t ai = 0; ai < pieces.size(); ++ai)
            for (size_t bi = 0; bi < pieces.size(); ++bi) {
                int axis = -1, diff = 0;
                for (int k = 0; k < 3; ++k)
                    if (pieces[ai].idx[k] != pieces[bi].idx[k]) {
                        ++diff;
                        axis = k;
                    }
                if (diff != 1 || pieces[bi].idx[axis] != pieces[ai].idx[axis] + 1) continue;
                const double c = cuts[axis][pieces[ai].idx[axis]];
                const int u = (axis + 1) % 3, w = (axis + 2) % 3;
                Vec3 ax, eu, ew;
                ax[axis] = 1;
                eu[u] = 1;
                ew[w] = 1;
                // The shared cell face.
                const double u0 = std::max(mn[u], mn[u] + pieces[ai].idx[u] * cell[u]), u1 = std::min(mx[u], u0 + cell[u]);
                const double w0 = std::max(mn[w], mn[w] + pieces[ai].idx[w] * cell[w]), w1 = std::min(mx[w], w0 + cell[w]);
                // Connectors stay inside the face the two pieces share (with
                // their margin), and inside both pieces along the axis.
                const double inset = half + p.margin + clr;
                if (u1 - u0 < 2 * inset || w1 - w0 < 2 * inset) continue;
                if (cell[axis] < L + clr + p.margin) continue;
                auto at = [&](double uu, double ww) {
                    Vec3 q;
                    q[axis] = c;
                    q[u] = uu;
                    q[w] = ww;
                    return q;
                };
                // Footprints: what the connector covers, and with the margin round it.
                auto footprint = [&](const Vec3& q, double hu, double hw, bool round, double grow) {
                    std::vector<Vec3> ring;
                    if (round) {
                        for (int i = 0; i < 16; ++i) {
                            const double t = 2 * kPi * i / 16;
                            ring.push_back(q + eu * ((hu + grow) * std::cos(t)) + ew * ((hu + grow) * std::sin(t)));
                        }
                    } else {
                        ring = {q + eu * (-hu - grow) + ew * (-hw - grow), q + eu * (hu + grow) + ew * (-hw - grow),
                                q + eu * (hu + grow) + ew * (hw + grow), q + eu * (-hu - grow) + ew * (hw + grow)};
                    }
                    return ring;
                };
                auto sampleRing = [&](const Vec3& q, double hu, double hw, bool round) {
                    std::vector<Vec3> s = footprint(q, hu, hw, round, p.margin);
                    s.push_back(q);
                    if (!round) {
                        // Edge midpoints too (long ribs).
                        const size_t k = s.size() - 1;
                        for (size_t i = 0; i < k; ++i) s.push_back((s[i] + s[(i + 1) % k]) * 0.5);
                    }
                    return s;
                };
                struct Conn {
                    Vec3 q;
                    double hu, hw;
                };
                std::vector<Conn> chosen;
                const bool round = p.connector == SlicerParams::RoundPegs;
                if (p.connector == SlicerParams::Ribs) {
                    // Ribs along the longer side of the cut, where the solid allows.
                    const bool alongU = (u1 - u0) >= (w1 - w0);
                    const double a0 = (alongU ? u0 : w0) + clr, a1 = (alongU ? u1 : w1) - clr;
                    const double b0 = (alongU ? w0 : u0) + inset, b1 = (alongU ? w1 : u1) - inset;
                    const int lines = 7;
                    const double step = std::max(half, (a1 - a0) / 200);
                    struct Run {
                        double from, to, across;
                    };
                    std::vector<Run> runs;
                    for (int li = 0; li < lines; ++li) {
                        const double across = lines == 1 ? (b0 + b1) / 2 : b0 + double(li) / (lines - 1) * (b1 - b0);
                        double start = 0;
                        bool open = false;
                        Run best{0, 0, across};
                        for (double s = a0 + half + p.margin; s <= a1 - half - p.margin + 1e-12; s += step) {
                            const Vec3 q = alongU ? at(s, across) : at(across, s);
                            const bool ok = roomFor(sampleRing(q, half, half, false), ax);
                            if (ok && !open) {
                                start = s;
                                open = true;
                            }
                            if (open && (!ok || s + step > a1 - half - p.margin + 1e-12)) {
                                const double end = ok ? s : s - step;
                                if (end - start > best.to - best.from) best = {start, end, across};
                                open = false;
                            }
                        }
                        if (best.to - best.from >= 2 * p.connectorSize) runs.push_back(best);
                    }
                    std::sort(runs.begin(), runs.end(), [](const Run& x, const Run& y) { return x.to - x.from > y.to - y.from; });
                    for (const Run& r : runs) {
                        if ((int)chosen.size() >= std::max(1, p.perCut)) break;
                        bool apart = true;
                        for (const Conn& c2 : chosen) {
                            const double other = alongU ? c2.q[w] : c2.q[u];
                            apart = apart && std::fabs(other - r.across) >= p.connectorSize * 2 + 2 * p.margin;
                        }
                        if (!apart) continue;
                        const double mid = (r.from + r.to) / 2, len = (r.to - r.from) / 2 + half;
                        Conn cn;
                        cn.q = alongU ? at(mid, r.across) : at(r.across, mid);
                        cn.hu = alongU ? len : half;
                        cn.hw = alongU ? half : len;
                        chosen.push_back(cn);
                    }
                } else {
                    // Pegs or keys on a grid of places, spread as far apart as they go.
                    const int k = 6;
                    std::vector<Vec3> ok;
                    for (int i = 0; i < k; ++i)
                        for (int j = 0; j < k; ++j) {
                            const Vec3 q = at(u0 + inset + double(i) / (k - 1) * (u1 - u0 - 2 * inset),
                                              w0 + inset + double(j) / (k - 1) * (w1 - w0 - 2 * inset));
                            if (roomFor(sampleRing(q, half, half, round), ax)) ok.push_back(q);
                        }
                    if (!ok.empty()) {
                        Vec3 centre;
                        for (const Vec3& q : ok) centre = centre + q;
                        centre = centre * (1.0 / ok.size());
                        std::vector<Vec3> picked;
                        auto firstIt = std::min_element(ok.begin(), ok.end(), [&](const Vec3& x, const Vec3& y) {
                            return (x - centre).length() < (y - centre).length();
                        });
                        picked.push_back(*firstIt);
                        while ((int)picked.size() < std::max(1, p.perCut)) {
                            double best = -1;
                            Vec3 bestQ;
                            for (const Vec3& q : ok) {
                                double d = 1e300;
                                for (const Vec3& r : picked) d = std::min(d, (q - r).length());
                                if (d > best) {
                                    best = d;
                                    bestQ = q;
                                }
                            }
                            if (best < p.connectorSize * 2 + 2 * p.margin) break;
                            picked.push_back(bestQ);
                        }
                        // With more than one, the middle one goes if the others spread wider.
                        if (picked.size() > 2) picked.erase(picked.begin());
                        for (const Vec3& q : picked) chosen.push_back({q, half, half});
                    }
                }
                // Peg into the lower piece (sunk half its length to join it),
                // socket out of the upper one. Kept only when both pieces stay
                // closed solids; touching coplanar faces (ribs ending in one
                // plane) can trip the boolean, so a few tries nudge it by a hair.
                int tryNo = 0;
                for (const Conn& cn0 : chosen) {
                    ++tryNo;
                    for (int attempt = 0; attempt < 3; ++attempt) {
                        Conn cn = cn0;
                        const double j = 1.7e-5 * (attempt * 3 + tryNo % 3);
                        cn.q = cn.q + eu * j + ew * (j * 0.61);
                        cn.hu -= j * 0.37;
                        cn.hw -= j * 0.23;
                        Mesh peg = prism(footprint(cn.q, cn.hu, cn.hw, round, 0), ax, -L * 0.5, L);
                        Mesh socket = prism(footprint(cn.q, cn.hu, cn.hw, round, clr), ax, -std::max(clr, 1e-4), L + clr);
                        // What each piece must gain and lose: the part of the
                        // peg standing out, and the socket.
                        auto area = [&](const std::vector<Vec3>& ring) {
                            Vec3 a;
                            for (size_t i = 0; i < ring.size(); ++i) a = a + ring[i].cross(ring[(i + 1) % ring.size()]);
                            return std::fabs(a.dot(ax)) / 2;
                        };
                        const double gain = area(footprint(cn.q, cn.hu, cn.hw, round, 0)) * L;
                        const double loss = area(footprint(cn.q, cn.hu, cn.hw, round, clr)) * (L + clr);
                        const double va = solidVolume(pieces[ai].m), vb = solidVolume(pieces[bi].m);
                        auto near = [](double got, double want, double scale) { return std::fabs(got - want) <= scale * 0.02 + 1e-12; };
                        Mesh a2, b2;
                        if (!booleanMeshes(pieces[ai].m, peg, BoolOp::Union, a2) || !isSolid(a2) || !near(solidVolume(a2), va + gain, gain)) continue;
                        if (!booleanMeshes(pieces[bi].m, socket, BoolOp::Difference, b2) || !isSolid(b2) || !near(solidVolume(b2), vb - loss, loss))
                            continue;
                        pieces[ai].m = std::move(a2);
                        pieces[bi].m = std::move(b2);
                        ++connectors;
                        break;
                    }
                }
            }
    }

    // ---- the pieces as components, in place of the selection ------------------
    Mesh& from = st.activeMesh();
    {
        std::set<Id> edges;
        for (Id f : faces)
            for (Id e : from.faceEdges(f)) edges.insert(e);
        for (Id f : faces) from.eraseFace(f);
        for (Id e : edges) {
            bool used = false;
            for (Id f : from.edgeFaces(e)) used = used || from.faces[f].alive;
            if (!used) from.eraseEdge(e, false);
        }
        from.removeOrphanVertices();
        from.touch();
    }
    for (Id i : insts) st.instances[i].alive = false;
    SlicerResult res;
    res.nx = n[0];
    res.ny = n[1];
    res.nz = n[2];
    res.cell = cell;
    std::sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) {
        return std::make_tuple(x.idx[2], x.idx[1], x.idx[0]) < std::make_tuple(y.idx[2], y.idx[1], y.idx[0]);
    });
    for (Piece& pc : pieces) {
        Vec3 lo, hi;
        if (!boundsOf(pc.m, lo, hi)) continue;
        ComponentDef def;
        char label[48];
        std::snprintf(label, sizeof label, " %d-%d-%d", pc.idx[0] + 1, pc.idx[1] + 1, pc.idx[2] + 1);
        def.name = uniqueComponentName(st, baseName + label);
        def.uid = newComponentUid();
        appendMesh(def.mesh, pc.m, Mat4::translation(-lo));
        st.defs.push_back(std::move(def));
        Instance in;
        in.def = (Id)st.defs.size() - 1;
        in.parent = st.activeDef();
        in.xf = Mat4::translation(lo + Vec3(pc.idx[0], pc.idx[1], pc.idx[2]) * p.spread);
        st.instances.push_back(in);
        res.created.instances.insert((Id)st.instances.size() - 1);
        ++res.pieces;
    }
    res.connectors = connectors;
    sel = res.created;
    if (out) *out = std::move(res);
    return true;
}

} // namespace gs
