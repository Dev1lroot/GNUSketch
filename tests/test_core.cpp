// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for the geometry kernel.
#include "core/Components.h"
#include "core/Document.h"
#include "core/Mesh.h"
#include "core/RoundCorners.h"
#include "core/Terrain.h"
#include "core/Material.h"
#include "core/FollowMe.h"
#include "core/ImageTrace.h"
#include "core/Triangulate.h"
#include "core/Units.h"

#include <cmath>
#include <cstdio>
#include <functional>
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

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
