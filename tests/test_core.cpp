// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for the geometry kernel.
#include "core/Document.h"
#include "core/Mesh.h"
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

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
