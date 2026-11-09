// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for the geometry kernel.
#include "core/Animation.h"
#include "core/Physics.h"
#include "core/Components.h"
#include "core/Document.h"
#include "core/Effects.h"
#include "core/Walker.h"
#include "core/Mesh.h"
#include "core/Primitives.h"
#include "core/RoundCorners.h"
#include "core/Solids.h"
#include "core/Terrain.h"
#include "core/TextureMapping.h"
#include "core/Material.h"
#include "core/FollowMe.h"
#include "core/ImageTrace.h"
#include "core/Triangulate.h"
#include "core/Units.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace gs;

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) ++g_passed;                                                          \
        else { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        auto va_ = (a); auto vb_ = (b);                                                             \
        if (va_ == vb_) ++g_passed;                                                                 \
        else { ++g_failed; std::printf("  FAIL %s:%d: %s == %s (%g vs %g)\n", __FILE__, __LINE__,  \
                                       #a, #b, double(va_), double(vb_)); }                         \
    } while (0)

static void rect(Mesh& m, double x0, double y0, double x1, double y1, double z = 0) {
    m.drawPolyline({{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}}, true);
}

static Id onlyFace(const Mesh& m) {
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) return f;
    return kNone;
}

static double totalArea(const Mesh& m) {
    double a = 0;
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) a += m.faceArea(f);
    return a;
}

static void test(const char* name, const std::function<void()>& fn) {
    int before = g_failed;
    fn();
    std::printf("%s %s\n", g_failed == before ? "[ ok ]" : "[FAIL]", name);
}

int main() {
    test("triangulate square with hole", [] {
        std::vector<std::vector<Vec2>> loops = {
            {{0, 0}, {4, 0}, {4, 4}, {0, 4}},
            {{1, 1}, {1, 3}, {3, 3}, {3, 1}},
        };
        auto tris = triangulatePolygon(loops);
        CHECK_EQ((int)tris.size(), 8);
        std::vector<Vec2> all;
        for (auto& l : loops) all.insert(all.end(), l.begin(), l.end());
        double area = 0;
        for (auto& t : tris) area += (all[t[1]] - all[t[0]]).cross(all[t[2]] - all[t[0]]) * 0.5;
        CHECK(std::fabs(area - 12.0) < 1e-9);
    });

    test("triangulate concave L", [] {
        std::vector<std::vector<Vec2>> loops = {{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}}};
        auto tris = triangulatePolygon(loops);
        CHECK_EQ((int)tris.size(), 4);
        double area = 0;
        for (auto& t : tris) area += (loops[0][t[1]] - loops[0][t[0]]).cross(loops[0][t[2]] - loops[0][t[0]]) * 0.5;
        CHECK(std::fabs(area - 3.0) < 1e-9);
    });

    test("four lines make a face", [] {
        Mesh m;
        m.drawLine({0, 0, 0}, {1, 0, 0});
        m.drawLine({1, 0, 0}, {1, 1, 0});
        m.drawLine({1, 1, 0}, {0, 1, 0});
        CHECK_EQ(m.aliveFaceCount(), 0);
        m.drawLine({0, 1, 0}, {0, 0, 0});
        CHECK_EQ(m.aliveFaceCount(), 1);
        CHECK_EQ(m.aliveEdgeCount(), 4);
        Id f = onlyFace(m);
        CHECK(std::fabs(m.faceArea(f) - 1.0) < 1e-9);
        // Ground faces face down like SketchUp.
        CHECK(m.faces[f].normal.z < -0.99);
        CHECK_EQ((int)m.faces[f].outer.size(), 4);
    });

    test("non-triangulated n-gon", [] {
        Mesh m;
        std::vector<Vec3> pts;
        for (int i = 0; i < 7; ++i) pts.push_back({std::cos(i * 2 * kPi / 7), std::sin(i * 2 * kPi / 7), 0.5});
        m.drawPolyline(pts, true);
        CHECK_EQ(m.aliveFaceCount(), 1);
        CHECK_EQ((int)m.faces[onlyFace(m)].outer.size(), 7);
    });

    test("crossing lines split each other", [] {
        Mesh m;
        m.drawLine({-1, 0, 0}, {1, 0, 0});
        m.drawLine({0, -1, 0}, {0, 1, 0});
        CHECK_EQ(m.aliveEdgeCount(), 4);
        CHECK_EQ(m.aliveVertexCount(), 5);
        CHECK(m.findVertex({0, 0, 0}) != kNone);
    });

    test("line across face splits it", [] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        CHECK_EQ(m.aliveFaceCount(), 1);
        m.drawLine({1, 0, 0}, {1, 1, 0});
        CHECK_EQ(m.aliveFaceCount(), 2);
        CHECK(std::fabs(totalArea(m) - 2.0) < 1e-9);
        // Diagonal line through an existing vertex-free interior
        m.drawLine({0, 0, 0}, {1, 1, 0});
        CHECK_EQ(m.aliveFaceCount(), 3);
        CHECK(std::fabs(totalArea(m) - 2.0) < 1e-9);
    });

    test("split face keeps material", [] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        m.faces[onlyFace(m)].front.material = 3;
        m.drawLine({1, -0.5, 0}, {1, 1.5, 0}); // crosses the boundary on both sides
        int painted = 0;
        for (const Face& f : m.faces)
            if (f.alive && f.front.material == 3) ++painted;
        CHECK_EQ(painted, 2);
        CHECK_EQ(m.aliveFaceCount(), 2);
    });

    test("rectangle inside face makes a hole", [] {
        Mesh m;
        rect(m, 0, 0, 4, 4);
        rect(m, 1, 1, 3, 3);
        CHECK_EQ(m.aliveFaceCount(), 2);
        int withHole = 0;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].holes.size() == 1) {
                ++withHole;
                CHECK(std::fabs(m.faceArea(f) - 12.0) < 1e-9);
            }
        CHECK_EQ(withHole, 1);
        CHECK(std::fabs(totalArea(m) - 16.0) < 1e-9);
    });

    test("rectangle around face makes a hole", [] {
        Mesh m;
        rect(m, 1, 1, 3, 3);
        rect(m, 0, 0, 4, 4);
        CHECK_EQ(m.aliveFaceCount(), 2);
        CHECK(std::fabs(totalArea(m) - 16.0) < 1e-9);
    });

    test("overlapping rectangles", [] {
        Mesh m;
        rect(m, 0, 0, 2, 2);
        rect(m, 1, 1, 3, 3);
        CHECK_EQ(m.aliveFaceCount(), 3);
        CHECK(std::fabs(totalArea(m) - 7.0) < 1e-9);
    });

    test("erase dividing edge merges faces", [] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        auto e = m.drawLine({1, 0, 0}, {1, 1, 0});
        CHECK_EQ(m.aliveFaceCount(), 2);
        m.eraseEdge(e[0], true);
        CHECK_EQ(m.aliveFaceCount(), 1);
        CHECK_EQ(m.aliveEdgeCount(), 4); // collinear pieces re-merged
        CHECK(std::fabs(totalArea(m) - 2.0) < 1e-9);
    });

    test("erase boundary edge removes face", [] {
        Mesh m;
        rect(m, 0, 0, 1, 1);
        Id e = m.findEdge(m.findVertex({0, 0, 0}), m.findVertex({1, 0, 0}));
        m.eraseEdge(e, true);
        CHECK_EQ(m.aliveFaceCount(), 0);
        CHECK_EQ(m.aliveEdgeCount(), 3);
    });

    test("retracing an edge heals a face", [] {
        Mesh m;
        rect(m, 0, 0, 1, 1);
        m.eraseFace(onlyFace(m));
        CHECK_EQ(m.aliveFaceCount(), 0);
        m.drawLine({0, 0, 0}, {1, 0, 0});
        CHECK_EQ(m.aliveFaceCount(), 1);
    });

    test("push/pull rectangle to box", [] {
        Mesh m;
        rect(m, 0, 0, 1, 2);
        Id f = onlyFace(m);
        // Face points down (-Z); pulling up is a negative distance along the normal.
        m.pushPull(f, -1.5);
        CHECK_EQ(m.aliveFaceCount(), 6);
        CHECK_EQ(m.aliveEdgeCount(), 12);
        CHECK_EQ(m.aliveVertexCount(), 8);
        CHECK(std::fabs(totalArea(m) - (2 * 2 + 2 * 1.5 * 1 + 2 * 1.5 * 2)) < 1e-9);
        // All normals point outward from the box centre.
        Vec3 c(0.5, 1, 0.75);
        for (Id g = 0; g < (Id)m.faces.size(); ++g) {
            if (!m.faces[g].alive) continue;
            Vec3 p = m.faceInteriorPoint(g);
            CHECK(m.faces[g].normal.dot(p - c) > 0);
        }
    });

    test("push/pull top of box stretches", [] {
        Mesh m;
        rect(m, 0, 0, 1, 1);
        m.pushPull(onlyFace(m), -1);
        Id top = kNone;
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive && m.faces[g].normal.z > 0.99) top = g;
        CHECK(top != kNone);
        m.pushPull(top, 0.5);
        CHECK_EQ(m.aliveFaceCount(), 6);
        CHECK_EQ(m.aliveEdgeCount(), 12);
        CHECK(std::fabs(totalArea(m) - (2 + 4 * 1.5)) < 1e-9);
        m.pushPull(top, -1.0);
        CHECK(std::fabs(totalArea(m) - (2 + 4 * 0.5)) < 1e-9);
    });

    test("push/pull partial face of box top", [] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        m.pushPull(onlyFace(m), -1);
        // Split the top in two and pull up one half.
        m.drawLine({1, 0, 1}, {1, 1, 1});
        Id half = kNone;
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive && m.faces[g].normal.z > 0.99 && m.faceInteriorPoint(g).x > 1) half = g;
        CHECK(half != kNone);
        m.pushPull(half, 1);
        // Expected an L-shaped solid: 8 faces (front/back L faces merged).
        CHECK_EQ(m.aliveFaceCount(), 8);
        double expected = 2 * (2 * 1 + 1 * 1) + 1 * 1 * 2 /*top two*/ + 2 * 1 /*bottom*/ + 1 /*x0 end*/ +
                          2 /*x2 end*/ + 1 /*inner step wall*/;
        CHECK(std::fabs(totalArea(m) - expected) < 1e-9);
    });

    test("push/pull several selected faces at once", [] {
        // Two separate boxes: both tops go up together.
        Mesh m;
        rect(m, 0, 0, 1, 1);
        rect(m, 3, 0, 4, 1);
        std::set<Id> ground;
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive) ground.insert(g);
        CHECK_EQ((int)ground.size(), 2);
        std::vector<Id> moved = m.pushPullFaces(ground, -1);
        CHECK_EQ((int)moved.size(), 2);
        CHECK_EQ(m.aliveFaceCount(), 12);
        CHECK(std::fabs(totalArea(m) - 2 * 6) < 1e-9);

        // Both halves of a split box top pulled up: one region, no inner walls,
        // the dividing line is kept on the new top.
        Mesh b;
        rect(b, 0, 0, 2, 1);
        b.pushPull(onlyFace(b), -1);
        b.drawLine({1, 0, 1}, {1, 1, 1});
        std::set<Id> tops;
        for (Id g = 0; g < (Id)b.faces.size(); ++g)
            if (b.faces[g].alive && b.faces[g].normal.z > 0.99) tops.insert(g);
        CHECK_EQ((int)tops.size(), 2);
        moved = b.pushPullFaces(tops, 1);
        CHECK_EQ((int)moved.size(), 2);
        Vec3 mn, mx;
        bool any;
        b.bounds(mn, mx, any);
        CHECK(std::fabs(mx.z - 2) < 1e-9);
        CHECK_EQ(b.aliveFaceCount(), 7);
        CHECK(std::fabs(totalArea(b) - (2 * 2 + 2 * 2 * 2 + 2 * 1 * 2)) < 1e-9);
        for (Id g : moved) CHECK(b.faces[g].alive && b.faces[g].normal.z > 0.99 && std::fabs(b.faceArea(g) - 1) < 1e-9);

        // Opposite sides of a box: each moves along its own normal.
        Mesh c;
        rect(c, 0, 0, 1, 1);
        c.pushPull(onlyFace(c), -1);
        std::set<Id> sides;
        for (Id g = 0; g < (Id)c.faces.size(); ++g)
            if (c.faces[g].alive && std::fabs(c.faces[g].normal.x) > 0.99) sides.insert(g);
        CHECK_EQ((int)sides.size(), 2);
        c.pushPullFaces(sides, 0.5);
        c.bounds(mn, mx, any);
        CHECK(std::fabs(mn.x + 0.5) < 1e-9 && std::fabs(mx.x - 1.5) < 1e-9);
        CHECK_EQ(c.aliveFaceCount(), 6);
    });

    test("push/pull through a solid cuts a hole", [] {
        Mesh m;
        rect(m, 0, 0, 2, 2);
        m.pushPull(onlyFace(m), -1);
        rect(m, 0.5, 0.5, 1.5, 1.5, 1.0);
        Id inner = kNone;
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive && m.faces[g].normal.z > 0.99 && m.faces[g].holes.empty() &&
                std::fabs(m.faceArea(g) - 1.0) < 1e-9)
                inner = g;
        CHECK(inner != kNone);
        m.pushPull(inner, -1.0);
        // top ring, bottom ring, 4 outer sides, 4 inner sides
        CHECK_EQ(m.aliveFaceCount(), 10);
        int rings = 0;
        for (const Face& f : m.faces)
            if (f.alive && f.holes.size() == 1) ++rings;
        CHECK_EQ(rings, 2);
        CHECK(std::fabs(totalArea(m) - (2 * 3 + 4 * 2 + 4 * 1)) < 1e-9);
    });

    // A wall (x 0..4, y 0..0.5, z 0..3) with a door rectangle on the front face
    // that touches the bottom edge.
    auto doorWall = [](Mesh& m) {
        rect(m, 0, 0, 4, 0.5);
        m.pushPull(onlyFace(m), -3);
        m.drawPolyline({{1.5, 0, 0}, {1.5, 0, 2}, {2.5, 0, 2}, {2.5, 0, 0}}, false);
        Id door = kNone;
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive && m.faces[g].normal.y < -0.99 && std::fabs(m.faceArea(g) - 2.0) < 1e-9)
                door = g;
        return door;
    };
    // No two coplanar, opposite-facing faces may overlap (z-fighting).
    auto noCoincidentOpposites = [](const Mesh& m) {
        for (Id a = 0; a < (Id)m.faces.size(); ++a)
            for (Id b = a + 1; b < (Id)m.faces.size(); ++b) {
                if (!m.faces[a].alive || !m.faces[b].alive) continue;
                if (m.faces[a].normal.dot(m.faces[b].normal) > -1 + 1e-6) continue;
                if (!m.facePlane(a).coincident(m.facePlane(b), 1e-6)) continue;
                if (m.pointInFace(b, m.faceInteriorPoint(a), false) ||
                    m.pointInFace(a, m.faceInteriorPoint(b), false))
                    return false;
            }
        return true;
    };

    test("push/pull doorway through a wall cuts the floor", [&] {
        Mesh m;
        Id door = doorWall(m);
        CHECK(door != kNone);
        m.pushPull(door, -0.5);
        CHECK(noCoincidentOpposites(m));
        // front, back, top, 2 ends, 2 bottom pieces, 2 jambs, lintel
        CHECK_EQ(m.aliveFaceCount(), 10);
        double expected = 2 * (12 - 2) + 4 * 0.5 /*top*/ + 2 * 1.5 /*ends*/ + 3 * 0.5 /*bottom*/ +
                          2 * 2 * 0.5 /*jambs*/ + 0.5 /*lintel*/;
        CHECK(std::fabs(totalArea(m) - expected) < 1e-9);
        for (const Edge& e : m.edges)
            if (e.alive) CHECK(!m.edgeFaces(&e - &m.edges[0]).empty());
    });

    test("push/pull doorway recess notches the floor", [&] {
        Mesh m;
        Id door = doorWall(m);
        m.pushPull(door, -0.2);
        CHECK(noCoincidentOpposites(m));
        double expected = 2 * 12 /*front with recess + back*/ + 4 * 0.5 + 2 * 1.5 + (2 - 0.2) /*bottom*/ +
                          2 * 2 * 0.2 + 0.2;
        CHECK(std::fabs(totalArea(m) - expected) < 1e-9);
    });

    // Closed, consistently oriented solid: every edge has exactly two faces that
    // traverse it in opposite directions.
    auto watertight = [](const Mesh& m) {
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            std::vector<Id> fs;
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive) fs.push_back(f);
            if (fs.empty()) continue; // loose edges (e.g. a Follow Me path)
            if (fs.size() != 2) return false;
            Id a = m.edges[e].v[0], b = m.edges[e].v[1];
            if (m.faceEdgeDirection(fs[0], a, b) + m.faceEdgeDirection(fs[1], a, b) != 0) return false;
        }
        return true;
    };
    auto volume = [](const Mesh& m) {
        double v = 0;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) v += m.pos(m.faces[f].outer[0]).dot(m.faces[f].normal) * m.faceArea(f) / 3;
        return v;
    };
    auto unitCube = [](Mesh& m) {
        rect(m, 0, 0, 1, 1);
        m.pushPull(onlyFace(m), -1);
    };
    auto edgesWhere = [](const Mesh& m, const std::function<bool(const Vec3&, const Vec3&)>& pred) {
        std::set<Id> r;
        for (Id e = 0; e < (Id)m.edges.size(); ++e)
            if (m.edges[e].alive && pred(m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1]))) r.insert(e);
        return r;
    };
    // Area cut away from a corner of a square by an N-segment fillet of radius R.
    auto cornerLoss = [](double R, int N) { return R * R - N * 0.5 * R * R * std::sin(kPi / 2 / N); };

    test("round corners of a rectangle (2D)", [&] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        std::set<Id> all = edgesWhere(m, [](const Vec3&, const Vec3&) { return true; });
        std::string err;
        CHECK(roundCorners(m, all, 0.1, 6, &err));
        CHECK_EQ(m.aliveFaceCount(), 1);
        CHECK_EQ(m.aliveEdgeCount(), 4 + 4 * 6);
        CHECK(std::fabs(totalArea(m) - (2 - 4 * cornerLoss(0.1, 6))) < 1e-9);
        // The rounded outline can still be extruded.
        m.pushPull(onlyFace(m), -0.5);
        CHECK(watertight(m));
    });

    test("round one edge of a cube", [&] {
        Mesh m;
        unitCube(m);
        std::set<Id> top = edgesWhere(m, [](const Vec3& a, const Vec3& b) {
            return a.z > 0.99 && b.z > 0.99 && a.y < 0.01 && b.y < 0.01;
        });
        CHECK_EQ((int)top.size(), 1);
        std::string err;
        CHECK(roundCorners(m, top, 0.2, 4, &err));
        CHECK(watertight(m));
        CHECK_EQ(m.aliveFaceCount(), 6 + 4);
        CHECK(std::fabs(volume(m) - (1 - cornerLoss(0.2, 4))) < 1e-9);
    });

    test("round the four top edges of a cube (mitred corners)", [&] {
        Mesh m;
        unitCube(m);
        std::set<Id> top = edgesWhere(m, [](const Vec3& a, const Vec3& b) { return a.z > 0.99 && b.z > 0.99; });
        CHECK_EQ((int)top.size(), 4);
        std::string err;
        CHECK(roundCorners(m, top, 0.25, 5, &err));
        CHECK(watertight(m));
        CHECK(volume(m) < 1 - 4 * 0.75 * cornerLoss(0.25, 5) + 1e-9);
    });

    test("round all edges of a cube (spherical corners)", [&] {
        Mesh m;
        unitCube(m);
        std::set<Id> all = edgesWhere(m, [](const Vec3&, const Vec3&) { return true; });
        std::string err;
        CHECK(roundCorners(m, all, 0.2, 4, &err));
        CHECK(watertight(m));
        Vec3 c(0.5, 0.5, 0.5);
        for (Id g = 0; g < (Id)m.faces.size(); ++g)
            if (m.faces[g].alive) CHECK(m.faces[g].normal.dot(m.faceInteriorPoint(g) - c) > 0);
        // Facets cut slightly inside the true rounding; with more segments the
        // volume converges to that of the exactly rounded cube.
        double exact = 1 - 12 * 0.6 * (0.2 * 0.2 - kPi * 0.04 / 4) - 8 * (0.008 - kPi * 0.008 / 6);
        CHECK(volume(m) < exact);
        Mesh fine;
        unitCube(fine);
        CHECK(roundCorners(fine, edgesWhere(fine, [](const Vec3&, const Vec3&) { return true; }), 0.2, 16, &err));
        CHECK(watertight(fine));
        CHECK(std::fabs(volume(fine) - exact) < 1e-3);
    });

    test("round corners rejects a radius that is too large", [&] {
        Mesh m;
        unitCube(m);
        std::set<Id> all = edgesWhere(m, [](const Vec3&, const Vec3&) { return true; });
        std::string err;
        int faces = m.aliveFaceCount();
        CHECK(!roundCorners(m, all, 0.6, 4, &err));
        CHECK(!err.empty());
        CHECK_EQ(m.aliveFaceCount(), faces);
    });

    // Face whose outer loop has exactly these corners.
    auto faceWith = [](const Mesh& m, size_t corners, double area) {
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].outer.size() == corners && std::fabs(m.faceArea(f) - area) < 1e-9) return f;
        return kNone;
    };
    auto pathEdges = [](const std::vector<Id>& es) { return std::set<Id>(es.begin(), es.end()); };

    test("follow me: profile along a line makes a beam", [&] {
        Mesh m;
        m.drawPolyline({{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, true);
        Id prof = faceWith(m, 4, 1.0);
        CHECK(prof != kNone);
        std::vector<Id> path = m.drawLine({0, 0, 0}, {3, 0, 0});
        std::string err;
        CHECK(followMe(m, prof, pathEdges(path), &err));
        CHECK(watertight(m));
        CHECK_EQ(m.aliveFaceCount(), 6);
        CHECK(std::fabs(volume(m) - 3) < 1e-9);
    });

    test("follow me: mitred L-shaped path", [&] {
        Mesh m;
        m.drawPolyline({{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}, true);
        Id prof = faceWith(m, 4, 1.0);
        std::vector<Id> path = m.drawPolyline({{0, 0, 0}, {3, 0, 0}, {3, 3, 0}}, false);
        std::string err;
        CHECK(followMe(m, prof, pathEdges(path), &err));
        CHECK(watertight(m));
        // Pappus for mitred prisms: area x length of the centroid's path.
        CHECK(std::fabs(volume(m) - 5) < 1e-9);
    });

    test("follow me: hollow pipe around a corner", [&] {
        Mesh m;
        auto ring = [](double r) {
            std::vector<Vec3> p;
            for (int i = 0; i < 24; ++i) p.push_back({0, 2 + r * std::cos(i * 2 * kPi / 24), 2 + r * std::sin(i * 2 * kPi / 24)});
            return p;
        };
        m.drawPolyline(ring(0.5), true, m.newCurveId());
        m.drawPolyline(ring(0.3), true, m.newCurveId());
        Id prof = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            if (m.faces[f].holes.size() == 1) prof = f;
            else m.eraseFace(f); // the inner disk: the pipe is hollow
        }
        CHECK(prof != kNone);
        double area = m.faceArea(prof);
        std::vector<Id> path = m.drawPolyline({{0, 2, 2}, {4, 2, 2}, {4, 6, 2}}, false);
        std::string err;
        CHECK(followMe(m, prof, pathEdges(path), &err));
        CHECK(watertight(m));
        CHECK(std::fabs(volume(m) - area * 8) < 1e-9);
        int soft = 0;
        for (const Edge& e : m.edges)
            if (e.alive && e.soft) ++soft;
        CHECK_EQ(soft, 2 * 24 * 2); // along both segments, outer and inner wall
    });

    test("follow me: profile around a circle (lathe)", [&] {
        Mesh m;
        m.drawPolyline({{1, 0, 0}, {2, 0, 0}, {2, 0, 1}, {1, 0, 1}}, true);
        Id prof = faceWith(m, 4, 1.0);
        CHECK(prof != kNone);
        std::vector<Vec3> c;
        for (int i = 0; i < 24; ++i) c.push_back({1.5 * std::cos(i * 2 * kPi / 24), 1.5 * std::sin(i * 2 * kPi / 24), -1});
        std::vector<Id> path = m.drawPolyline(c, true, m.newCurveId());
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (f != prof && m.faces[f].alive) m.eraseFace(f); // the circle's own disk
        std::string err;
        CHECK(followMe(m, prof, pathEdges(path), &err));
        CHECK(watertight(m));
        CHECK_EQ(m.aliveFaceCount(), 24 * 4);
        // Each profile point keeps its corner radius: a 24-gon ring, radius 1..2, height 1.
        CHECK(std::fabs(volume(m) - 24 * 0.5 * std::sin(2 * kPi / 24) * (4 - 1)) < 1e-9);
    });

    test("follow me: edge chain follows a polyline", [&] {
        Mesh m;
        std::vector<Id> path = m.drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}, false);
        CHECK_EQ((int)edgeChain(m, path[1]).size(), 3);
    });

    // Synthetic image: value 1 where inside(x, y) (pixel centres), else 0.
    auto makeImage = [](int w, int h, const std::function<bool(double, double)>& inside) {
        ScalarImage img;
        img.width = w;
        img.height = h;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) img.values.push_back(inside(x + 0.5, y + 0.5) ? 1.f : 0.f);
        return img;
    };
    auto loopArea = [](const std::vector<Vec2>& L) {
        return std::fabs(signedArea2D((int)L.size(), [&](int i) { return L[i]; }));
    };

    test("trace image: disc contour", [&] {
        ScalarImage img = makeImage(64, 64, [](double x, double y) { return std::hypot(x - 32, y - 32) < 20; });
        TraceOptions opt;
        auto loops = traceContours(img, opt);
        CHECK_EQ((int)loops.size(), 1);
        if (!loops.empty()) CHECK(std::fabs(loopArea(loops[0]) - kPi * 400) < 0.03 * kPi * 400);
    });

    test("trace image: ring gives outer and inner contour, specks dropped", [&] {
        ScalarImage img = makeImage(64, 64, [](double x, double y) {
            double r = std::hypot(x - 32, y - 32);
            return (r < 25 && r > 12) || (std::fabs(x - 2) < 1 && std::fabs(y - 2) < 1);
        });
        TraceOptions opt;
        CHECK_EQ((int)traceContours(img, opt).size(), 2);
    });

    test("trace image: shape touching the border closes along it", [&] {
        ScalarImage img = makeImage(40, 20, [](double x, double) { return x < 20; });
        TraceOptions opt;
        auto loops = traceContours(img, opt);
        CHECK_EQ((int)loops.size(), 1);
        if (!loops.empty()) CHECK(std::fabs(loopArea(loops[0]) - 20 * 20) < 1e-6);
    });

    test("trace image: cut out an alpha shape from an image plane", [&] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        Id f = onlyFace(m);
        bool front = m.faces[f].normal.z > 0;
        TexMap map = fitTextureToFace(m, f, front);
        // A square with a square hole, in a 200 x 100 image of the 2 x 1 face.
        ScalarImage img = makeImage(200, 100, [](double x, double y) {
            bool outer = x > 20 && x < 80 && y > 20 && y < 80;
            bool hole = x > 40 && x < 60 && y > 40 && y < 60;
            return outer && !hole;
        });
        TraceOptions opt;
        std::string err;
        CHECK(traceImageOnFace(m, f, map, img, opt, &err));
        CHECK_EQ(m.aliveFaceCount(), 1);
        Id g = onlyFace(m);
        CHECK(g != kNone && m.faces[g].holes.size() == 1);
        CHECK(std::fabs(totalArea(m) - (0.6 * 0.6 - 0.2 * 0.2)) < 1e-6);
        CHECK_EQ(m.aliveEdgeCount(), 8);
        // Keeping the background splits the face instead.
        Mesh k;
        rect(k, 0, 0, 2, 1);
        opt.removeBackground = false;
        CHECK(traceImageOnFace(k, onlyFace(k), map, img, opt, &err));
        CHECK_EQ(k.aliveFaceCount(), 3);
        CHECK(std::fabs(totalArea(k) - 2) < 1e-6);
    });

    test("trace image: traced curves push/pull with smooth sides, corners stay sharp", [&] {
        auto traceAndPull = [&](const std::function<bool(double, double)>& shape) {
            Mesh m;
            rect(m, 0, 0, 1, 1);
            Id f = onlyFace(m);
            TexMap map = fitTextureToFace(m, f, m.faces[f].normal.z > 0);
            std::string err;
            traceImageOnFace(m, f, map, makeImage(100, 100, shape), TraceOptions(), &err);
            Id g = onlyFace(m);
            m.pushPull(g, m.faces[g].normal.z < 0 ? -0.2 : 0.2);
            int soft = 0, vertical = 0;
            for (const Edge& e : m.edges) {
                if (!e.alive || std::fabs(m.pos(e.v[0]).z - m.pos(e.v[1]).z) < 0.1) continue;
                ++vertical;
                if (e.soft) ++soft;
            }
            return std::make_pair(soft, vertical);
        };
        auto disc = traceAndPull([](double x, double y) { return std::hypot(x - 50, y - 50) < 35; });
        CHECK(disc.second > 8 && disc.first == disc.second);
        auto square = traceAndPull([](double x, double y) { return x > 20 && x < 80 && y > 20 && y < 80; });
        CHECK_EQ(square.second, 4);
        CHECK_EQ(square.first, 0);
    });

    test("circle push/pull has soft sides", [] {
        Mesh m;
        std::vector<Vec3> pts;
        for (int i = 0; i < 24; ++i) pts.push_back({std::cos(i * 2 * kPi / 24), std::sin(i * 2 * kPi / 24), 0});
        Id c = m.newCurveId();
        m.drawPolyline(pts, true, c);
        CHECK_EQ(m.aliveFaceCount(), 1);
        m.pushPull(onlyFace(m), -1);
        CHECK_EQ(m.aliveFaceCount(), 26);
        int soft = 0;
        for (const Edge& e : m.edges)
            if (e.alive && e.soft) ++soft;
        CHECK_EQ(soft, 24);
    });

    test("12 edges of a cube create 6 faces", [] {
        Mesh m;
        Vec3 p[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        int E[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : E) m.drawLine(p[e[0]], p[e[1]]);
        CHECK_EQ(m.aliveFaceCount(), 6);
        CHECK_EQ(m.aliveEdgeCount(), 12);
    });

    test("move vertices welds", [] {
        Mesh m;
        rect(m, 0, 0, 1, 1);
        rect(m, 2, 0, 3, 1);
        std::set<Id> vs;
        for (Id v = 0; v < (Id)m.vertices.size(); ++v)
            if (m.vertices[v].alive && m.vertices[v].p.x > 1.5) vs.insert(v);
        m.transformVertices(vs, Mat4::translation({-1, 0, 0}));
        CHECK_EQ(m.aliveVertexCount(), 6);
        CHECK_EQ(m.aliveEdgeCount(), 7);
        CHECK_EQ(m.aliveFaceCount(), 2);
    });

    test("duplicate and translate", [] {
        Mesh m;
        rect(m, 0, 0, 1, 1);
        std::set<Id> nf, ne, nv;
        m.duplicate({onlyFace(m)}, {}, nf, ne, nv);
        m.transformVertices(nv, Mat4::translation({0, 0, 2}));
        CHECK_EQ(m.aliveFaceCount(), 2);
        CHECK_EQ(m.aliveEdgeCount(), 8);
    });

    test("compact keeps topology", [] {
        Mesh m;
        rect(m, 0, 0, 2, 1);
        auto e = m.drawLine({1, 0, 0}, {1, 1, 0});
        m.eraseEdge(e[0], true);
        m.compact();
        CHECK_EQ((int)m.faces.size(), 1);
        CHECK_EQ((int)m.edges.size(), 4);
        CHECK_EQ((int)m.vertices.size(), 4);
        m.drawLine({1, 0, 0}, {1, 1, 0});
        CHECK_EQ(m.aliveFaceCount(), 2);
    });

    test("vertical face", [] {
        Mesh m;
        m.drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 0, 3}, {0, 0, 3}}, true);
        CHECK_EQ(m.aliveFaceCount(), 1);
        m.drawLine({0.5, 0, 0}, {0.5, 0, 2});
        m.drawLine({0.5, 0, 2}, {1.5, 0, 2});
        m.drawLine({1.5, 0, 2}, {1.5, 0, 0});
        CHECK_EQ(m.aliveFaceCount(), 2); // door opening split
        CHECK(std::fabs(totalArea(m) - 6.0) < 1e-9);
    });

    test("undo/redo", [] {
        Document d;
        d.beginOp("Rect");
        rect(d.mesh(), 0, 0, 1, 1);
        d.commitOp();
        CHECK_EQ(d.mesh().aliveFaceCount(), 1);
        d.undo();
        CHECK_EQ(d.mesh().aliveFaceCount(), 0);
        d.redo();
        CHECK_EQ(d.mesh().aliveFaceCount(), 1);
        d.beginOp("x");
        d.mesh().eraseFace(onlyFace(d.mesh()));
        d.cancelOp();
        CHECK_EQ(d.mesh().aliveFaceCount(), 1);
    });

    // A 1x1x1 box at (x, y, 0) in the top-level mesh.
    auto box = [](Document& d, double x, double y) {
        Mesh& m = d.mesh();
        rect(m, x, y, x + 1, y + 1);
        Id f = kNone;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive && m.pointInFace(i, Vec3(x + 0.5, y + 0.5, 0))) f = i;
        if (m.faces[f].normal.z < 0) m.reverseFace(f);
        m.pushPull(f, 1.0);
    };
    auto selectAllGeometry = [](Document& d) {
        Selection s;
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive) s.faces.insert(f);
        for (Id e = 0; e < (Id)d.mesh().edges.size(); ++e)
            if (d.mesh().edges[e].alive) s.edges.insert(e);
        return s;
    };

    test("make component moves geometry into a definition", [&] {
        Document d;
        box(d, 2, 3);
        CHECK_EQ(d.mesh().aliveFaceCount(), 6);
        Id inst = makeComponent(d.state, selectAllGeometry(d), "Box");
        CHECK(inst != kNone);
        CHECK_EQ(d.state.mesh.aliveFaceCount(), 0);
        CHECK_EQ(d.state.mesh.aliveEdgeCount(), 0);
        CHECK_EQ((int)d.state.defs.size(), 1);
        CHECK_EQ(d.state.defs[0].mesh.aliveFaceCount(), 6);
        CHECK(d.state.defs[0].name == "Box");
        // Axes at the bounding box minimum; world bounds unchanged.
        Vec3 mn, mx;
        CHECK(instanceBounds(d.state, inst, mn, mx));
        CHECK(mn.nearlyEquals(Vec3(2, 3, 0)) && mx.nearlyEquals(Vec3(3, 4, 1)));
        Vec3 lmn, lmx;
        bool any;
        d.state.defs[0].mesh.bounds(lmn, lmx, any);
        CHECK(lmn.nearlyEquals(Vec3(0, 0, 0)));
    });

    test("make component of part keeps the rest intact", [&] {
        Document d;
        box(d, 0, 0);
        Selection s;
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive && d.mesh().faces[f].normal.z > 0.9) s.faces.insert(f);
        CHECK_EQ((int)s.faces.size(), 1);
        makeComponent(d.state, s, "Lid");
        CHECK_EQ(d.state.mesh.aliveFaceCount(), 5);
        CHECK_EQ(d.state.mesh.aliveEdgeCount(), 12); // the top rim still bounds the sides
        CHECK_EQ(d.state.defs[0].mesh.aliveFaceCount(), 1);
    });

    test("editing a component changes every instance", [&] {
        Document d;
        box(d, 0, 0);
        Id a = makeComponent(d.state, selectAllGeometry(d), "Box");
        Id b = cloneInstance(d.state, a);
        d.state.instances[b].xf = Mat4::translation(Vec3(5, 0, 0)) * Mat4::rotation(Vec3(0, 0, 1), kPi / 2) *
                                  d.state.instances[b].xf;
        CHECK_EQ(instanceCount(d.state, 0), 2);
        Vec3 bmn, bmx;
        instanceBounds(d.state, b, bmn, bmx);

        d.openComponent(b);
        CHECK(d.editingComponent());
        // The open definition is in b's world coordinates.
        Vec3 mn, mx;
        bool any;
        d.mesh().bounds(mn, mx, any);
        CHECK(mn.nearlyEquals(bmn) && mx.nearlyEquals(bmx));
        // Other instance drawn relative to the open one stays where it was.
        Vec3 amn, amx;
        instanceBounds(d.state, a, amn, amx);
        CHECK(amn.nearlyEquals(Vec3(0, 0, 0), 1e-9) && amx.nearlyEquals(Vec3(1, 1, 1), 1e-9));

        // Push the top face of the open copy up by 1.
        d.beginOp("Push/Pull");
        Mesh& m = d.mesh();
        Id top = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].normal.z > 0.9) top = f;
        m.pushPull(top, 1.0);
        d.commitOp();
        instanceBounds(d.state, a, amn, amx);
        CHECK(std::fabs(amx.z - 2) < 1e-9); // the other copy grew too

        d.closeComponent();
        CHECK(!d.editingComponent());
        CHECK(d.selection.instances.count(b));
        instanceBounds(d.state, b, bmn, bmx);
        CHECK(std::fabs(bmx.z - 2) < 1e-9 && std::fabs(bmn.x - 4) < 1e-9);
        d.state.defs[0].mesh.bounds(mn, mx, any);
        CHECK(mn.nearlyEquals(Vec3(0, 0, 0), 1e-9) && mx.nearlyEquals(Vec3(1, 1, 2), 1e-9));

        // Undo the push/pull: back inside the component, then out again.
        CHECK(d.undo());
        CHECK(d.editingComponent());
        d.closeComponent();
        instanceBounds(d.state, a, amn, amx);
        CHECK(std::fabs(amx.z - 1) < 1e-9);
    });

    test("make unique and explode", [&] {
        Document d;
        box(d, 0, 0);
        Id a = makeComponent(d.state, selectAllGeometry(d), "Box");
        Id b = cloneInstance(d.state, a);
        Id c = cloneInstance(d.state, a);
        makeUnique(d.state, {b});
        CHECK_EQ((int)d.state.defs.size(), 2);
        CHECK(d.state.instances[b].def == 1 && d.state.instances[a].def == 0 && d.state.instances[c].def == 0);
        CHECK(d.state.defs[1].name != d.state.defs[0].name);
        CHECK(d.state.defs[1].uid != d.state.defs[0].uid);
        makeUnique(d.state, {b}); // already unique
        CHECK_EQ((int)d.state.defs.size(), 2);

        d.state.instances[c].xf = Mat4::translation(Vec3(3, 0, 0)) * d.state.instances[c].xf;
        Selection out;
        explodeInstance(d.state, c, &out);
        CHECK(!d.state.instances[c].alive);
        CHECK_EQ(d.state.mesh.aliveFaceCount(), 6);
        CHECK_EQ((int)out.faces.size(), 6);
        Vec3 mn, mx;
        bool any;
        d.state.mesh.bounds(mn, mx, any);
        CHECK(mn.nearlyEquals(Vec3(3, 0, 0), 1e-9) && mx.nearlyEquals(Vec3(4, 1, 1), 1e-9));
        CHECK_EQ(instanceCount(d.state, 0), 1);
    });

    test("mirrored instance keeps faces outward", [&] {
        Document d;
        box(d, 0, 0);
        Id a = makeComponent(d.state, selectAllGeometry(d), "Box");
        d.state.instances[a].xf = Mat4::scale(Vec3(-1, 1, 1)) * d.state.instances[a].xf;
        d.openComponent(a);
        const Mesh& m = d.mesh();
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            Vec3 c = m.faceInteriorPoint(f);
            CHECK(m.faces[f].normal.dot(c - Vec3(-0.5, 0.5, 0.5)) > 0);
        }
        d.closeComponent();
        Mesh flat = flattenModel(d.state);
        for (Id f = 0; f < (Id)flat.faces.size(); ++f) {
            if (!flat.faces[f].alive) continue;
            CHECK(flat.faces[f].normal.dot(flat.faceInteriorPoint(f) - Vec3(-0.5, 0.5, 0.5)) > 0);
        }
    });

    test("units", [] {
        Units u;
        double m;
        CHECK(u.parseLength("1500", m) && std::fabs(m - 1.5) < 1e-12);
        CHECK(u.parseLength("1.5m", m) && std::fabs(m - 1.5) < 1e-12);
        CHECK(u.parseLength("150cm", m) && std::fabs(m - 1.5) < 1e-12);
        CHECK(u.parseLength("2,5m", m) && std::fabs(m - 2.5) < 1e-12);
        CHECK(u.parseLength("5'6\"", m) && std::fabs(m - 66 * 0.0254) < 1e-12);
        CHECK(u.parseLength("12\"", m) && std::fabs(m - 0.3048) < 1e-12);
        CHECK(!u.parseLength("abc", m));
        CHECK(u.format(1.5) == "1500mm");
    });

    test("terrain falloff", [] {
        CHECK(std::fabs(terrainFalloff(0, 1, 0) - 1) < 1e-12);
        CHECK(terrainFalloff(1, 1, 0.5) == 0);
        CHECK(std::fabs(terrainFalloff(0.4, 1, 0.5) - 1) < 1e-12);
        CHECK(terrainFalloff(0.7, 1, 0.5) > 0 && terrainFalloff(0.7, 1, 0.5) < 1);
        CHECK(terrainFalloff(0.6, 1, 0) > terrainFalloff(0.8, 1, 0));
    });

    test("triangulate face keeps area and orientation", [] {
        Mesh m;
        rect(m, 0, 0, 4, 4);
        rect(m, 1, 1, 2, 2);
        Id outer = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && !m.faces[f].holes.empty()) outer = f;
        CHECK(outer != kNone);
        Vec3 n = m.faces[outer].normal;
        auto tris = triangulateFace(m, outer);
        CHECK(tris.size() >= 8);
        double a = 0;
        for (Id t : tris) {
            a += m.faceArea(t);
            CHECK(m.faces[t].outer.size() == 3 && m.faces[t].normal.dot(n) > 0.999);
        }
        CHECK(std::fabs(a - 15.0) < 1e-9);
    });

    test("terrain: collinear boundary points leave no slits", [] {
        Mesh m;
        m.drawPolyline({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}}, true);
        Id f = onlyFace(m);
        auto tris = triangulateFace(m, f);
        for (Id t : tris) CHECK(m.faceArea(t) > 1e-9);
        CHECK(std::fabs(totalArea(m) - 4.0) < 1e-9);
    });

    test("terrain: mountain on a plane", [] {
        Mesh m;
        rect(m, -5, -5, 5, 5);
        Id f = onlyFace(m);
        const double side = m.faces[f].normal.z; // ground faces point down
        TerrainBrush b;
        b.radius = 2;
        b.force = 0.5;
        b.hardness = 0.3;
        TerrainStroke s(TerrainMode::Raise, b);
        CHECK(s.begin(m, Vec3(0, 0, 0), f, Vec3(0, 0, 1)));
        CHECK(s.direction().z > 0.999);
        for (int i = 0; i < 30; ++i) s.apply(m, Vec3(0, 0, 0), 1.0 / 30);
        s.finish(m, Vec3(0, 0, 0));
        double top = -1, corner = 0;
        for (const Vertex& v : m.vertices) {
            if (!v.alive) continue;
            top = std::max(top, v.p.z);
            if (std::fabs(v.p.x) > 4.99 && std::fabs(v.p.y) > 4.99) corner = std::max(corner, std::fabs(v.p.z));
        }
        CHECK(std::fabs(top - 0.5) < 0.05); // 1 s at 0.5 m/s
        CHECK(std::fabs(corner) < 1e-12);
        int tris = 0;
        double maxEdge = 0;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive) {
                CHECK(m.faces[i].outer.size() == 3);
                CHECK(m.faces[i].normal.z * side > 0);
                ++tris;
            }
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            const Vec3 mid = (m.pos(m.edges[e].v[0]) + m.pos(m.edges[e].v[1])) * 0.5;
            if (Vec3(mid.x, mid.y, 0).length() < 1.5) maxEdge = std::max(maxEdge, m.edgeLength(e));
        }
        CHECK(tris > 100);
        CHECK(maxEdge <= b.detail() + 1e-9);
        // Watertight: every interior edge is shared by exactly two triangles.
        Mesh copy = m;
        int open = 0;
        for (Id e = 0; e < (Id)copy.edges.size(); ++e) {
            if (!copy.edges[e].alive) continue;
            const Vec3 a = copy.pos(copy.edges[e].v[0]), c = copy.pos(copy.edges[e].v[1]);
            bool border = (std::fabs(a.x) > 4.99 && std::fabs(c.x) > 4.99 && std::fabs(a.x - c.x) < 1e-9) ||
                          (std::fabs(a.y) > 4.99 && std::fabs(c.y) > 4.99 && std::fabs(a.y - c.y) < 1e-9);
            if (!border && copy.edgeFaces(e).size() != 2) ++open;
        }
        CHECK_EQ(open, 0);

        // Pit on top of it lowers again; Flatten levels to the start height.
        TerrainStroke pit(TerrainMode::Lower, b);
        Id hitFace = kNone;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive && m.pointInFace(i, Vec3(3.5, 3.5, 0))) hitFace = i;
        CHECK(hitFace != kNone);
        CHECK(pit.begin(m, Vec3(3.5, 3.5, 0), hitFace, Vec3(0, 0, 1)));
        for (int i = 0; i < 10; ++i) pit.apply(m, Vec3(3.5, 3.5, 0), 0.1);
        double low = 0;
        for (const Vertex& v : m.vertices) if (v.alive) low = std::min(low, v.p.z);
        CHECK(low < -0.3);

        TerrainBrush fb = b;
        fb.radius = 8;
        fb.force = 10;
        TerrainStroke flat(TerrainMode::Flatten, fb);
        Id corner0 = kNone;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive && m.pointInFace(i, Vec3(-4.9, -4.9, 0))) corner0 = i;
        CHECK(flat.begin(m, Vec3(-4.9, -4.9, 0), corner0, Vec3(0, 0, 1)));
        for (int i = 0; i < 20; ++i) flat.apply(m, Vec3(0, 0, 0), 0.1);
        double dev = 0;
        for (const Vertex& v : m.vertices) if (v.alive) dev = std::max(dev, std::fabs(v.p.z));
        CHECK(dev < 1e-9);
    });

    test("terrain: vertical lock on a slope", [] {
        Mesh m;
        m.drawPolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 5}, {0, 10, 5}}, true);
        Id f = onlyFace(m);
        TerrainBrush b;
        b.radius = 2;
        b.force = 1;
        TerrainStroke s(TerrainMode::Raise, b);
        const Vec3 hit(5, 5, 2.5);
        CHECK(s.begin(m, hit, f, Vec3(0, 0, 1)));
        CHECK(std::fabs(s.direction().y) > 0.1); // follows the slope by default
        s.setDirection(terrainVertical(s.direction()));
        CHECK(s.direction().z > 0.999);
        std::vector<Vec3> before;
        for (const Vertex& v : m.vertices) before.push_back(v.p);
        s.apply(m, hit, 0.5);
        bool xyFixed = true, rose = false;
        for (size_t i = 0; i < before.size(); ++i) {
            const Vec3 d = m.vertices[i].p - before[i];
            xyFixed = xyFixed && std::fabs(d.x) < 1e-12 && std::fabs(d.y) < 1e-12;
            rose = rose || d.z > 0.4;
        }
        CHECK(xyFixed);
        CHECK(rose);
    });

    test("terrain: texture stays continuous across sculpted triangles", [] {
        // UVs of each shared vertex must agree between the faces on both sides
        // of every edge (front and back), otherwise the texture shows seams.
        auto maxSeam = [](const Mesh& m) {
            Material mat;
            mat.texture.worldWidth = 0.5;
            mat.texture.worldHeight = 0.5;
            double worst = 0;
            for (Id e = 0; e < (Id)m.edges.size(); ++e) {
                if (!m.edges[e].alive) continue;
                const auto& fs = m.edgeFaces(e);
                if (fs.size() != 2) continue;
                for (Id v : m.edges[e].v)
                    for (bool front : {true, false}) {
                        Vec2 a = faceTexCoord(m.faces[fs[0]], front, m.pos(v), &mat);
                        Vec2 b = faceTexCoord(m.faces[fs[1]], front, m.pos(v), &mat);
                        worst = std::max(worst, (a - b).length());
                    }
            }
            return worst;
        };
        Mesh m;
        rect(m, -5, -5, 5, 5);
        Id f = onlyFace(m);
        TerrainBrush b;
        b.radius = 2;
        b.force = 1;
        TerrainStroke s(TerrainMode::Raise, b);
        CHECK(s.begin(m, Vec3(0, 0, 0), f, Vec3(0, 0, 1)));
        for (int i = 0; i < 20; ++i) s.apply(m, Vec3(0.1 * i, 0, 0), 0.05);
        s.finish(m, Vec3(2, 0, 0));
        CHECK(maxSeam(m) < 1e-9);

        // Terrain sculpted before axes existed heals on the next stroke.
        for (Face& F : m.faces) F.front.axis = F.back.axis = Vec3();
        CHECK(maxSeam(m) > 1e-3);
        Id hit = kNone;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive && m.pointInFace(i, Vec3(4.5, 4.5, 0))) hit = i;
        TerrainStroke again(TerrainMode::Lower, b);
        CHECK(again.begin(m, Vec3(4.5, 4.5, 0), hit, Vec3(0, 0, 1)));
        again.apply(m, Vec3(4.5, 4.5, 0), 0.1);
        CHECK(maxSeam(m) < 1e-9);
    });

    test("texture brush: paint weights mix and erase", [] {
        Vertex v;
        paintVertex(v, 3, 0.5);
        CHECK(v.paint.size() == 1 && std::fabs(v.paint[0].weight - 0.5f) < 1e-6);
        paintVertex(v, 7, 0.5); // mixes: 3 fades to 0.25, 7 gets 0.5
        float w3 = 0, w7 = 0, sum = 0;
        for (auto& w : v.paint) { sum += w.weight; if (w.material == 3) w3 = w.weight; if (w.material == 7) w7 = w.weight; }
        CHECK(std::fabs(w3 - 0.25f) < 1e-6 && std::fabs(w7 - 0.5f) < 1e-6 && sum <= 1.0f + 1e-6);
        for (int i = 0; i < 40; ++i) paintVertex(v, 7, 0.5);
        CHECK(v.paint.size() == 1 && v.paint[0].material == 7 && v.paint[0].weight > 0.999f);
        for (int i = 0; i < 40; ++i) paintVertex(v, kNone, 0.5);
        CHECK(v.paint.empty());
    });

    test("texture brush: stroke paints over a textured face without moving it", [] {
        Mesh m;
        rect(m, 0, 0, 10, 10);
        Id f = onlyFace(m);
        m.faces[f].front.material = m.faces[f].back.material = 1; // the face's own texture
        TerrainBrush b;
        b.radius = 2;
        b.hardness = 0.5;
        TerrainStroke s(TerrainMode::Paint, b);
        s.setPaintMaterial(2);
        const Vec3 c(5, 5, 0);
        CHECK(s.begin(m, c, f, Vec3(0, 0, 1)));
        for (int i = 0; i < 10; ++i) s.apply(m, c, 0.05);
        double maxZ = 0, centre = 0, outside = 0;
        for (const Vertex& v : m.vertices) {
            if (!v.alive) continue;
            maxZ = std::max(maxZ, std::fabs(v.p.z));
            float w = 0;
            for (auto& pw : v.paint) if (pw.material == 2) w = pw.weight;
            double d = (v.p - c).length();
            if (d < 0.5) centre = std::max(centre, (double)w);
            if (d > 2.0) outside = std::max(outside, (double)w);
        }
        CHECK(maxZ == 0);
        CHECK(centre > 0.7);
        CHECK(outside == 0);
        int painted = 0;
        for (const Face& F : m.faces)
            if (F.alive) { CHECK(F.front.material == 1 && F.outer.size() == 3); ++painted; }
        CHECK(painted > 100); // subdivided so the soft edge has vertices
        // Split edges inherit the blend of their ends.
        s.finish(m, c);
        for (Id e = 0; e < (Id)m.edges.size(); ++e) CHECK(m.edges[e].alive);
    });

    test("terrain: max density limits the triangles", [] {
        auto sculpt = [](double density, int& tris, double& shortest) {
            Mesh m;
            rect(m, 0, 0, 6, 6);
            Id f = onlyFace(m);
            if (m.faces[f].normal.z < 0) m.reverseFace(f);
            TerrainBrush b;
            b.radius = 2;
            b.force = 1;
            b.density = density;
            TerrainStroke s(TerrainMode::Raise, b);
            CHECK(s.begin(m, Vec3(3, 3, 0), f, Vec3(0, 0, 1)));
            for (int i = 0; i < 20; ++i) s.apply(m, Vec3(3, 3, 0), 0.05);
            s.finish(m, Vec3(3, 3, 0));
            tris = 0;
            shortest = 1e300;
            for (Id i = 0; i < (Id)m.faces.size(); ++i) tris += m.faces[i].alive;
            for (Id e = 0; e < (Id)m.edges.size(); ++e)
                if (m.edges[e].alive && m.edges[e].soft) shortest = std::min(shortest, m.edgeLength(e));
        };
        TerrainBrush b;
        b.radius = 2;
        CHECK(std::fabs(b.detail() - 0.25) < 1e-12);
        b.density = 2;
        CHECK(std::fabs(b.detail() - 0.5) < 1e-12);
        b.density = 100; // finer than automatic: the automatic size still applies
        CHECK(std::fabs(b.detail() - 0.25) < 1e-12);
        int autoTris, capTris;
        double autoShort, capShort;
        sculpt(0, autoTris, autoShort);
        sculpt(2, capTris, capShort);
        std::printf("    tris auto=%d cap=%d shortest auto=%g cap=%g\n", autoTris, capTris, autoShort, capShort);
        CHECK(capTris * 2 < autoTris);
        CHECK(capShort >= 0.25 - 1e-9);
        CHECK(autoShort < 0.25);
    });

    test("terrain: walls on the plane stay planar", [] {
        Mesh m;
        rect(m, 0, 0, 4, 4);
        Id f = onlyFace(m);
        m.pushPull(f, 2 * m.faces[f].normal.z); // up
        Id top = kNone;
        for (Id i = 0; i < (Id)m.faces.size(); ++i)
            if (m.faces[i].alive && m.faces[i].normal.z > 0.99) top = i;
        TerrainBrush b;
        b.radius = 1.5;
        b.force = 1;
        TerrainStroke s(TerrainMode::Raise, b);
        CHECK(s.begin(m, Vec3(0.5, 2, 2), top, Vec3(0, 0, 1)));
        for (int i = 0; i < 10; ++i) s.apply(m, Vec3(0.5, 2, 2), 0.05);
        int walls = 0;
        for (Id i = 0; i < (Id)m.faces.size(); ++i) {
            if (!m.faces[i].alive || m.faces[i].outer.size() == 3) continue;
            ++walls;
            Plane P = m.facePlane(i);
            for (Id v : m.faceVertices(i)) CHECK(std::fabs(P.distance(m.pos(v))) < 1e-9);
        }
        CHECK_EQ(walls, 5);
        double top2 = 0;
        for (const Vertex& v : m.vertices) if (v.alive) top2 = std::max(top2, v.p.z);
        CHECK(top2 > 2.3);
    });

    // ---- solid tools ------------------------------------------------------------
    // Axis-aligned box [mn, mx] drawn as loose geometry.
    auto solidBox = [](Mesh& m, Vec3 mn, Vec3 mx) {
        m.drawPolyline({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}}, true);
        Vec3 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, mn.z);
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.facePlane(f).contains(c) && m.pointInFace(f, c)) {
                m.pushPull(f, (mx.z - mn.z) * m.faces[f].normal.z);
                return;
            }
    };
    auto boxMesh = [&](Vec3 mn, Vec3 mx) {
        Mesh m;
        solidBox(m, mn, mx);
        return m;
    };
    auto cylinder = [](Vec3 c, double r, double h, int n) {
        Mesh m;
        std::vector<Vec3> pts;
        for (int i = 0; i < n; ++i) pts.push_back(c + Vec3(r * std::cos(2 * kPi * i / n), r * std::sin(2 * kPi * i / n), 0));
        m.drawPolyline(pts, true, m.newCurveId());
        Id f = onlyFace(m);
        m.pushPull(f, h * m.faces[f].normal.z);
        return m;
    };
    auto near = [](double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; };
    // Every face lies in its plane and the result is a closed solid.
    auto wellFormed = [](const Mesh& m) {
        if (!isSolid(m)) return false;
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive) continue;
            int n = 0;
            for (Id f : m.edgeFaces(e)) n += m.faces[f].alive;
            if (n != 2) return false;
        }
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            Plane P = m.facePlane(f);
            for (Id v : m.faceVertices(f))
                if (std::fabs(P.distance(m.pos(v))) > 1e-7) return false;
        }
        return true;
    };

    test("solids: closed box is a solid, open box is not", [&] {
        Mesh m = boxMesh(Vec3(0, 0, 0), Vec3(2, 1, 1));
        CHECK(isSolid(m));
        CHECK(near(solidVolume(m), 2));
        Mesh open = m;
        for (Id f = 0; f < (Id)open.faces.size(); ++f)
            if (open.faces[f].alive && open.faces[f].normal.z > 0.9) open.eraseFace(f);
        std::string why;
        CHECK(!isSolid(open, &why));
        CHECK(why.find("one face") != std::string::npos);
        CHECK_EQ(solidVolume(open), 0.0);
        // Reversed faces are fixed up for the operation.
        Mesh rev = m;
        for (Id f = 0; f < (Id)rev.faces.size(); ++f)
            if (rev.faces[f].alive && f % 2) rev.reverseFace(f);
        CHECK(isSolid(rev));
        CHECK(near(solidVolume(rev), 2));
    });

    test("solids: overlapping boxes union / subtract / intersect", [&] {
        Mesh a = boxMesh(Vec3(0, 0, 0), Vec3(1, 1, 1));
        Mesh b = boxMesh(Vec3(0.5, 0.5, 0.5), Vec3(1.5, 1.5, 1.5));
        Mesh u, d, i;
        std::string err;
        CHECK(booleanMeshes(a, b, BoolOp::Union, u, &err));
        CHECK(booleanMeshes(a, b, BoolOp::Difference, d, &err));
        CHECK(booleanMeshes(a, b, BoolOp::Intersect, i, &err));
        CHECK(err.empty());
        CHECK(wellFormed(u) && wellFormed(d) && wellFormed(i));
        CHECK(near(solidVolume(u), 1.875));
        CHECK(near(solidVolume(d), 0.875));
        CHECK(near(solidVolume(i), 0.125));
        CHECK_EQ(i.aliveFaceCount(), 6);
        CHECK_EQ(d.aliveFaceCount(), 9); // three notched faces + three faces of the cut corner + three untouched
        CHECK_EQ(u.aliveFaceCount(), 12);
        // Faces stay n-gons: the notched faces are L-shaped hexagons.
        int hex = 0;
        for (const Face& F : d.faces) hex += F.alive && F.outer.size() == 6;
        CHECK_EQ(hex, 3);
    });

    test("solids: flush faces merge, touching faces vanish", [&] {
        Mesh a = boxMesh(Vec3(0, 0, 0), Vec3(1, 1, 1));
        Mesh b = boxMesh(Vec3(0.5, 0, 0), Vec3(1.5, 1, 1)); // shares four side planes
        Mesh u, d, i;
        CHECK(booleanMeshes(a, b, BoolOp::Union, u));
        CHECK(booleanMeshes(a, b, BoolOp::Difference, d));
        CHECK(booleanMeshes(a, b, BoolOp::Intersect, i));
        CHECK(wellFormed(u) && wellFormed(d) && wellFormed(i));
        CHECK(near(solidVolume(u), 1.5) && near(solidVolume(d), 0.5) && near(solidVolume(i), 0.5));
        CHECK_EQ(u.aliveFaceCount(), 6);
        CHECK_EQ(u.aliveEdgeCount(), 12);
        CHECK_EQ(d.aliveFaceCount(), 6);
        CHECK_EQ(i.aliveFaceCount(), 6);

        Mesh c = boxMesh(Vec3(1, 0, 0), Vec3(2, 1, 1)); // touches a's +x face
        Mesh j;
        CHECK(booleanMeshes(a, c, BoolOp::Union, j));
        CHECK(wellFormed(j) && near(solidVolume(j), 2));
        CHECK_EQ(j.aliveFaceCount(), 6);
        CHECK(booleanMeshes(a, c, BoolOp::Difference, j));
        CHECK(wellFormed(j) && near(solidVolume(j), 1));
        CHECK_EQ(j.aliveFaceCount(), 6);
        CHECK(booleanMeshes(a, c, BoolOp::Intersect, j));
        CHECK_EQ(j.aliveFaceCount(), 0);
    });

    test("solids: drilling a hole makes faces with holes", [&] {
        Mesh plate = boxMesh(Vec3(0, 0, 0), Vec3(2, 2, 1));
        Mesh drill = boxMesh(Vec3(0.5, 0.5, -1), Vec3(1.5, 1.5, 2));
        Mesh r;
        CHECK(booleanMeshes(plate, drill, BoolOp::Difference, r));
        CHECK(wellFormed(r));
        CHECK(near(solidVolume(r), 3));
        CHECK_EQ(r.aliveFaceCount(), 10);
        int holed = 0;
        for (const Face& F : r.faces) holed += F.alive && F.holes.size() == 1;
        CHECK_EQ(holed, 2);
        // A pocket that stops inside the plate.
        Mesh pocket = boxMesh(Vec3(0.5, 0.5, 0.5), Vec3(1.5, 1.5, 2));
        CHECK(booleanMeshes(plate, pocket, BoolOp::Difference, r));
        CHECK(wellFormed(r) && near(solidVolume(r), 3.5));
        CHECK_EQ(r.aliveFaceCount(), 11);
        // A cutter fully inside leaves a void (inward-facing inner shell).
        Mesh inner = boxMesh(Vec3(0.5, 0.5, 0.25), Vec3(1.5, 1.5, 0.75));
        CHECK(booleanMeshes(plate, inner, BoolOp::Difference, r));
        CHECK(isSolid(r) && near(solidVolume(r), 3.5));
        CHECK_EQ(r.aliveFaceCount(), 12);
    });

    test("solids: cylinder through a box keeps soft sides and n-gon caps", [&] {
        Mesh boxm = boxMesh(Vec3(-1, -1, 0), Vec3(1, 1, 1));
        Mesh cyl = cylinder(Vec3(0, 0, -0.5), 0.5, 2, 24);
        CHECK(isSolid(cyl));
        const double prism = 24 * 0.5 * 0.25 * std::sin(2 * kPi / 24); // cross-section area of the 24-gon
        Mesh r;
        CHECK(booleanMeshes(boxm, cyl, BoolOp::Difference, r));
        CHECK(wellFormed(r));
        CHECK(near(solidVolume(r), 4 - prism, 1e-6));
        int soft = 0, ring = 0;
        for (const Edge& E : r.edges) soft += E.alive && E.soft;
        for (const Face& F : r.faces) ring += F.alive && F.holes.size() == 1 && F.holes[0].size() == 24;
        CHECK_EQ(soft, 24);
        CHECK_EQ(ring, 2);
        Mesh i;
        CHECK(booleanMeshes(boxm, cyl, BoolOp::Intersect, i));
        CHECK(wellFormed(i) && near(solidVolume(i), prism, 1e-6));
        // Rotated, off-axis cutter: generic intersection curves.
        Mesh tilted = cyl;
        std::set<Id> all;
        for (Id v = 0; v < (Id)tilted.vertices.size(); ++v)
            if (tilted.vertices[v].alive) all.insert(v);
        tilted.transformVertices(all, Mat4::translation(Vec3(0.3, 0.1, 0.5)) * Mat4::rotation(Vec3(1, 0.3, 0), 0.7) *
                                          Mat4::translation(Vec3(0, 0, -0.5)), false);
        Mesh u;
        CHECK(booleanMeshes(boxm, tilted, BoolOp::Union, u));
        CHECK(booleanMeshes(boxm, tilted, BoolOp::Difference, r));
        CHECK(booleanMeshes(boxm, tilted, BoolOp::Intersect, i));
        CHECK(wellFormed(u) && wellFormed(r) && wellFormed(i));
        double vu = solidVolume(u), vr = solidVolume(r), vi = solidVolume(i);
        CHECK(near(vr + vi, 4, 1e-6));
        CHECK(near(vu, 4 + solidVolume(tilted) - vi, 1e-6));
    });

    test("solids: random and grid-aligned pairs keep volumes consistent", [&] {
        uint32_t seed = 12345;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / double(1 << 24); };
        auto moved = [](Mesh m, const Mat4& xf) {
            std::set<Id> all;
            for (Id v = 0; v < (Id)m.vertices.size(); ++v)
                if (m.vertices[v].alive) all.insert(v);
            m.transformVertices(all, xf, false);
            return m;
        };
        int bad = 0;
        for (int it = 0; it < 200; ++it) {
            Mesh a, b;
            if (it % 2) {
                // Grid-snapped boxes: many coplanar faces and shared edges/vertices.
                auto g = [&] { return std::floor(rnd() * 4) * 0.5; };
                Vec3 p(g(), g(), g());
                a = boxMesh(Vec3(0, 0, 0), Vec3(1.5, 1.5, 1.5));
                b = boxMesh(p, p + Vec3(0.5 + g(), 0.5 + g(), 0.5 + g()));
            } else {
                a = boxMesh(Vec3(0, 0, 0), Vec3(1, 1, 1));
                Mat4 xf = Mat4::translation(Vec3(rnd(), rnd(), rnd())) *
                          Mat4::rotation(Vec3(rnd() - 0.5, rnd() - 0.5, rnd() - 0.5), rnd() * 3);
                b = it % 4 == 0 ? moved(cylinder(Vec3(0, 0, -0.5), 0.3 + rnd() * 0.3, 1.2, 12 + it % 7), xf)
                                : moved(boxMesh(Vec3(-0.4, -0.3, -0.35), Vec3(0.4, 0.3, 0.35)), xf);
            }
            Mesh u, d, i;
            std::string err;
            bool ok = booleanMeshes(a, b, BoolOp::Union, u, &err) && booleanMeshes(a, b, BoolOp::Difference, d, &err) &&
                      booleanMeshes(a, b, BoolOp::Intersect, i, &err);
            // Signed volume straight from the faces: also defined for results that
            // touch themselves along an edge (not solids in the strict sense).
            auto vol = [](const Mesh& m) {
                double v = 0;
                for (Id f = 0; f < (Id)m.faces.size(); ++f) {
                    if (!m.faces[f].alive) continue;
                    Vec3 bu, bv;
                    planeBasis(m.faces[f].normal, bu, bv);
                    std::vector<std::vector<Vec2>> loops;
                    std::vector<Vec3> flat;
                    std::vector<std::vector<Id>> ls = {m.faces[f].outer};
                    for (const auto& h : m.faces[f].holes) ls.push_back(h);
                    for (const auto& L : ls) {
                        loops.emplace_back();
                        for (Id x : L) {
                            loops.back().emplace_back(m.pos(x).dot(bu), m.pos(x).dot(bv));
                            flat.push_back(m.pos(x));
                        }
                    }
                    for (const auto& t : triangulatePolygon(loops)) v += flat[t[0]].dot(flat[t[1]].cross(flat[t[2]])) / 6;
                }
                return v;
            };
            double va = vol(a), vb = vol(b), vu = vol(u), vd = vol(d), vi = vol(i);
            bool good = ok && near(vd + vi, va, 1e-6) && near(vu, va + vb - vi, 1e-6);
            // Results may touch themselves along an edge (grid cases); only check
            // closedness when the pieces are regular solids.
            for (const Mesh* r : {&u, &d, &i}) {
                if (r->aliveFaceCount() == 0) continue;
                std::string why;
                if (!isSolid(*r, &why) && why.find("more than two") == std::string::npos) good = false;
            }
            if (!good) {
                ++bad;
                std::printf("    case %d: ok=%d err='%s' va=%g vb=%g vu=%g vd=%g vi=%g\n", it, ok, err.c_str(), va, vb, vu, vd, vi);
            }
        }
        CHECK_EQ(bad, 0);
    });

    test("solids: split gives three pieces that add up", [&] {
        Mesh a = boxMesh(Vec3(0, 0, 0), Vec3(1, 1, 1));
        Mesh b = boxMesh(Vec3(0.25, 0.25, 0.5), Vec3(2, 2, 2));
        Mesh x, y, z;
        CHECK(splitMeshes(a, b, x, y, z));
        CHECK(wellFormed(x) && wellFormed(y) && wellFormed(z));
        double va = solidVolume(a), vb = solidVolume(b);
        CHECK(near(solidVolume(x) + solidVolume(y), va));
        CHECK(near(solidVolume(z) + solidVolume(y), vb));
        CHECK(near(solidVolume(y), 0.75 * 0.75 * 0.5));
    });

    test("solids: not a solid gives a message and no result", [&] {
        Mesh a = boxMesh(Vec3(0, 0, 0), Vec3(1, 1, 1));
        Mesh b;
        rect(b, 0, 0, 1, 1, 0.5);
        Mesh r;
        std::string err;
        CHECK(!booleanMeshes(a, b, BoolOp::Union, r, &err));
        CHECK(err.find("second solid is not a solid") != std::string::npos);
        CHECK_EQ(r.aliveFaceCount(), 0);
    });

    test("solids: component subtract, trim and split", [&] {
        auto twoBoxes = [&](Document& d, Id& target, Id& cutter) {
            solidBox(d.mesh(), Vec3(0, 0, 0), Vec3(2, 2, 1));
            target = makeComponent(d.state, selectAllGeometry(d), "Plate");
            solidBox(d.mesh(), Vec3(0.5, 0.5, -1), Vec3(1.5, 1.5, 2));
            cutter = makeComponent(d.state, selectAllGeometry(d), "Peg");
        };
        auto defVolume = [](const Document& d, Id inst) {
            return solidVolume(d.state.defs[d.state.instances[inst].def].mesh);
        };
        {
            Document d;
            Id plate, peg;
            twoBoxes(d, plate, peg);
            CHECK(instanceIsSolid(d.state, plate) && instanceIsSolid(d.state, peg));
            Selection out;
            std::string err;
            CHECK(solidOperation(d.state, SolidOp::Subtract, peg, plate, &out, &err));
            CHECK(!d.state.instances[peg].alive && d.state.instances[plate].alive);
            CHECK(out.instances == std::set<Id>{plate});
            CHECK(near(defVolume(d, plate), 3));
            // The result keeps the instance's own axes.
            CHECK(d.state.instances[plate].xf.isIdentity());
        }
        {
            Document d;
            Id plate, peg;
            twoBoxes(d, plate, peg);
            // Move the plate's instance: the result must stay where it is in the world.
            d.state.instances[plate].xf = Mat4::translation(Vec3(0.25, 0, 0)) * d.state.instances[plate].xf;
            Id copy = cloneInstance(d.state, plate);
            d.state.instances[copy].xf = Mat4::translation(Vec3(10, 0, 0)) * d.state.instances[copy].xf;
            CHECK(solidOperation(d.state, SolidOp::Trim, peg, plate));
            CHECK(d.state.instances[peg].alive && near(defVolume(d, peg), 3));
            CHECK(near(defVolume(d, plate), 3));
            CHECK(near(defVolume(d, copy), 4)); // the other copy keeps the old definition
            CHECK(d.state.instances[plate].def != d.state.instances[copy].def);
            Vec3 mn, mx;
            CHECK(instanceBounds(d.state, plate, mn, mx));
            CHECK(mn.nearlyEquals(Vec3(0.25, 0, 0)) && mx.nearlyEquals(Vec3(2.25, 2, 1)));
        }
        {
            Document d;
            Id plate, peg;
            twoBoxes(d, plate, peg);
            Selection out;
            CHECK(solidOperation(d.state, SolidOp::Split, peg, plate, &out));
            CHECK_EQ((int)out.instances.size(), 3);
            double total = 0;
            for (Id i : out.instances) total += defVolume(d, i);
            CHECK(near(total, 4 + 3 - 1));
            CHECK(near(defVolume(d, peg), 2) && near(defVolume(d, plate), 3));
            CHECK(d.state.defs[d.state.instances[peg].def].name == "Peg");
            CHECK(d.state.defs[d.state.instances[plate].def].name == "Plate");
        }
        {
            Document d;
            Id plate, peg;
            twoBoxes(d, plate, peg);
            CHECK(solidOperation(d.state, SolidOp::Union, peg, plate));
            CHECK(!d.state.instances[peg].alive && near(defVolume(d, plate), 6));
            Id far = kNone;
            solidBox(d.mesh(), Vec3(5, 5, 5), Vec3(6, 6, 6));
            far = makeComponent(d.state, selectAllGeometry(d), "Far");
            std::string err;
            ModelState before = d.state;
            CHECK(!solidOperation(d.state, SolidOp::Intersect, far, plate, nullptr, &err));
            CHECK(!err.empty() && d.state.instances[far].alive && d.state.instances[plate].alive);
        }
    });

    // ---- effects -------------------------------------------------------------
    auto allFaces = [](const Mesh& m) {
        std::set<Id> out;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) out.insert(f);
        return out;
    };
    auto cube = [&](Mesh& m, double a = 1) { solidBox(m, Vec3(0, 0, 0), Vec3(a, a, a)); };
    auto triangleMeshOnly = [](const Mesh& m) {
        for (const Face& F : m.faces)
            if (F.alive && (F.outer.size() != 3 || !F.holes.empty())) return false;
        return true;
    };
    auto worstWarp = [](const Mesh& m) {
        double worst = 0;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            Plane P = m.facePlane(f);
            for (Id v : m.faceVertices(f)) worst = std::max(worst, std::fabs(P.distance(m.pos(v))));
        }
        return worst;
    };
    // A plain upward 10 x 10 grid plane, as one n-gon.
    auto plate = [](Mesh& m, double size = 10) {
        m.groundFacesDown = false;
        m.viewHint = Vec3(0, 0, 10);
        rect(m, 0, 0, size, size);
    };

    test("effects: subdivide reaches the density and stays closed", [&] {
        Mesh m;
        cube(m);
        EffectResult r;
        CHECK(effectSubdivide(m, allFaces(m), 4, &r));
        CHECK(isSolid(m));
        CHECK(near(solidVolume(m), 1.0));
        CHECK(triangleMeshOnly(m));
        CHECK(m.aliveFaceCount() >= 6 * 16);
        CHECK_EQ((int)r.faces.size(), m.aliveFaceCount());
        for (Id e = 0; e < (Id)m.edges.size(); ++e)
            if (m.edges[e].alive) CHECK(m.edgeLength(e) <= 0.25 + 1e-9);
    });
    test("effects: triangulate and merge flat are inverses on a cube", [&] {
        Mesh m;
        cube(m);
        CHECK(effectTriangulate(m, allFaces(m)));
        CHECK_EQ(m.aliveFaceCount(), 12);
        CHECK(isSolid(m));
        CHECK(effectMergeFlat(m, allFaces(m)));
        CHECK_EQ(m.aliveFaceCount(), 6);
        CHECK_EQ(m.aliveVertexCount(), 8);
        CHECK(isSolid(m) && near(solidVolume(m), 1.0));
    });
    test("effects: merge flat keeps a hole and drops collinear vertices", [&] {
        Mesh m;
        plate(m, 10);
        rect(m, 4, 4, 6, 6);
        // Remove the inner square: a plate with a hole.
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].holes.empty()) m.eraseFace(f);
        CHECK_EQ(m.aliveFaceCount(), 1);
        const double area = totalArea(m);
        CHECK(effectSubdivide(m, allFaces(m), 1));
        CHECK(m.aliveFaceCount() > 50);
        CHECK(effectMergeFlat(m, allFaces(m)));
        CHECK_EQ(m.aliveFaceCount(), 1);
        CHECK_EQ(m.faces[onlyFace(m)].holes.size(), size_t(1));
        CHECK_EQ(m.aliveVertexCount(), 8);
        CHECK(near(totalArea(m), area));
    });
    test("effects: noise moves only allowed axes, within the distance", [&] {
        Mesh m;
        cube(m);
        NoiseParams p;
        p.x = p.y = false;
        p.z = true;
        p.amount = 0.2;
        p.scale = 0.4;
        std::vector<Vec2> before;
        CHECK(effectNoise(m, allFaces(m), p));
        CHECK(isSolid(m));
        double moved = 0;
        for (const Vertex& v : m.vertices) {
            if (!v.alive) continue;
            CHECK(v.p.x >= -1e-9 && v.p.x <= 1 + 1e-9 && v.p.y >= -1e-9 && v.p.y <= 1 + 1e-9);
            CHECK(v.p.z >= -0.2 - 1e-9 && v.p.z <= 1.2 + 1e-9);
            double dz = std::min(std::fabs(v.p.z), std::fabs(v.p.z - 1));
            if (v.p.x > 1e-9 && v.p.x < 1 - 1e-9 && v.p.y > 1e-9 && v.p.y < 1 - 1e-9) moved = std::max(moved, dz);
        }
        CHECK(moved > 0.02);
        CHECK(worstWarp(m) < 1e-6);

        Mesh a, b, c;
        cube(a); cube(b); cube(c);
        p.x = p.y = p.z = true;
        p.seed = 7;
        CHECK(effectNoise(a, allFaces(a), p));
        CHECK(effectNoise(b, allFaces(b), p));
        p.seed = 8;
        CHECK(effectNoise(c, allFaces(c), p));
        bool same = a.vertices.size() == b.vertices.size(), differs = false;
        for (size_t i = 0; same && i < a.vertices.size(); ++i) same = a.vertices[i].p.nearlyEquals(b.vertices[i].p, 1e-12);
        for (size_t i = 0; i < std::min(a.vertices.size(), c.vertices.size()); ++i) differs = differs || !a.vertices[i].p.nearlyEquals(c.vertices[i].p, 1e-6);
        CHECK(same && differs);

        p.x = p.y = p.z = false;
        std::string err;
        Mesh d;
        cube(d);
        CHECK(!effectNoise(d, allFaces(d), p, nullptr, &err) && !err.empty());
        CHECK_EQ(d.aliveFaceCount(), 6);
    });
    test("effects: a partial selection leaves its neighbours alone", [&] {
        Mesh m;
        cube(m);
        Id top = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].normal.z > 0.9) top = f;
        NoiseParams p;
        p.x = p.y = false; // keeps the top's inside within the outline, for the check below
        p.amount = 0.2;
        p.scale = 0.3;
        CHECK(effectNoise(m, {top}, p));
        CHECK(isSolid(m)); // still watertight: the sides took in the new outline vertices
        int planar = 0;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            if (m.faces[f].outer.size() > 3) ++planar;
            Plane P = m.facePlane(f);
            for (Id v : m.faceVertices(f)) CHECK(std::fabs(P.distance(m.pos(v))) < 1e-6);
        }
        CHECK(planar >= 5); // the other faces are still n-gons
        // The top's outline did not move.
        Vec3 mn, mx;
        bool any;
        m.bounds(mn, mx, any);
        for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
            if (!m.vertices[v].alive) continue;
            const Vec3& q = m.pos(v);
            const bool onOutline = q.x < 1e-9 || q.x > 1 - 1e-9 || q.y < 1e-9 || q.y > 1 - 1e-9;
            if (onOutline && q.z > 0.5) CHECK(near(q.z, 1.0));
        }
    });
    test("effects: blur softens a cube without shrinking it away", [&] {
        Mesh m;
        cube(m);
        BlurParams p;
        p.radius = 0.15;
        CHECK(effectBlur(m, allFaces(m), p));
        CHECK(isSolid(m));
        const double v = solidVolume(m);
        CHECK(v > 0.75 && v < 1.05);
        // The sharp corner (1,1,1) is gone, the middle of a face barely moved.
        double farthest = 0;
        for (const Vertex& q : m.vertices)
            if (q.alive) farthest = std::max(farthest, (q.p - Vec3(0.5, 0.5, 0.5)).length());
        CHECK(farthest < 0.82); // a corner is 0.866 away
        Vec3 mn, mx;
        bool any;
        m.bounds(mn, mx, any);
        CHECK(mx.z - mn.z > 0.85);
        // All soft: no creases left.
        for (Id e = 0; e < (Id)m.edges.size(); ++e)
            if (m.edges[e].alive) CHECK(m.edges[e].soft);
    });
    test("effects: blur keeps the outline of a selection", [&] {
        Mesh m;
        cube(m);
        Id top = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faces[f].normal.z > 0.9) top = f;
        CHECK(effectBlur(m, {top}, BlurParams{}));
        CHECK(isSolid(m) && near(solidVolume(m), 1.0));
    });
    test("effects: decimate reaches its targets and keeps the shape", [&] {
        Mesh m;
        cube(m);
        CHECK(effectSubdivide(m, allFaces(m), 8));
        const int fine = m.aliveFaceCount();
        CHECK(fine > 500);
        DecimateParams p;
        p.mergeFlat = false;
        p.ratio = 0.25;
        CHECK(effectDecimate(m, allFaces(m), p));
        CHECK(m.aliveFaceCount() <= fine / 4 + 1);
        CHECK(isSolid(m) && near(solidVolume(m), 1.0));
        // Flat faces collapse first, at no error: at 12 triangles a cube is still
        // exact, and merging brings back its 6 quads.
        p.mergeFlat = true;
        p.ratio = 12.0 / m.aliveFaceCount();
        CHECK(effectDecimate(m, allFaces(m), p));
        CHECK(isSolid(m) && near(solidVolume(m), 1.0));
        CHECK_EQ(m.aliveFaceCount(), 6);
    });
    test("effects: decimate by density on a bumpy plate", [&] {
        Mesh m;
        plate(m);
        NoiseParams n;
        n.x = n.y = false;
        n.amount = 0.4;
        n.scale = 2;
        n.octaves = 1;
        n.detail = 0.25;
        CHECK(effectNoise(m, allFaces(m), n));
        const int fine = m.aliveFaceCount();
        Vec3 mn0, mx0;
        bool any;
        m.bounds(mn0, mx0, any);
        DecimateParams p;
        p.mode = DecimateParams::Density;
        p.density = 2;
        p.mergeFlat = false;
        const int wanted = (int)std::ceil(totalArea(m) * p.density);
        CHECK(effectDecimate(m, allFaces(m), p));
        CHECK(fine > 1000);
        CHECK(m.aliveFaceCount() <= wanted + 1);
        CHECK(m.aliveFaceCount() > 50);
        // The outline keeps its rectangle.
        Vec3 mn, mx;
        m.bounds(mn, mx, any);
        CHECK(near(mn.x, 0) && near(mn.y, 0) && near(mx.x, 10) && near(mx.y, 10));
        CHECK(near(mx.z, mx0.z, 0.15));
    });
    test("effects: decimate leaves neighbours connected", [&] {
        Mesh m;
        cube(m);
        std::set<Id> sides;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && std::fabs(m.faces[f].normal.z) < 0.5) sides.insert(f);
        CHECK(effectSubdivide(m, sides, 8));
        DecimateParams p;
        p.ratio = 0.1;
        std::set<Id> newSides;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && std::fabs(m.faces[f].normal.z) < 0.5) newSides.insert(f);
        CHECK(effectDecimate(m, newSides, p));
        CHECK(isSolid(m) && near(solidVolume(m), 1.0));
    });
    auto cellCounts = [](const Mesh& m) {
        std::map<int, int> c;
        for (const Face& F : m.faces)
            if (F.alive) ++c[(int)F.outer.size()];
        return c;
    };
    test("effects: remesh a plate into a real pattern", [&] {
        for (auto shape : {RemeshParams::Quads, RemeshParams::Hexagons, RemeshParams::Triangles}) {
            Mesh m;
            plate(m);
            RemeshParams p;
            p.shape = shape;
            p.cellsPerMeter = 2;
            EffectResult r;
            CHECK(effectRemesh(m, allFaces(m), p, &r));
            CHECK(worstWarp(m) < 1e-6);
            CHECK(near(totalArea(m), 100.0, 1e-6));
            const int want = shape == RemeshParams::Quads ? 4 : shape == RemeshParams::Hexagons ? 6 : 3;
            auto c = cellCounts(m);
            // Cell area: 0.25 m2 for quads, 0.2165 hexagons, 0.108 triangles.
            const double area = shape == RemeshParams::Quads ? 0.25 : shape == RemeshParams::Hexagons ? 0.2165 : 0.10825;
            const int expected = int(100 / area);
            CHECK(m.aliveFaceCount() > expected * 0.85 && m.aliveFaceCount() < expected * 1.15);
            // Nearly all whole cells have the right number of sides; the rest are at the outline.
            CHECK(c[want] > expected * 0.75);
            // Interior cells are regular: the right size.
            int exact = 0;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && (int)m.faces[f].outer.size() == want && near(m.faceArea(f), area, 1e-3)) ++exact;
            CHECK(exact > expected * 0.7);
            for (Id f : r.faces) CHECK(f >= 0 && m.faces[f].alive);
        }
    });
    test("effects: remesh a cube stays closed", [&] {
        for (auto shape : {RemeshParams::Quads, RemeshParams::Hexagons, RemeshParams::Triangles}) {
            Mesh m;
            cube(m);
            RemeshParams p;
            p.shape = shape;
            p.cellsPerMeter = 3;
            CHECK(effectRemesh(m, allFaces(m), p));
            std::string why;
            CHECK(isSolid(m, &why));
            CHECK(near(solidVolume(m), 1.0, 1e-6));
            CHECK(worstWarp(m) < 1e-6);
            CHECK(m.aliveFaceCount() > 30);
        }
    });
    test("effects: remesh a curved surface and a partial selection", [&] {
        Mesh m;
        plate(m);
        NoiseParams n;
        n.x = n.y = false;
        n.amount = 0.5;
        n.scale = 4;
        n.octaves = 1;
        CHECK(effectNoise(m, allFaces(m), n));
        RemeshParams p;
        p.shape = RemeshParams::Hexagons;
        p.cellsPerMeter = 1;
        CHECK(effectRemesh(m, allFaces(m), p));
        CHECK(cellCounts(m)[6] > 50);
        CHECK(worstWarp(m) < 0.25); // gently warped, not torn

        Mesh c;
        cube(c);
        Id top = kNone;
        for (Id f = 0; f < (Id)c.faces.size(); ++f)
            if (c.faces[f].alive && c.faces[f].normal.z > 0.9) top = f;
        p.cellsPerMeter = 5;
        CHECK(effectRemesh(c, {top}, p));
        CHECK(isSolid(c));
        CHECK(near(solidVolume(c), 1.0, 1e-6));
        CHECK(worstWarp(c) < 1e-6);
    });
    test("effects: materials and painted weights carry over", [&] {
        Mesh m;
        plate(m, 2);
        Id f = onlyFace(m);
        m.faces[f].front.material = 3;
        m.faces[f].back.material = 5;
        for (Vertex& v : m.vertices)
            if (v.alive) v.paint = {{4, 0.5f}};
        CHECK(effectSubdivide(m, allFaces(m), 2));
        for (const Face& F : m.faces)
            if (F.alive) CHECK(F.front.material == 3 && F.back.material == 5 && F.front.axis.length2() > 0.5);
        for (const Vertex& v : m.vertices)
            if (v.alive) CHECK(v.paint.size() == 1 && v.paint[0].material == 4 && near(v.paint[0].weight, 0.5));
    });
    test("effects: an absurd density is limited, not hung", [&] {
        Mesh m;
        plate(m, 1000);
        EffectResult r;
        CHECK(effectSubdivide(m, allFaces(m), 1000, &r));
        CHECK(r.limited);
        CHECK(m.aliveFaceCount() <= kEffectTriangleLimit + 1000);
    });
    test("effects: components get their own copy, in world coordinates", [&] {
        Document d;
        cube(d.mesh());
        Selection all;
        all.faces = allFaces(d.mesh());
        Id a = makeComponent(d.state, all, "Box");
        Id b = cloneInstance(d.state, a);
        d.state.instances[b].xf = Mat4::translation(Vec3(5, 0, 0));
        d.state.instances[a].xf = Mat4::translation(Vec3(0, 0, 3));
        const Id oldDef = d.state.instances[b].def;
        const int defFaces = d.state.defs[oldDef].mesh.aliveFaceCount();
        Selection sel;
        sel.instances.insert(a);
        CHECK_EQ(effectFaceCount(d.state, sel), 6);
        bool limited = false;
        std::string err;
        NoiseParams p;
        p.x = p.y = false;
        p.amount = 0.1;
        CHECK(applyEffect(d.state, sel, [&](Mesh& m, const std::set<Id>& f, EffectResult* r, std::string* e) {
            return effectNoise(m, f, p, r, e);
        }, &limited, &err));
        CHECK(d.state.instances[a].def != oldDef);
        CHECK_EQ(d.state.instances[b].def, oldDef);
        CHECK_EQ(d.state.defs[oldDef].mesh.aliveFaceCount(), defFaces);
        CHECK(d.state.defs[d.state.instances[a].def].mesh.aliveFaceCount() > defFaces);
        CHECK(d.state.editing == kNone);
        Vec3 mn, mx;
        CHECK(instanceBounds(d.state, a, mn, mx));
        CHECK(near(mn.x, 0, 1e-6) && near(mn.z, 3, 0.11) && near(mx.z, 4, 0.11));
    });

    // ---- walking --------------------------------------------------------------
    // A floor, as a large face facing up.
    auto floorMesh = [](Mesh& m, double s = 30) {
        m.groundFacesDown = false;
        m.viewHint = Vec3(0, 0, 10);
        rect(m, -s, -s, s, s, 0.5);
    };
    auto walkFor = [](Walker& w, double seconds, double fwd, double strafe = 0, bool jump = false, bool run = false) {
        WalkInput in;
        in.forward = fwd;
        in.strafe = strafe;
        in.jump = jump;
        in.run = run;
        w.advance(seconds, in);
    };
    auto worldOf = [](const Mesh& m, CollisionWorld& world) {
        ModelState st;
        st.mesh = m;
        world.buildFromModel(st);
    };
    test("walker: capsule is 1.80 m by 0.30 m and lands on a floor", [&] {
        WalkerParams defaults;
        CHECK(near(defaults.height, 1.80) && near(defaults.radius * 2, 0.30));
        Mesh m;
        floorMesh(m);
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 3));
        CHECK(!w.grounded());
        walkFor(w, 2, 0);
        CHECK(w.grounded());
        CHECK(near(w.feet().z, 0.5, 0.01));
        CHECK(near(w.eye().z - w.feet().z, defaults.eyeHeight, 1e-2));
        CHECK(w.overlaps(Vec3(0, 0, 0.0)));
        CHECK(!w.overlaps(w.feet() + Vec3(0, 0, 0.01)));
    });
    test("walker: W moves where it looks, A/D sideways, at walking speed", [&] {
        Mesh m;
        floorMesh(m);
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 0.5));
        w.setYaw(0);
        walkFor(w, 0.5, 0);
        Vec3 s = w.feet();
        walkFor(w, 2, 1);
        Vec3 d = w.feet() - s;
        CHECK(d.x > 5 && d.x < 6.1 && std::fabs(d.y) < 1e-6 && near(d.z, 0, 0.01));
        walkFor(w, 0.5, 0);
        s = w.feet();
        walkFor(w, 1, 0, 1); // D: to the right of +X is -Y
        d = w.feet() - s;
        CHECK(d.y < -2 && std::fabs(d.x) < 1e-6);
        w.look(kPi / 2, 0); // now looking along +Y
        walkFor(w, 0.5, 0);
        s = w.feet();
        walkFor(w, 1, 1);
        d = w.feet() - s;
        CHECK(d.y > 2 && std::fabs(d.x) < 1e-6);
        walkFor(w, 0.5, 0);
        s = w.feet();
        walkFor(w, 1, 1, 0, false, true);
        CHECK((w.feet() - s).length() > 4.5);
    });
    test("walker: a wall stops it at its radius, also a thin one-sided plane", [&] {
        Mesh m;
        floorMesh(m);
        m.drawPolyline({{3, -10, 0.5}, {3, 10, 0.5}, {3, 10, 4}, {3, -10, 4}}, true);
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 0.5));
        walkFor(w, 3, 1, 0, false, true);
        CHECK(w.feet().x < 3 - 0.149 && w.feet().x > 2.8);
        // Sliding along the wall.
        const double y0 = w.feet().y;
        w.look(0.5, 0);
        walkFor(w, 1, 1);
        CHECK(w.feet().y > y0 + 0.5);
        CHECK(w.feet().x < 3);
        // From the other side as well.
        Walker other(world);
        other.spawn(Vec3(6, 0, 0.5));
        other.setYaw(kPi);
        walkFor(other, 3, 1, 0, false, true);
        CHECK(other.feet().x > 3 + 0.149);
    });
    test("walker: fast falls do not pass through a thin floor", [&] {
        Mesh m;
        floorMesh(m);
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 200));
        walkFor(w, 8, 0);
        CHECK(near(w.feet().z, 0.5, 0.01));
        CHECK(w.grounded());
    });
    test("walker: climbs steps but not walls, jumps up a little more", [&] {
        Mesh m;
        floorMesh(m);
        // Stairs of 0.2 m along +X, starting at x = 2.
        double z = 0.5;
        for (int i = 0; i < 5; ++i) {
            solidBox(m, Vec3(2 + i * 0.3, -3, z), Vec3(2.0 + 1.5, 3, z + 0.2));
            z += 0.2;
        }
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 0.5));
        walkFor(w, 1.15, 1); // five steps up, at the top of the stairs
        CHECK(w.feet().z > 0.5 + 0.9);
        CHECK(w.grounded());

        Mesh tall;
        floorMesh(tall);
        solidBox(tall, Vec3(2, -3, 0.5), Vec3(4, 3, 1.5));
        CollisionWorld tallWorld;
        worldOf(tall, tallWorld);
        Walker blocked(tallWorld);
        blocked.spawn(Vec3(0, 0, 0.5));
        walkFor(blocked, 2, 1);
        CHECK(blocked.feet().x < 2 - 0.14 && near(blocked.feet().z, 0.5, 0.01));

        // A 0.6 m ledge: too high to step, reachable with a jump.
        Mesh ledge;
        floorMesh(ledge);
        solidBox(ledge, Vec3(2, -3, 0.5), Vec3(4, 3, 1.1));
        CollisionWorld ledgeWorld;
        worldOf(ledge, ledgeWorld);
        Walker j(ledgeWorld);
        j.spawn(Vec3(0, 0, 0.5));
        walkFor(j, 2, 1);
        CHECK(near(j.feet().z, 0.5, 0.01));
        walkFor(j, 0.05, 1, 0, true);
        for (int i = 0; i < 300 && (i < 5 || !j.grounded()); ++i) walkFor(j, 0.01, 1);
        CHECK(j.feet().x > 2 && near(j.feet().z, 1.1, 0.02));
    });
    test("walker: walks slopes up to the limit and follows them down", [&] {
        auto ramp = [](Mesh& m, double degrees) {
            const double run = 4 / std::tan(degrees * kPi / 180);
            m.groundFacesDown = false;
            m.viewHint = Vec3(0, 0, 100);
            m.drawPolyline({{1, -5, 0.5}, {1 + run, -5, 4.5}, {1 + run, 5, 4.5}, {1, 5, 0.5}}, true);
        };
        Mesh gentle;
        floorMesh(gentle);
        ramp(gentle, 30);
        CollisionWorld gw;
        worldOf(gentle, gw);
        Walker up(gw);
        up.spawn(Vec3(0, 0, 0.5));
        walkFor(up, 2.5, 1);
        CHECK(up.feet().z > 2.0);
        CHECK(up.grounded());
        // Standing on the slope it does not creep.
        walkFor(up, 0.3, 0);
        const Vec3 rest = up.feet();
        walkFor(up, 3, 0);
        CHECK((up.feet() - rest).length() < 1e-3);
        // Downhill it stays on the ground instead of hopping.
        up.setYaw(kPi);
        bool alwaysGrounded = true;
        for (int i = 0; i < 40; ++i) {
            walkFor(up, 0.02, 1);
            alwaysGrounded = alwaysGrounded && up.grounded();
        }
        CHECK(alwaysGrounded);

        Mesh steep;
        floorMesh(steep);
        ramp(steep, 70);
        CollisionWorld sw;
        worldOf(steep, sw);
        Walker no(sw);
        no.spawn(Vec3(0, 0, 0.5));
        walkFor(no, 4, 1);
        CHECK(no.feet().z < 1.2);
    });
    test("walker: walks off an edge and falls; floor below everything", [&] {
        Mesh m;
        solidBox(m, Vec3(-2, -2, 3), Vec3(2, 2, 4));
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 4));
        walkFor(w, 0.2, 0);
        CHECK(w.grounded() && near(w.feet().z, 4, 0.01));
        walkFor(w, 3, 1);
        CHECK(w.feet().x > 2);
        CHECK(near(w.feet().z, 0, 0.01)); // landed on the ground plane
        CHECK(w.grounded());
        // Empty models still have the ground.
        CollisionWorld empty;
        empty.build();
        Walker e(empty);
        e.spawn(Vec3(5, 5, 1));
        walkFor(e, 2, 1);
        CHECK(near(e.feet().z, 0, 0.01));

        Mesh pit;
        solidBox(pit, Vec3(-2, -2, -5), Vec3(2, 2, -4));
        CollisionWorld pw;
        worldOf(pit, pw);
        CHECK(near(pw.floorZ(), -5));
    });
    test("walker: head hits a ceiling; a low gap keeps it out", [&] {
        Mesh m;
        floorMesh(m);
        // A ceiling plate 2.1 m above the floor.
        solidBox(m, Vec3(-5, -5, 2.6), Vec3(5, 5, 3));
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(0, 0, 0.5));
        walkFor(w, 0.2, 0);
        walkFor(w, 0.05, 0, 0, true);
        double top = 0;
        for (int i = 0; i < 60; ++i) {
            walkFor(w, 0.01, 0);
            top = std::max(top, w.feet().z + w.params().height);
        }
        CHECK(top <= 2.6 + 1e-3);
        walkFor(w, 1, 0);
        CHECK(w.grounded() && near(w.feet().z, 0.5, 0.01));

        Mesh gap;
        floorMesh(gap);
        solidBox(gap, Vec3(2, -5, 2.0), Vec3(4, 5, 3)); // only 1.5 m of room under it
        CollisionWorld gw;
        worldOf(gap, gw);
        Walker g(gw);
        g.spawn(Vec3(0, 0, 0.5));
        walkFor(g, 3, 1);
        CHECK(g.feet().x < 2);
    });
    test("walker: collides with moved component instances, not hidden ones", [&] {
        Document d;
        solidBox(d.mesh(), Vec3(0, 0, 0), Vec3(1, 1, 3));
        Selection all;
        all.faces = allFaces(d.mesh());
        Id a = makeComponent(d.state, all, "Post");
        Id b = cloneInstance(d.state, a);
        d.state.instances[a].xf = Mat4::translation(Vec3(5, -0.5, 0));
        d.state.instances[b].xf = Mat4::translation(Vec3(-6, -0.5, 0));
        d.state.instances[b].hidden = true;
        CollisionWorld world;
        world.buildFromModel(d.state);
        Walker w(world);
        w.spawn(Vec3(0, 0, 0));
        walkFor(w, 3, 1);
        CHECK(w.feet().x < 5 - 0.14);
        Walker back(world);
        back.spawn(Vec3(0, 0, 0));
        back.setYaw(kPi);
        walkFor(back, 3, 1);
        CHECK(back.feet().x < -7);
    });
    test("walker: terrain from the noise effect is walkable", [&] {
        Mesh m;
        floorMesh(m, 15);
        NoiseParams n;
        n.x = n.y = false;
        n.amount = 0.3;
        n.scale = 4;
        n.octaves = 2;
        n.detail = 0.5;
        CHECK(effectNoise(m, allFaces(m), n));
        CollisionWorld world;
        worldOf(m, world);
        Walker w(world);
        w.spawn(Vec3(-10, 0, 2));
        walkFor(w, 1, 0);
        CHECK(w.grounded());
        walkFor(w, 5, 1);
        CHECK(w.feet().x > 0);
        CHECK(w.feet().z > 0.5 - 0.4 && w.feet().z < 0.5 + 0.4);
    });

    // ---- bend -----------------------------------------------------------------
    // Straight stairs along +X, 1 m wide along Y, as one closed solid.
    auto stairs = [&](Mesh& m, int steps, double run, double rise, double width = 1.0) {
        m.groundFacesDown = false;
        m.viewHint = Vec3(0, -10, 2);
        std::vector<Vec3> p{{0, 0, 0}};
        for (int i = 0; i < steps; ++i) {
            p.push_back({i * run, 0, (i + 1) * rise});
            p.push_back({(i + 1) * run, 0, (i + 1) * rise});
        }
        p.push_back({steps * run, 0, 0});
        m.drawPolyline(p, true);
        Id f = onlyFace(m);
        m.pushPull(f, width * (m.faces[f].normal.y > 0 ? 1 : -1));
    };
    test("bend: the mapping keeps length, stays put before, rigid after", [&] {
        BendParams b;
        CHECK(makeBend(Vec3(1, 0, 0), Vec3(5, 0, 7), Vec3(0, 0, 1), kPi / 2, 0, b));
        CHECK(near(b.length, 4) && near(b.dir.x, 1) && near(b.dir.z, 0));
        CHECK_EQ(b.segmentCount(), 12);
        CHECK(b.apply(Vec3(0.5, 3, 2)).nearlyEquals(Vec3(0.5, 3, 2)));
        // A quarter circle turning +X towards +Y, radius 4/(pi/2).
        const double R = 8 / kPi;
        CHECK(b.apply(Vec3(5, 0, 2)).nearlyEquals(Vec3(1 + R, R, 2), 1e-9));
        CHECK(b.apply(Vec3(3, 0, 0)).nearlyEquals(Vec3(1 + R * std::sin(kPi / 4), R - R * std::cos(kPi / 4), 0), 1e-9));
        // Past the end: straight on along +Y.
        CHECK(b.apply(Vec3(6, 0, 0)).nearlyEquals(Vec3(1 + R, R + 1, 0), 1e-9));
        // Across: the outside of the turn gets longer, the inside shorter.
        CHECK(b.apply(Vec3(5, -1, 0)).nearlyEquals(Vec3(1 + R + 1, R, 0), 1e-9));
        // Negative angles turn the other way; no angle changes nothing.
        b.angle = -kPi / 2;
        CHECK(b.apply(Vec3(5, 0, 0)).nearlyEquals(Vec3(1 + R, -R, 0), 1e-9));
        b.angle = 0;
        CHECK(b.apply(Vec3(5, 2, 9)).nearlyEquals(Vec3(5, 2, 9)));
        CHECK(!makeBend(Vec3(0, 0, 0), Vec3(0, 0, 5), Vec3(0, 0, 1), 1, 0, b));
    });
    test("bend: straight stairs become a quarter-circle staircase", [&] {
        Mesh m;
        const int steps = 12;
        const double run = 0.3, rise = 0.18;
        stairs(m, steps, run, rise);
        CHECK(isSolid(m));
        const double v0 = solidVolume(m);
        // The stretch is the stairs' run, slanting up with them; the arc is 3.6 m long.
        BendParams b;
        CHECK(makeBend(Vec3(0, 0, 0), Vec3(steps * run, 0, steps * rise), Vec3(0, 0, 1), kPi / 2, 0, b));
        EffectResult r;
        std::string why;
        CHECK(effectBend(m, allFaces(m), nullptr, b, &r, &why));
        CHECK(isSolid(m, &why));
        // Pappus: the stairs lie on the inside of the turn, 0.5 m from the line
        // bent, whose length is kept. Straight arc pieces take off a little more.
        const double R0 = steps * run / (kPi / 2);
        const double expected = v0 * (1 - 0.5 / R0);
        CHECK(std::fabs(solidVolume(m) - expected) / expected < 0.01);
        CHECK(!r.faces.empty());
        // Every tread stays one single horizontal face at its own height.
        std::map<int, int> treads;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive || m.faces[f].normal.z < 0.999) continue;
            ++treads[(int)std::lround(m.facePlane(f).d / rise)];
        }
        CHECK_EQ((int)treads.size(), steps);
        for (const auto& [level, count] : treads) CHECK_EQ(count, 1);
        CHECK(worstWarp(m) < 1e-6);
        // Height is unchanged; the top end now faces along +Y, on the quarter circle.
        Vec3 mn, mx;
        bool any;
        m.bounds(mn, mx, any);
        CHECK(near(mx.z, steps * rise) && near(mn.z, 0));
        const double R = steps * run / (kPi / 2);
        CHECK(m.findVertex(Vec3(R, R, steps * rise), 1e-6) != kNone);
        CHECK(m.findVertex(Vec3(R, R, 0), 1e-6) != kNone);
        CHECK(m.findVertex(Vec3(0, 0, 0), 1e-6) != kNone);
        // All vertices of the y = 0 side lie on the circle: radius R around (0, R),
        // and those of the y = 1 side on the circle inside it.
        for (const Vertex& v : m.vertices) {
            if (!v.alive) continue;
            const double fromCentre = (Vec3(v.p.x, v.p.y, 0) - Vec3(0, R, 0)).length();
            CHECK(near(fromCentre, R, 1e-6) || near(fromCentre, R - 1, 1e-6));
        }
        // Sides curve smoothly: no hard edges between their strips, a tread's edges stay.
        int softEdges = 0;
        for (const Edge& E : m.edges)
            if (E.alive && E.soft) ++softEdges;
        CHECK(softEdges > 12);
        // Far fewer polygons than triangles: flat pieces merged.
        CHECK(m.aliveFaceCount() < 150);
    });
    test("bend: the bent stairs are walkable", [&] {
        Mesh m;
        const int steps = 12;
        const double run = 0.3, rise = 0.18;
        stairs(m, steps, run, rise, 1.5);
        BendParams b;
        CHECK(makeBend(Vec3(0, 0, 0), Vec3(steps * run, 0, 0), Vec3(0, 0, 1), kPi / 2, 0, b));
        CHECK(effectBend(m, allFaces(m), nullptr, b));
        CollisionWorld world;
        ModelState st;
        st.mesh = m;
        world.buildFromModel(st);
        // Starts in front of the stairs and walks the middle of the curve (y = 0.75 straight,
        // the side of the turn away from the centre is -Y).
        Walker w(world);
        w.spawn(Vec3(-1, 0.75, 0));
        w.setYaw(0);
        const double R = b.radius() - 0.75;
        WalkInput in;
        in.forward = 1;
        for (int i = 0; i < 500; ++i) {
            const Vec3 q = w.feet();
            // Tangent of the circle around (0, b.radius()) at this point.
            if (q.x > 0) {
                const double a = std::atan2(q.x, b.radius() - q.y);
                if (a < kPi / 2) w.setYaw(a);
                else w.setYaw(kPi / 2);
            }
            w.advance(0.01, in);
            if (q.z > steps * rise - 0.01) break;
        }
        CHECK(w.feet().z > steps * rise - 0.05);
        CHECK(R > 0);
    });
    test("bend: selection, neighbours, components, loose lines, limits", [&] {
        // Only part of a box: the rest stays and all stays connected.
        Mesh box;
        solidBox(box, Vec3(0, 0, 0), Vec3(4, 1, 1));
        std::set<Id> top;
        for (Id f = 0; f < (Id)box.faces.size(); ++f)
            if (box.faces[f].alive && box.faces[f].normal.z > 0.9) top.insert(f);
        BendParams b;
        CHECK(makeBend(Vec3(1, 0, 1), Vec3(3, 0, 1), Vec3(0, 1, 0), 0.3, 0, b));
        CHECK(effectBend(box, top, nullptr, b));
        CHECK(isSolid(box));

        // Around a horizontal axis: a beam bent upwards stays closed, its end rises.
        Mesh beam;
        solidBox(beam, Vec3(0, 0, 0), Vec3(4, 1, 1));
        BendParams up;
        CHECK(makeBend(Vec3(1, 0, 0), Vec3(3, 0, 0), Vec3(0, 1, 0), kPi / 3, 0, up));
        const Vec3 tip = up.apply(Vec3(4, 0, 0));
        CHECK(effectBend(beam, allFaces(beam), nullptr, up));
        CHECK(isSolid(beam));
        CHECK(std::fabs(tip.z) > 1);
        CHECK(beam.findVertex(tip, 1e-6) != kNone);
        CHECK(worstWarp(beam) < 1e-6);

        // Too tight: inside the arc's centre it would turn inside out, nothing changes.
        Mesh tight;
        solidBox(tight, Vec3(0, -5, 0), Vec3(1, 5, 1));
        const int faces0 = tight.aliveFaceCount();
        BendParams t;
        CHECK(makeBend(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), kPi, 0, t));
        std::string err;
        CHECK(!effectBend(tight, allFaces(tight), nullptr, t, nullptr, &err));
        CHECK(!err.empty());
        CHECK_EQ(tight.aliveFaceCount(), faces0);

        // Loose line: cut into the arc's segments, ends on the arc.
        Mesh line;
        line.drawLine(Vec3(0, 0, 0), Vec3(4, 0, 0));
        std::set<Id> edges;
        for (Id e = 0; e < (Id)line.edges.size(); ++e)
            if (line.edges[e].alive) edges.insert(e);
        BendParams arc;
        CHECK(makeBend(Vec3(0, 0, 0), Vec3(4, 0, 0), Vec3(0, 0, 1), kPi / 2, 8, arc));
        EffectResult r;
        CHECK(effectBend(line, {}, &edges, arc, &r));
        CHECK_EQ(line.aliveEdgeCount(), 8);
        CHECK_EQ((int)r.edges.size(), 8);
        const double R = arc.radius();
        CHECK(line.findVertex(Vec3(R, R, 0), 1e-6) != kNone);
        for (const Vertex& v : line.vertices)
            if (v.alive) CHECK(near((v.p - Vec3(0, R, 0)).length(), R, 1e-9));

        // Materials stay.
        Mesh mat;
        solidBox(mat, Vec3(0, 0, 0), Vec3(4, 1, 1));
        for (Face& F : mat.faces)
            if (F.alive) F.front.material = 7;
        CHECK(effectBend(mat, allFaces(mat), nullptr, up));
        for (const Face& F : mat.faces)
            if (F.alive) CHECK_EQ(F.front.material, 7);

        // A component with a copy: only the selected one bends.
        Document d;
        stairs(d.mesh(), 4, 0.3, 0.18);
        Selection all;
        all.faces = allFaces(d.mesh());
        Id a = makeComponent(d.state, all, "Stairs");
        Id c = cloneInstance(d.state, a);
        d.state.instances[c].xf = Mat4::translation(Vec3(10, 0, 0));
        const Id shared = d.state.instances[c].def;
        const int defFaces = d.state.defs[shared].mesh.aliveFaceCount();
        Selection sel;
        sel.instances.insert(a);
        BendParams s;
        CHECK(makeBend(Vec3(0, 0, 0), Vec3(1.2, 0, 0), Vec3(0, 0, 1), kPi / 4, 0, s));
        CHECK(applyBend(d.state, sel, s));
        CHECK(d.state.instances[a].def != shared);
        CHECK_EQ(d.state.defs[shared].mesh.aliveFaceCount(), defFaces);
        CHECK(isSolid(d.state.defs[d.state.instances[a].def].mesh));
    });

    // ---- position texture -----------------------------------------------------
    auto uvAt = [](const TexMap& t, const Vec3& p) {
        const Vec3 d = p - t.origin;
        return Vec2(d.dot(t.u), d.dot(t.v));
    };
    auto sameUv = [&](const Vec2& a, const Vec2& b) { return near(a.x, b.x, 1e-9) && near(a.y, b.y, 1e-9); };
    test("texture: explicit map equals the default projection", [&] {
        Mesh m;
        solidBox(m, Vec3(0, 0, 0), Vec3(2, 3, 1));
        Material mat;
        mat.texture.data = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1});
        mat.texture.worldWidth = 0.5;
        mat.texture.worldHeight = 2;
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            if (!m.faces[f].alive) continue;
            for (bool front : {true, false}) {
                const TexMap t = explicitTexMap(m.faces[f], front, &mat);
                for (Id v : m.faceVertices(f)) {
                    const Vec2 want = faceTexCoord(m.faces[f], front, m.pos(v), &mat);
                    CHECK(sameUv(uvAt(t, m.pos(v)), want));
                }
            }
        }
    });
    test("texture: each move shows the image where it should", [&] {
        TexMap base;
        base.valid = true;
        base.origin = Vec3(1, 2, 0);
        base.u = Vec3(2, 0, 0);
        base.v = Vec3(0, 0.5, 0);
        const Vec3 p(3, 5, 0);
        auto check = [&](const UvAffine& moved) {
            TexMap t = base;
            CHECK(moveTexMap(t, moved));
            // What showed at uv now shows at the place moved(uv) occupied.
            for (Vec2 uv : {Vec2(0, 0), Vec2(1, 0), Vec2(0.3, 0.7), Vec2(-2, 5)}) {
                const Vec2 to = moved.apply(uv);
                const Vec3 world = base.origin + Vec3(to.x / 2, to.y / 0.5, 0);
                CHECK(sameUv(uvAt(t, world), uv));
            }
            (void)p;
        };
        check(UvAffine::translate(0.25, -3));
        check(UvAffine::scaleAbout(Vec2(1, 1), 2, 0.5));
        check(UvAffine::scaleAbout(Vec2(0, 0), -1, 1)); // mirrored
        check(UvAffine::shearAbout(Vec2(0.5, 0), 0.8, true));
        check(UvAffine::shearAbout(Vec2(0, 0.5), -0.4, false));
        // Undoes itself.
        const UvAffine a = UvAffine::shearAbout(Vec2(1, 2), 0.3, true);
        TexMap t = base;
        CHECK(moveTexMap(t, a));
        CHECK(moveTexMap(t, a.inverse()));
        for (Vec3 q : {Vec3(0, 0, 0), Vec3(4, 1, 0)}) CHECK(sameUv(uvAt(t, q), uvAt(base, q)));
        // Collapsing is refused; the map stays.
        t = base;
        CHECK(!moveTexMap(t, UvAffine::scaleAbout(Vec2(0, 0), 0, 1)));
        CHECK(t.u.nearlyEquals(base.u));
    });
    test("texture: rotation turns in the world whatever the proportions", [&] {
        TexMap base; // a tile 4 m wide and 1 m high, on the ground
        base.valid = true;
        base.u = Vec3(0.25, 0, 0);
        base.v = Vec3(0, 1, 0);
        PlaneTexMap plane;
        CHECK(plane.set(base, Vec3(0, 0, 1), Vec3(1, 1, 0)));
        CHECK(near(plane.lengthU(), 4) && near(plane.lengthV(), 1));
        const Vec3 q(3, -2, 0);
        CHECK(plane.pointAt(plane.uvAt(q)).nearlyEquals(q, 1e-9));
        const Vec2 c(0.5, 0.5); // world (2, 0.5)
        const UvAffine r = plane.rotationAbout(c, kPi / 2);
        // The tile's right middle, 2 m east of the centre, lands 2 m north of it.
        const Vec2 to = r.apply(Vec2(1, 0.5));
        CHECK(plane.pointAt(to).nearlyEquals(Vec3(2, 2.5, 0), 1e-9));
        // Lengths stay, right angles stay, in the world.
        TexMap t = base;
        CHECK(moveTexMap(t, r));
        CHECK(near(t.u.dot(t.v), 0, 1e-9));
        CHECK(near(1 / t.u.length(), 4, 1e-9) && near(1 / t.v.length(), 1, 1e-9));
        CHECK(near(plane.angleOf(c, Vec3(2, 5, 0)), kPi / 2, 1e-9));
    });

    test("materials: map names", [] {
        CHECK(guessMapSlot("oak_Roughness_2k.png") == MapRoughness);
        CHECK(guessMapSlot("/tmp/wood_floor/Wood-Normal.jpg") == MapNormal);
        CHECK(guessMapSlot("woodNormalGL.png") == MapNormal);
        CHECK(guessMapSlot("brick_albedo.png") == MapSlotCount);
        CHECK(guessMapSlot("rock_AO.tga") == MapOcclusion);
        CHECK(guessMapSlot("metal_plate_Metalness.png") == MapMetallic);
        CHECK(guessMapSlot("x_disp.png") == MapHeight);
        CHECK(guessMapSlot("holiday.jpg") == -1);
        CHECK(guessMapSlot("broadcast.png") == -1); // "ao" and "col" only count as whole words
    });

    test("materials: shader properties", [] {
        const auto p = parseShaderProps(
            "uniform float _Speed;  // range(0, 10) default(2.5)\n"
            "  uniform vec3 _Tint; // color default(1, 0.5, 0.25)\r\n"
            "uniform vec2 _Offset;\n"
            "uniform sampler2D _Mask;\n"
            "uniform float _Bad; // range(a,b) default(zz)\n"
            "void surface(inout Surface s) { float uniform_like = 1.0; }\n");
        CHECK(p.size() == 4);
        CHECK(p[0].name == "_Speed" && p[0].components == 1 && p[0].max == 10 && p[0].def[0] == 2.5f);
        CHECK(p[1].name == "_Tint" && p[1].isColor && p[1].components == 3 && p[1].def[1] == 0.5f);
        CHECK(p[2].name == "_Offset" && p[2].components == 2 && !p[2].isColor);
        CHECK(p[3].name == "_Bad" && p[3].max > p[3].min);
    });

    test("materials: advanced flag, tile size, comparison", [] {
        Material m;
        CHECK(!m.advanced() && !m.blended());
        double w, h;
        m.tileSize(w, h);
        CHECK(w == 1 && h == 1);
        auto bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
        m.maps[MapNormal].tex.data = bytes;
        m.maps[MapNormal].tex.worldWidth = 2;
        m.maps[MapNormal].tex.worldHeight = 3;
        CHECK(m.advanced());
        m.tileSize(w, h);
        CHECK(w == 2 && h == 3); // no base image: the map decides
        m.setTileSize(4, 5);
        m.tileSize(w, h);
        CHECK(w == 4 && h == 5);
        Material a = m;
        a.maps[MapNormal].tex.data = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
        CHECK(sameMaterial(a, m)); // equal bytes in another buffer
        a.maps[MapNormal].strength = 0.5f;
        CHECK(!sameMaterial(a, m));
        Material b = m;
        b.maps[MapOpacity].tex.data = bytes;
        CHECK(b.blended() && !sameMaterial(b, m));
        Material sh;
        sh.shader = "x";
        CHECK(!sh.advanced() && !sh.customShader()); // written but switched off
        sh.shaderOn = true;
        CHECK(sh.advanced() && sh.customShader());
    });

    test("scene objects: names and selection", [] {
        Document d;
        SceneObject o;
        Id a = d.addObject(o), b = d.addObject(o);
        CHECK(d.state.objects[a].name == "Point Light" && d.state.objects[b].name == "Point Light 2");
        o.kind = SceneObject::ViewCamera;
        CHECK(d.state.objects[d.addObject(o)].name == "Camera");
        d.selection.objects = {a, b, 99};
        CHECK(d.selection.count() == 3 && !d.selection.empty());
        d.beginOp("Erase");
        d.state.objects[a].alive = false;
        d.commitOp();
        CHECK(d.selection.objects == std::set<Id>{b}); // the deleted and the invalid are gone
        d.undo();
        CHECK(d.state.objects[a].alive);
    });

    test("components: extract and import a definition", [] {
        Document d;
        Mesh& m = d.mesh();
        m.drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, true);
        Material unused, used;
        unused.name = "Unused";
        used.name = "Used";
        used.pbr = true;
        d.addMaterial(unused);
        const Id mid = d.addMaterial(used);
        Selection s;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) {
                m.faces[f].front.material = mid;
                s.faces.insert(f);
            }
        const Id inst = makeComponent(d.state, s, "Tile");
        const Id def = d.state.instances[inst].def;
        ModelState pre = extractDefinition(d.state, def);
        CHECK(pre.defs.size() == 1 && pre.instances.size() == 1 && pre.materials.size() == 1);
        CHECK(pre.materials[0].name == "Used");
        bool mapped = false;
        for (const Face& f : pre.defs[0].mesh.faces) mapped = mapped || (f.alive && f.front.material == 0);
        CHECK(mapped);
        CHECK(pre.instances[0].xf.isIdentity());

        Document other;
        Material first;
        first.name = "Something else";
        other.addMaterial(first);
        const Id d0 = importDefinition(other, pre, 0);
        CHECK(d0 == 0 && other.state.materials.size() == 2);
        bool remapped = false;
        for (const Face& f : other.state.defs[0].mesh.faces) remapped = remapped || (f.alive && f.front.material == 1);
        CHECK(remapped);
        // Placing it again reuses the definition and its materials.
        CHECK(importDefinition(other, pre, 0) == d0);
        CHECK(other.state.defs.size() == 1 && other.state.materials.size() == 2);
        // With a different identity the equal material is still merged.
        ModelState variant = pre;
        variant.defs[0].uid = "another";
        CHECK(importDefinition(other, variant, 0) == 1);
        CHECK(other.state.materials.size() == 2);
        CHECK(importDefinition(other, pre, 5) == kNone);
    });

    test("animation: rigid fit and screw motion", [&] {
        // A door 1 m wide hinged on the Z axis through (2, 3): a quarter turn.
        const std::vector<Vec3> rest = {{2, 3, 0}, {3, 3, 0}, {3, 3, 2}, {2, 3, 2}};
        std::vector<Vec3> open = {{2, 3, 0}, {2, 4, 0}, {2, 4, 2}, {2, 3, 2}};
        double residual = 1;
        const Mat4 fit = fitRigid(rest, open, &residual);
        CHECK(residual < 1e-9);
        for (size_t i = 0; i < rest.size(); ++i) CHECK(fit.transformPoint(rest[i]).nearlyEquals(open[i], 1e-9));
        // Half way the free edge is on the arc, not on the straight line between.
        const Mat4 half = interpolateRigid(Mat4(), fit, 0.5);
        const Vec3 edge = half.transformPoint(Vec3(3, 3, 1));
        CHECK(near((edge - Vec3(2, 3, 1)).length(), 1.0, 1e-9));
        CHECK(edge.nearlyEquals(Vec3(2 + std::sqrt(0.5), 3 + std::sqrt(0.5), 1), 1e-9));
        CHECK(half.transformPoint(Vec3(2, 3, 1)).nearlyEquals(Vec3(2, 3, 1), 1e-9)); // the hinge stays put
        CHECK(interpolateRigid(Mat4(), fit, 0).isIdentity());
        CHECK(interpolateRigid(Mat4(), fit, 1).transformPoint(rest[1]).nearlyEquals(open[1], 1e-9));
        // Between two non-identity poses.
        const Mat4 from = Mat4::translation(Vec3(5, 0, 0));
        CHECK(interpolateRigid(from, Mat4::translation(Vec3(5, 0, 4)), 0.25).transformPoint(Vec3()).nearlyEquals(Vec3(5, 0, 1), 1e-9));
        // A half turn is handled, and a turn plus a slide.
        const Mat4 turn = Mat4::translation(Vec3(1, 0, 3)) * Mat4::rotation(Vec3(0, 0, 1), kPi);
        const Mat4 end = interpolateRigid(Mat4(), turn, 1);
        CHECK(end.transformPoint(Vec3(2, 5, 1)).nearlyEquals(turn.transformPoint(Vec3(2, 5, 1)), 1e-6));
        const Vec3 mid = interpolateRigid(Mat4(), turn, 0.5).transformPoint(Vec3(0.5, 0, 0));
        CHECK(near(mid.z, 1.5, 1e-6)); // half the slide
        // A scaled shape is no rigid motion.
        std::vector<Vec3> big;
        for (const Vec3& p : rest) big.push_back(p * 2.0);
        fitRigid(rest, big, &residual);
        CHECK(residual > 0.1);
        // Mirror-free fit of a flat, three-point set still works.
        const std::vector<Vec3> tri = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        const Mat4 q = Mat4::rotation(Vec3(1, 0, 0), 0.7);
        std::vector<Vec3> tri2;
        for (const Vec3& p : tri) tri2.push_back(q.transformPoint(p));
        fitRigid(tri, tri2, &residual);
        CHECK(residual < 1e-9);
    });

    test("animation: states of a component", [&] {
        Document d;
        Mesh& m = d.mesh();
        m.drawPolyline({{1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}}, true);
        Selection s;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) s.faces.insert(f);
        const Id inst = makeComponent(d.state, s, "Door");
        ModelState& st = d.state;
        const Id def = st.instances[inst].def;
        CHECK(!recordState(st, "CLOSED")); // nothing open
        d.state.instances[inst].xf = Mat4::translation(Vec3(10, 0, 0)) * d.state.instances[inst].xf;
        d.openComponent(inst);
        CHECK(recordState(st, "CLOSED"));
        CHECK(st.defs[def].states.size() == 1);
        // States are in the definition's own coordinates, not the world's.
        Vec3 mn(1e9, 1e9, 1e9);
        for (const Vec3& p : st.defs[def].states[0].pos) mn = Vec3(std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z));
        CHECK(mn.nearlyEquals(Vec3(0, 0, 0), 1e-9));
        std::set<Id> all;
        Mesh& open = st.activeMesh();
        for (Id v = 0; v < (Id)open.vertices.size(); ++v)
            if (open.vertices[v].alive) all.insert(v);
        open.transformVertices(all, Mat4::translation(Vec3(0, 0, 2)), false);
        CHECK(recordState(st, "UP"));
        CHECK(st.defs[def].states.size() == 2);
        CHECK(recordState(st, "UP")); // same name: updates it
        CHECK(st.defs[def].states.size() == 2 && findState(st.defs[def], "UP") == 1);
        CHECK(!recordState(st, ""));
        // Going to a state while open keeps world placement right.
        CHECK(applyState(st, def, "CLOSED"));
        Vec3 a, b;
        bool any = false;
        st.activeMesh().bounds(a, b, any);
        CHECK(a.nearlyEquals(Vec3(11, 1, 0), 1e-9));
        CHECK(!applyState(st, def, "NOPE"));
        CHECK(applyState(st, def, "UP"));
        d.closeComponent();
        CHECK(instanceBounds(st, inst, a, b));
        CHECK(near(a.z, 2, 1e-9));
        CHECK(applyState(st, def, "CLOSED")); // closed: in its own coordinates
        CHECK(instanceBounds(st, inst, a, b));
        CHECK(near(a.z, 0, 1e-9) && near(a.x, 11, 1e-9));
    });

    test("animation: parts of an entity", [&] {
        ModelState st;
        // Two separate pieces in one component: a frame that stays and a panel that swings.
        ComponentDef def;
        def.name = "Gate";
        def.mesh.drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 0, 2}, {0, 0, 2}}, true);
        def.mesh.drawPolyline({{5, 0, 0}, {6, 0, 0}, {6, 0, 2}, {5, 0, 2}}, true);
        CHECK(meshIslands(def.mesh).size() == 2);
        AnimState closed, open;
        closed.name = "CLOSED";
        open.name = "OPEN";
        const Mat4 swing = Mat4::translation(Vec3(5, 0, 0)) * Mat4::rotation(Vec3(0, 0, 1), kPi / 2) * Mat4::translation(Vec3(-5, 0, 0));
        for (Id v = 0; v < (Id)def.mesh.vertices.size(); ++v) {
            const Vec3 p = def.mesh.vertices[v].p;
            closed.verts.push_back(v);
            closed.pos.push_back(p);
            open.verts.push_back(v);
            open.pos.push_back(p.x >= 5 ? swing.transformPoint(p) : p);
        }
        def.states = {closed, open};
        st.defs.push_back(def);
        Instance in;
        in.def = 0;
        in.xf = Mat4::translation(Vec3(0, 7, 0));
        in.entity.classname = "func_door";
        st.instances.push_back(in);
        Instance plain;
        plain.def = 0;
        st.instances.push_back(plain);
        auto ents = prepareEntities(st);
        CHECK(ents.size() == 1 && ents[0].parts.size() == 2 && ents[0].states.size() == 2);
        CHECK(!st.instances[0].alive && st.instances[1].alive); // plain geometry stays as it is
        const AnimEntity& e = ents[0];
        CHECK(st.instances[e.parts[0].instance].dynamic && st.defs[e.parts[0].def].mesh.aliveFaceCount() == 1);
        poseEntity(st, e, 0, 1, 1.0);
        const AnimPart* panel = nullptr;
        const AnimPart* frame = nullptr;
        for (const AnimPart& p : e.parts) (st.defs[p.def].mesh.vertices[4].alive ? panel : frame) = &p;
        CHECK(panel && frame);
        if (panel && frame) {
            CHECK(st.instances[frame->instance].xf.isIdentity() == false); // the instance's place
            CHECK(st.instances[frame->instance].xf.transformPoint(Vec3(1, 0, 0)).nearlyEquals(Vec3(1, 7, 0), 1e-9));
            CHECK(st.instances[panel->instance].xf.transformPoint(Vec3(6, 0, 0)).nearlyEquals(Vec3(5, 8, 0), 1e-9));
            poseEntity(st, e, 0, 1, 0.5);
            const Vec3 mid = st.instances[panel->instance].xf.transformPoint(Vec3(6, 0, 0));
            CHECK(near((mid - Vec3(5, 7, 0)).length(), 1.0, 1e-9)); // on the arc
            CHECK(!poseEntity(st, e, 0, 0, 0)); // rigid parts leave the meshes alone
        }
        // A state that changes the shape blends the vertices instead.
        ModelState st2;
        ComponentDef def2 = def;
        for (Vec3& p : def2.states[1].pos) p = p * 2.0;
        st2.defs.push_back(def2);
        Instance in2;
        in2.def = 0;
        in2.entity.classname = "func_actor";
        st2.instances.push_back(in2);
        auto ents2 = prepareEntities(st2);
        CHECK(ents2[0].parts[0].deforms);
        CHECK(poseEntity(st2, ents2[0], 0, 1, 0.5));
        CHECK(poseEntity(st2, ents2[0], 0, 0, 0)); // and back to the rest shape
        // Collision bodies move with their transform and are hit by rays.
        CollisionWorld w;
        w.buildFromModel(st);
        const int body = w.addBody(st.defs[e.parts[0].def].mesh, st.instances[e.parts[0].instance].xf);
        CHECK(w.bodyCount() == 1 && !w.bodyTriangles(body).empty());
        double t = 0;
        int hit = -5;
        Vec3 mn, mx;
        w.bodyBounds(body, mn, mx);
        const Vec3 c = (mn + mx) * 0.5;
        CHECK(w.raycast(c + Vec3(0.05, -3, 0.1), Vec3(0, 1, 0), 10, t, hit) && hit == body);
        CHECK(!w.raycast(c + Vec3(0.05, -3, 0.1), Vec3(0, 1, 0), 1, t, hit));
        w.setBodyTransform(body, Mat4::translation(Vec3(0, 0, 100)));
        hit = -5;
        CHECK(!w.raycast(c + Vec3(0.05, -3, 0.1), Vec3(0, 1, 0), 10, t, hit) || hit != body);
    });

    test("components inside components", [&] {
        auto boxAt = [](Document& d, const Vec3& o, const char* name) {
            Mesh& m = d.mesh();
            std::set<Id> before;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive) before.insert(f);
            m.drawPolyline({o, o + Vec3(1, 0, 0), o + Vec3(1, 1, 0), o + Vec3(0, 1, 0)}, true);
            Selection s;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && !before.count(f)) s.faces.insert(f);
            return makeComponent(d.state, s, name);
        };
        Document d;
        ModelState& st = d.state;
        const Id handle = boxAt(d, Vec3(5, 0, 0), "Handle");
        const Id door = boxAt(d, Vec3(0, 0, 0), "Door");
        // A component made of components.
        Selection both;
        both.instances = {handle, door};
        const Id whole = makeComponent(st, both, "Whole");
        CHECK(whole != kNone && st.instances[whole].parent == kNone);
        const Id wholeDef = st.instances[whole].def;
        CHECK(st.instances[handle].parent == wholeDef && st.instances[door].parent == wholeDef);
        CHECK(childrenOf(st, wholeDef).size() == 2);
        Vec3 a, b;
        CHECK(instanceBounds(st, whole, a, b));
        CHECK(a.nearlyEquals(Vec3(0, 0, 0), 1e-9) && b.nearlyEquals(Vec3(6, 1, 0), 1e-9));
        CHECK(placedInstances(st).size() == 3);
        CHECK(flattenModel(st).aliveFaceCount() == 2);
        // Placed elsewhere, what is inside goes along, and copies share it.
        st.instances[whole].xf = Mat4::translation(Vec3(0, 10, 0));
        const Id copy = cloneInstance(st, whole);
        st.instances[copy].xf = Mat4::translation(Vec3(0, 20, 0));
        auto placedDoorAt = [&](Id root) {
            for (const PlacedInstance& p : placedInstances(st))
                if (p.root == root && p.inst == door) return p.xf.transformPoint(Vec3());
            return Vec3(-99, -99, -99);
        };
        CHECK(placedDoorAt(whole).nearlyEquals(Vec3(0, 10, 0), 1e-9) && placedDoorAt(copy).nearlyEquals(Vec3(0, 20, 0), 1e-9));
        // Nothing can go inside itself.
        CHECK(definitionContains(st, wholeDef, st.instances[door].def) && !definitionContains(st, st.instances[door].def, wholeDef));
        CHECK(canPlaceInActive(st, wholeDef));
        // Opening stacks: the frame, then the door in it.
        const ModelState closed = st;
        d.openComponent(whole);
        CHECK(st.editing == whole && st.activeDef() == wholeDef);
        CHECK(!canPlaceInActive(st, wholeDef) && canPlaceInActive(st, st.instances[door].def));
        CHECK(st.instances[door].xf.transformPoint(Vec3()).nearlyEquals(Vec3(0, 10, 0), 1e-9)); // world, for the tools
        d.selection.instances = {door};
        d.validateSelection();
        CHECK(d.selection.instances == std::set<Id>{door}); // what is inside can be selected now
        d.selection.instances = {whole};
        d.validateSelection();
        CHECK(d.selection.instances.empty());
        d.openComponent(door);
        CHECK(st.editing == door && st.outerOpen == std::vector<Id>{whole});
        Vec3 ma, mb;
        bool any = false;
        st.activeMesh().bounds(ma, mb, any);
        CHECK(ma.nearlyEquals(Vec3(0, 10, 0), 1e-9));
        CHECK(!canPlaceInActive(st, wholeDef) && !canPlaceInActive(st, st.instances[door].def));
        // The other copy's door still is where it belongs.
        bool otherOk = false;
        for (const PlacedInstance& p : placedInstances(st))
            if (p.inst == door && !p.active) otherOk = otherOk || p.xf.transformPoint(st.activeMesh().vertices[0].p).nearlyEquals(Vec3(0, 20, 0) + (st.activeMesh().vertices[0].p - Vec3(0, 10, 0)), 1e-9);
        CHECK(otherOk);
        d.closeComponent();
        CHECK(st.editing == whole && st.outerOpen.empty());
        d.closeComponent();
        CHECK(st.editing == kNone);
        CHECK(st.instances[door].xf.transformPoint(Vec3()).nearlyEquals(closed.instances[door].xf.transformPoint(Vec3()), 1e-9));
        // A state records where the components inside are.
        d.openComponent(whole);
        CHECK(recordState(st, "A"));
        CHECK(st.defs[wholeDef].states[0].children.size() == 2);
        st.instances[door].xf = Mat4::translation(Vec3(0, 0, 3)) * st.instances[door].xf;
        CHECK(recordState(st, "B"));
        CHECK(applyState(st, wholeDef, "A"));
        CHECK(near(st.instances[door].xf.at(2, 3), closed.instances[door].xf.at(2, 3), 1e-9));
        d.closeComponent();
        // Made unique, the copy gets its own contents.
        Document u = Document();
        u.state = st;
        makeUnique(u.state, {copy});
        const Id newDef = u.state.instances[copy].def;
        CHECK(newDef != wholeDef && childrenOf(u.state, newDef).size() == 2 && childrenOf(u.state, wholeDef).size() == 2);
        for (const auto& [id, xf] : u.state.defs[newDef].states[0].children) CHECK(u.state.instances[id].parent == newDef);
        // Exploded, they come out to the level it was on.
        Selection out;
        explodeInstance(u.state, copy, &out);
        CHECK(out.instances.size() == 2);
        for (Id i : out.instances) CHECK(u.state.instances[i].parent == kNone);
        CHECK(childrenOf(u.state, wholeDef).size() == 2);
        // Inside an open component, Make Component nests too.
        Document n;
        const Id outer = boxAt(n, Vec3(0, 0, 0), "Outer");
        n.openComponent(outer);
        const Id inner = boxAt(n, Vec3(0, 0, 2), "Inner");
        CHECK(inner != kNone && n.state.instances[inner].parent == n.state.activeDef());
        CHECK(n.state.instances[inner].xf.transformPoint(Vec3()).nearlyEquals(Vec3(0, 0, 2), 1e-9));
        n.closeComponent();
        CHECK(flattenModel(n.state).aliveFaceCount() == 2);
        // Extraction carries the tree.
        Material used;
        used.name = "Oak";
        const Id mid = d.addMaterial(used);
        for (Face& f : st.defs[st.instances[door].def].mesh.faces) f.front.material = mid;
        ModelState pre = extractDefinition(st, wholeDef);
        CHECK(pre.defs.size() == 3 && pre.instances.size() == 3 && pre.instances[0].def == 0 && pre.instances[0].parent == kNone);
        CHECK(pre.materials.size() == 1 && childrenOf(pre, 0).size() == 2);
        CHECK(pre.defs[0].states.size() == 2);
        for (const auto& [id, xf] : pre.defs[0].states[0].children) CHECK(id >= 0 && pre.instances[id].parent == 0);
        Document other;
        const Id imported = importDefinition(other, pre, 0);
        CHECK(other.state.defs.size() == 3 && other.state.instances.size() == 2 && other.state.materials.size() == 1);
        CHECK(childrenOf(other.state, imported).size() == 2);
        for (const auto& [id, xf] : other.state.defs[imported].states[0].children) CHECK(other.state.instances[id].parent == imported);
        CHECK(importDefinition(other, pre, 0) == imported && other.state.instances.size() == 2);
    });

    test("entities inside components and inside entities", [&] {
        // A frame (plain) holding a door (entity) holding a handle (plain), holding a lock (entity).
        ModelState st;
        auto plate = [&](const char* name, const Vec3& o) {
            ComponentDef d;
            d.name = name;
            d.mesh.drawPolyline({o, o + Vec3(1, 0, 0), o + Vec3(1, 0, 1), o + Vec3(0, 0, 1)}, true);
            st.defs.push_back(d);
            return (Id)st.defs.size() - 1;
        };
        const Id frame = plate("Frame", Vec3(-1, 0, 0)), doorDef = plate("Door", Vec3(0, 0, 0)), handleDef = plate("Handle", Vec3(0.5, 0, 0.5)), lockDef = plate("Lock", Vec3(0.7, 0, 0.7));
        Instance root;
        root.def = frame;
        root.xf = Mat4::translation(Vec3(100, 0, 0));
        st.instances.push_back(root);
        Instance door;
        door.def = doorDef;
        door.parent = frame;
        door.xf = Mat4::translation(Vec3(2, 0, 0));
        door.entity.classname = "func_door";
        st.instances.push_back(door);
        Instance handle;
        handle.def = handleDef;
        handle.parent = doorDef;
        st.instances.push_back(handle);
        Instance lock;
        lock.def = lockDef;
        lock.parent = handleDef;
        lock.entity.classname = "func_actor";
        st.instances.push_back(lock);
        // Door states: it rises by 2; the handle inside tilts up by 1 more.
        AnimState closed, open;
        closed.name = "CLOSED";
        open.name = "OPEN";
        for (Id v = 0; v < (Id)st.defs[doorDef].mesh.vertices.size(); ++v) {
            closed.verts.push_back(v);
            closed.pos.push_back(st.defs[doorDef].mesh.vertices[v].p);
            open.verts.push_back(v);
            open.pos.push_back(st.defs[doorDef].mesh.vertices[v].p + Vec3(0, 0, 2));
        }
        closed.children.emplace_back(2, Mat4());
        open.children.emplace_back(2, Mat4::translation(Vec3(0, 0, 3)));
        st.defs[doorDef].states = {closed, open};
        auto ents = prepareEntities(st);
        CHECK(ents.size() == 2);
        CHECK(!st.instances[0].alive);
        const int de = ents[0].props.classname == "func_door" ? 0 : 1, le = 1 - de;
        CHECK(ents[le].owner == de && ents[de].owner == -1);
        CHECK(ents[de].parts.size() == 2); // its own plate, and the handle
        // Everything that is shown is now flat: top level, no children below.
        CollisionWorld w;
        w.buildFromModel(st);
        CHECK(w.triangles().size() == 2); // only the frame is static
        auto setPose = [&](int a, int b, double t) {
            ents[de].poseA = a;
            ents[de].poseB = b;
            ents[de].poseT = t;
            poseEntity(st, ents[de], a, b, t);
            poseMounted(st, ents, de, [](int) {});
        };
        setPose(0, 0, 0);
        const Mat4 lockClosed = st.instances[ents[le].parts[0].instance].xf;
        CHECK(lockClosed.transformPoint(Vec3()).nearlyEquals(Vec3(102, 0, 0), 1e-9));
        setPose(1, 1, 0);
        CHECK(st.instances[ents[de].parts[0].instance].xf.transformPoint(Vec3()).nearlyEquals(Vec3(102, 0, 2), 1e-9));
        CHECK(st.instances[ents[de].parts[1].instance].xf.transformPoint(Vec3()).nearlyEquals(Vec3(102, 0, 3), 1e-9));
        CHECK(st.instances[ents[le].parts[0].instance].xf.transformPoint(Vec3()).nearlyEquals(Vec3(102, 0, 3), 1e-9)); // the lock rides along
        setPose(0, 1, 0.5);
        CHECK(st.instances[ents[le].parts[0].instance].xf.transformPoint(Vec3()).nearlyEquals(Vec3(102, 0, 1.5), 1e-9));
    });

    test("physics: falling, resting, sliding", [&] {
        // A cube as a mesh, its lowest corner at `o`.
        auto cube = [](const Vec3& o, double a) {
            Mesh m;
            const Vec3 p[8] = {o, o + Vec3(a, 0, 0), o + Vec3(a, a, 0), o + Vec3(0, a, 0),
                               o + Vec3(0, 0, a), o + Vec3(a, 0, a), o + Vec3(a, a, a), o + Vec3(0, a, a)};
            Id v[8];
            for (int i = 0; i < 8; ++i) v[i] = m.addVertexRaw(p[i]);
            const int F[6][4] = {{3, 2, 1, 0}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
            for (const auto& f : F) {
                std::vector<Id> loop;
                for (int k : f) loop.push_back(v[k]);
                for (int k = 0; k < 4; ++k) m.addEdgeRaw(loop[k], loop[(k + 1) % 4]);
                const Vec3 n = (p[f[1]] - p[f[0]]).cross(p[f[2]] - p[f[0]]).normalized();
                m.addFaceRaw(loop, {}, n);
            }
            m.touch();
            return m;
        };
        auto addCube = [&](PhysicsWorld& pw, CollisionWorld& w, const Vec3& o, double a, PhysicsParams params = {}) {
            const int c = w.addBody(cube(o, a), Mat4());
            return pw.addBody(params, {c}, w);
        };
        auto run = [](PhysicsWorld& pw, CollisionWorld& w, double seconds) {
            // Each body's triangles were added in world coordinates.
            for (double t = 0; t < seconds; t += 1.0 / 60)
                pw.step(1.0 / 60, w, [&](int b) {
                    for (int c : pw.collisionBodies(b)) w.setBodyTransform(c, pw.transform(b));
                });
        };
        auto lowest = [](const CollisionWorld& w, int c) {
            Vec3 mn, mx;
            w.bodyBounds(c, mn, mx);
            return mn;
        };
        {
            // It falls by the book, lands flat on the ground and comes to rest.
            CollisionWorld w;
            w.build();
            PhysicsWorld pw;
            const int b = addCube(pw, w, Vec3(0, 0, 20), 1);
            run(pw, w, 1.0);
            CHECK(near(lowest(w, 0).z, 20 - 0.5 * 9.81, 0.2));
            run(pw, w, 3.0);
            CHECK(near(lowest(w, 0).z, 0, 0.02));
            run(pw, w, 2.0);
            CHECK(pw.asleep(b));
            const Mat4 xf = pw.transform(b);
            CHECK(near(xf.at(0, 0), 1, 0.01) && near(xf.at(2, 2), 1, 0.01)); // not tipped over
            CHECK(near(lowest(w, 0).x, 0, 0.05));
            // Pushed, it wakes up and moves on the ground.
            pw.applyImpulse(b, Vec3(40, 0, 0), pw.centre(b));
            CHECK(!pw.asleep(b));
            run(pw, w, 0.3);
            CHECK(lowest(w, 0).x > 0.1);
            CHECK(near(lowest(w, 0).z, 0, 0.1));
        }
        auto slope = [](CollisionWorld& w, double degrees) {
            const double a = degrees * kPi / 180, L = 40;
            const Vec3 d(std::cos(a) * L, 0, -std::sin(a) * L);
            const Vec3 o(-5, -5, std::sin(a) * L + 2), s(0, 10, 0);
            w.addTriangle(o, o + d, o + d + s);
            w.addTriangle(o, o + d + s, o + s);
            w.build();
        };
        {
            // Steep and slippery: it slides down.
            CollisionWorld w;
            slope(w, 25);
            PhysicsWorld pw;
            PhysicsParams slick;
            slick.friction = 0.1;
            addCube(pw, w, Vec3(0, -0.5, 18), 1, slick);
            run(pw, w, 1.0); // lands
            const double v1 = pw.velocity(0).length();
            run(pw, w, 2.0);
            const double v3 = pw.velocity(0).length();
            // g (sin a - mu cos a) along the slope.
            const double expected = 9.81 * (std::sin(25 * kPi / 180) - 0.1 * std::cos(25 * kPi / 180));
            CHECK(near((v3 - v1) / 2, expected, 0.1));
            const double slid = lowest(w, 0).x;
            CHECK(slid > 3.0);
            CHECK(std::fabs(lowest(w, 0).y + 0.5) < 0.5); // straight down the slope
        }
        {
            // Same slope with grip: it stays where it lands.
            CollisionWorld w;
            slope(w, 25);
            PhysicsWorld pw;
            PhysicsParams grip;
            grip.friction = 0.9;
            // Set down gently: its base just above the slope where that is highest, at x = 0.
            addCube(pw, w, Vec3(0, -0.5, std::sin(25 * kPi / 180) * 40 + 2 - 5 * std::tan(25 * kPi / 180) + 0.03), 1, grip);
            run(pw, w, 2.5); // tips onto its base
            const double x1 = lowest(w, 0).x;
            CHECK(pw.transform(0).at(2, 2) > 0.85); // lying on its side's base, not rolled over
            run(pw, w, 3.0);
            CHECK(std::fabs(lowest(w, 0).x - x1) < 0.03);
            CHECK(pw.asleep(0));
        }
        {
            // Placed sunk into a floor by half its height, it comes out on top.
            CollisionWorld w;
            const Vec3 q[4] = {{-5, -5, 3}, {5, -5, 3}, {5, 5, 3}, {-5, 5, 3}};
            w.addTriangle(q[0], q[1], q[2]);
            w.addTriangle(q[0], q[2], q[3]);
            w.build();
            PhysicsWorld pw;
            addCube(pw, w, Vec3(0, 0, 2.6), 1); // centre above the floor by 0.1
            run(pw, w, 3.0);
            CHECK(near(lowest(w, 0).z, 3, 0.05));
        }
        {
            // It rests on a static block instead of falling through.
            CollisionWorld w;
            const Vec3 q[4] = {{-5, -5, 3}, {5, -5, 3}, {5, 5, 3}, {-5, 5, 3}};
            w.addTriangle(q[0], q[1], q[2]);
            w.addTriangle(q[0], q[2], q[3]);
            w.build();
            PhysicsWorld pw;
            addCube(pw, w, Vec3(0, 0, 6), 1);
            run(pw, w, 3.0);
            CHECK(near(lowest(w, 0).z, 3, 0.03));
        }
        {
            // A corner of the world poking at a flat side: the post test.
            CollisionWorld w;
            w.addTriangle(Vec3(0.4, 0.4, 2), Vec3(0.6, 0.4, 0), Vec3(0.4, 0.6, 0));
            w.build();
            PhysicsWorld pw;
            addCube(pw, w, Vec3(0, 0, 4), 1);
            run(pw, w, 0.8);
            CHECK(lowest(w, 0).z > 1.5); // stopped by the tip, or tilted off it, not through it at once
        }
        {
            // Mass: a heavy box shoves a light one aside; the other way round it hardly does.
            auto knock = [&](double hitter, double target) {
                CollisionWorld w;
                w.build();
                PhysicsWorld pw;
                PhysicsParams a, b;
                a.mass = hitter;
                b.mass = target;
                a.friction = b.friction = 0.05;
                a.bounce = b.bounce = 0;
                const int A = addCube(pw, w, Vec3(0, 0, 0), 1, a);
                addCube(pw, w, Vec3(1.5, 0, 0), 1, b);
                pw.applyImpulse(A, Vec3(hitter * 4, 0, 0), pw.centre(A));
                run(pw, w, 0.8);
                return lowest(w, 1).x - 1.5;
            };
            CHECK(knock(100, 5) > knock(5, 100) + 0.5);
        }
        {
            // A stack settles.
            CollisionWorld w;
            w.build();
            PhysicsWorld pw;
            addCube(pw, w, Vec3(0, 0, 0.5), 1);
            addCube(pw, w, Vec3(0, 0, 2), 1);
            run(pw, w, 4.0);
            CHECK(near(lowest(w, 0).z, 0, 0.05));
            CHECK(near(lowest(w, 1).z, 1, 0.08));
        }
    });

    test("names look in their own group first", [&] {
        auto entity = [](const char* name, std::vector<std::pair<int, std::string>> scope, int occ) {
            AnimEntity e;
            e.props.targetname = name;
            e.props.classname = "func_door";
            e.scope = std::move(scope);
            e.occurrence = occ;
            return e;
        };
        std::vector<AnimEntity> all;
        all.push_back(entity("button", {{1, "front"}}, 10));
        all.push_back(entity("door", {{1, "front"}}, 11));
        all.push_back(entity("button", {{2, "back"}}, 12));
        all.push_back(entity("door", {{2, "back"}}, 13));
        all.push_back(entity("master", {}, 14));
        all.push_back(entity("lamp", {}, 15));
        using V = std::vector<int>;
        CHECK(resolveEntityName(all, 0, "door") == V{1});
        CHECK(resolveEntityName(all, 2, "door") == V{3});
        CHECK((resolveEntityName(all, 4, "door") == V{1, 3})); // from outside: all of them
        CHECK(resolveEntityName(all, 4, "front/door") == V{1});
        CHECK(resolveEntityName(all, 0, "back/door") == V{3}); // reaching across
        CHECK(resolveEntityName(all, 0, "lamp") == V{5});      // not in the group: further out
        CHECK(resolveEntityName(all, 0, "nothing").empty());
        CHECK(resolveEntityName(all, 0, "").empty());
        CHECK(resolveEntityName(all, -1, "front/door") == V{1});
    });

    test("effects: voxelate", [&] {
        {
            // A box that fits the grid keeps its exact size, 4 x 4 x 4 cubes.
            Mesh m;
            cube(m);
            Material none;
            EffectResult r;
            VoxelateParams p;
            p.size = 0.25;
            CHECK(effectVoxelate(m, allFaces(m), p, &r));
            CHECK_EQ(m.aliveFaceCount(), 6 * 16);
            CHECK_EQ((int)r.faces.size(), m.aliveFaceCount());
            CHECK(!r.limited);
            CHECK(isSolid(m));
            CHECK(near(solidVolume(m), 1.0));
            Vec3 mn, mx;
            bool any = false;
            m.bounds(mn, mx, any);
            CHECK(mn.nearlyEquals(Vec3(0, 0, 0), 1e-9) && mx.nearlyEquals(Vec3(1, 1, 1), 1e-9));
            CHECK(m.aliveVertexCount() == 5 * 5 * 5 - 3 * 3 * 3);
            for (Id f = 0; f < (Id)m.faces.size(); ++f) // normals look out
                if (m.faces[f].alive) CHECK((m.faceInteriorPoint(f) - Vec3(0.5, 0.5, 0.5)).dot(m.faces[f].normal) > 0);
        }
        {
            // Merged, it is a plain box again.
            Mesh m;
            cube(m);
            VoxelateParams p;
            p.size = 0.25;
            p.mergeFlat = true;
            CHECK(effectVoxelate(m, allFaces(m), p));
            CHECK_EQ(m.aliveFaceCount(), 6);
            CHECK(near(solidVolume(m), 1.0));
        }
        {
            // Not filled: a shell, with an inner surface too.
            Mesh m;
            cube(m);
            VoxelateParams p;
            p.size = 0.25;
            p.fill = false;
            CHECK(effectVoxelate(m, allFaces(m), p));
            CHECK_EQ(m.aliveFaceCount(), 6 * 16 + 6 * 4);
            CHECK(near(solidVolume(m), 1.0 - 0.125, 1e-6));
        }
        {
            // A size that does not fit rounds up to whole cubes that cover it.
            Mesh m;
            cube(m);
            VoxelateParams p;
            p.size = 0.3;
            CHECK(effectVoxelate(m, allFaces(m), p));
            CHECK(near(solidVolume(m), 1.2 * 1.2 * 1.2, 1e-6));
        }
        {
            // A flat face is one layer thick, and open shapes do not fill.
            Mesh m;
            m.drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, true);
            VoxelateParams p;
            p.size = 0.25;
            CHECK(effectVoxelate(m, allFaces(m), p));
            CHECK(near(solidVolume(m), 0.25, 1e-6));
            Vec3 mn, mx;
            bool any = false;
            m.bounds(mn, mx, any);
            CHECK(near(mx.z - mn.z, 0.25));
        }
        {
            // A slanted surface becomes stairs of cubes.
            Mesh m;
            m.drawPolyline({{0, 0, 0}, {4, 0, 2}, {4, 4, 2}, {0, 4, 0}}, true);
            VoxelateParams p;
            p.size = 0.5;
            CHECK(effectVoxelate(m, allFaces(m), p));
            CHECK(m.aliveFaceCount() > 40);
            for (Id f = 0; f < (Id)m.faces.size(); ++f) // on the grid, along the axes
                if (m.faces[f].alive) CHECK(near(std::fabs(m.faces[f].normal.x) + std::fabs(m.faces[f].normal.y) + std::fabs(m.faces[f].normal.z), 1));
        }
        {
            // Materials stay; each side takes the source facing its way.
            Mesh m;
            cube(m);
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive) m.faces[f].front.material = m.faces[f].normal.z > 0.5 ? 3 : 1;
            VoxelateParams p;
            p.size = 0.25;
            CHECK(effectVoxelate(m, allFaces(m), p));
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive) CHECK_EQ(m.faces[f].front.material, m.faces[f].normal.z > 0.5 ? 3 : 1);
        }
        {
            // Asking too much makes bigger cubes, and says so; bad input changes nothing.
            Mesh m;
            cube(m, 100);
            EffectResult r;
            VoxelateParams p;
            p.size = 0.01;
            CHECK(effectVoxelate(m, allFaces(m), p, &r));
            CHECK(r.limited);
            CHECK(m.aliveFaceCount() <= kEffectTriangleLimit);
            Mesh same;
            cube(same);
            p.size = 0;
            std::string err;
            CHECK(!effectVoxelate(same, allFaces(same), p, nullptr, &err) && !err.empty());
            CHECK_EQ(same.aliveFaceCount(), 6);
            p.size = 0.1;
            CHECK(!effectVoxelate(same, {}, p, nullptr, &err));
        }
        {
            // Only what is selected changes.
            Mesh m;
            cube(m);
            m.drawPolyline({{5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}}, true);
            std::set<Id> box;
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive && m.faceInteriorPoint(f).x < 2) box.insert(f);
            VoxelateParams p;
            p.size = 0.5;
            CHECK(effectVoxelate(m, box, p));
            CHECK_EQ(m.aliveFaceCount(), 6 * 4 + 1);
        }
    });

    test("primitives: closed, outward, on the ground", [&] {
        auto softCount = [](const Mesh& m) {
            int n = 0;
            for (const Edge& e : m.edges) n += e.alive && e.soft;
            return n;
        };
        auto check = [&](PrimitiveParams p, int verts, int edges, int faces, double volume, double volTol) {
            Mesh m;
            std::string err;
            CHECK(buildPrimitive(p, m, &err));
            if (!err.empty()) std::printf("  %s: %s\n", primitiveName(p.kind), err.c_str());
            CHECK_EQ(m.aliveVertexCount(), verts);
            CHECK_EQ(m.aliveEdgeCount(), edges);
            CHECK_EQ(m.aliveFaceCount(), faces);
            std::string why;
            const bool solid = isSolid(m, &why);
            CHECK(solid);
            if (!solid) std::printf("  %s: %s\n", primitiveName(p.kind), why.c_str());
            // Positive volume means the faces point outwards.
            const double v = solidVolume(m);
            CHECK(std::fabs(v - volume) <= volume * volTol);
            if (std::fabs(v - volume) > volume * volTol) std::printf("  %s volume %g, expected %g\n", primitiveName(p.kind), v, volume);
            Vec3 mn, mx;
            bool any = false;
            m.bounds(mn, mx, any);
            CHECK(std::fabs(mn.z) < 1e-9);
            CHECK(std::fabs(mn.x + mx.x) < 1e-6 && std::fabs(mn.y + mx.y) < 1e-6);
            return m;
        };
        const double r = 1;
        PrimitiveParams p;
        p.radius = r;
        p.kind = PrimitiveKind::Tetrahedron;
        check(p, 4, 6, 4, 8 * std::sqrt(3.0) / 27, 1e-6);
        p.kind = PrimitiveKind::Octahedron;
        check(p, 6, 12, 8, 4.0 / 3, 1e-6);
        p.kind = PrimitiveKind::Dodecahedron;
        {
            Mesh m = check(p, 20, 30, 12, 2.785163863, 1e-6);
            int pentagons = 0;
            for (const Face& f : m.faces) pentagons += f.alive && f.outer.size() == 5;
            CHECK_EQ(pentagons, 12);
            CHECK_EQ(softCount(m), 0);
            // It rests on a face: five vertices on the ground.
            int low = 0;
            for (const Vertex& v : m.vertices) low += std::fabs(v.p.z) < 1e-9;
            CHECK_EQ(low, 5);
        }
        p.kind = PrimitiveKind::Icosahedron;
        check(p, 12, 30, 20, 2.536150994, 1e-6);
        p.kind = PrimitiveKind::Box;
        p.width = 2; p.depth = 3; p.height = 4;
        check(p, 8, 12, 6, 24, 1e-9);
        p.kind = PrimitiveKind::Sphere;
        p.segments = 24; p.rings = 12;
        {
            Mesh m = check(p, 24 * 11 + 2, 24 * 11 + 24 * 12, 24 * 12, 4.0 / 3 * kPi, 0.05);
            CHECK_EQ(softCount(m), m.aliveEdgeCount());
            CHECK(std::fabs(m.vertices[0].p.z) < 2 * r + 1e-9);
        }
        p.kind = PrimitiveKind::GeoSphere;
        p.frequency = 3;
        check(p, 10 * 9 + 2, 30 * 9, 20 * 9, 4.0 / 3 * kPi, 0.08);
        p.kind = PrimitiveKind::Cylinder;
        p.height = 2; p.segments = 16;
        {
            Mesh m = check(p, 32, 48, 18, kPi * 2, 0.03);
            CHECK_EQ(softCount(m), 16); // the vertical edges; the rims stay hard
            std::set<Id> curves;
            for (const Edge& e : m.edges)
                if (e.curve != kNone) curves.insert(e.curve);
            CHECK_EQ((int)curves.size(), 2);
        }
        p.kind = PrimitiveKind::Cone;
        p.radius2 = 0;
        check(p, 17, 32, 17, kPi * 2 / 3, 0.03);
        p.radius2 = 0.5; // frustum
        check(p, 32, 48, 18, kPi * 2 / 3 * (1 + 0.5 + 0.25), 0.03);
        p.kind = PrimitiveKind::Torus;
        p.radius = 2; p.radius2 = 0.5; p.segments = 32; p.rings = 16;
        check(p, 32 * 16, 2 * 32 * 16, 32 * 16, 2 * kPi * kPi * 2 * 0.25, 0.05);

        Mesh bad;
        std::string err;
        p.radius2 = 3; // tube wider than the ring
        CHECK(!buildPrimitive(p, bad, &err) && !err.empty());
        p.kind = PrimitiveKind::Sphere;
        p.radius = 0;
        CHECK(!buildPrimitive(p, bad, &err));
        p.radius = 1; p.smooth = false;
        Mesh hard;
        CHECK(buildPrimitive(p, hard));
        CHECK_EQ(softCount(hard), 0);
    });

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
