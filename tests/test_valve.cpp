// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Valve exports (plugins/valve/ValveFormat.h).
#include "ValveFormat.h"
#include "core/Components.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace gs;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) ++g_passed;                                                             \
        else { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

// A box (meters) as loose geometry or in `m`.
static void box(Mesh& m, Vec3 lo, Vec3 hi) {
    m.groundFacesDown = false;
    m.drawPolyline({{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}}, true);
    Id f = kNone;
    for (Id i = 0; i < (Id)m.faces.size(); ++i)
        if (m.faces[i].alive && std::fabs(m.faces[i].normal.z) > 0.99 && m.pointInFace(i, (lo + hi) * 0.5 - Vec3(0, 0, (hi.z - lo.z) * 0.5), false))
            f = i;
    m.pushPull(f, m.faces[f].normal.z < 0 ? -(hi.z - lo.z) : hi.z - lo.z);
}

static Vec3 parsePoint(const QString& s) {
    const QStringList p = s.split(' ');
    return Vec3(p[0].toDouble(), p[1].toDouble(), p[2].toDouble());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const double u = 0.0254; // one Source unit in meters
    {
        // A 128 unit cube: one brush of six sides, all facing outwards.
        Document d;
        box(d.mesh(), Vec3(0, 0, 0), Vec3(128 * u, 128 * u, 128 * u));
        QStringList msgs;
        const QString vmf = QString::fromUtf8(valve::vmfText(d, {}, &msgs));
        CHECK(vmf.count(QStringLiteral("\tsolid\n")) == 1);
        static const QRegularExpression plane(QStringLiteral("\"plane\" \"\\(([^)]*)\\) \\(([^)]*)\\) \\(([^)]*)\\)\""));
        int sides = 0, outward = 0;
        for (auto it = plane.globalMatch(vmf); it.hasNext();) {
            const auto m = it.next();
            const Vec3 p1 = parsePoint(m.captured(1)), p2 = parsePoint(m.captured(2)), p3 = parsePoint(m.captured(3));
            // Hammer: points clockwise seen from outside, so (p3-p1)x(p2-p1) points out.
            const Vec3 n = (p3 - p1).cross(p2 - p1);
            const Vec3 centre = (p1 + p2 + p3) / 3.0;
            ++sides;
            outward += n.dot(centre - Vec3(64, 64, 64)) > 0;
            for (const Vec3& p : {p1, p2, p3})
                for (int k = 0; k < 3; ++k) CHECK(std::fabs(p[k]) < 1e-6 || std::fabs(p[k] - 128) < 1e-3);
        }
        CHECK(sides == 6 && outward == 6);
        CHECK(vmf.contains(QStringLiteral("\"material\" \"DEV/GRAYGRID\"")));
        CHECK(vmf.contains(QStringLiteral("\"classname\" \"worldspawn\"")) && vmf.contains(QStringLiteral("\"skyname\" \"sky_day01_01\"")));
        CHECK(msgs.join(' ').contains(QStringLiteral("info_player_start")));
        std::printf("[ ok ] a cube is a brush of six outward sides\n");
    }
    {
        // Materials, an L shape (not convex), entities, groups, lights.
        Document d;
        Material nodraw;
        nodraw.name = "tools-toolsnodraw";
        d.materials().push_back(nodraw);
        box(d.mesh(), Vec3(0, 0, 0), Vec3(1, 1, 1));
        for (Face& F : d.mesh().faces) F.front.material = 0;
        // L-shaped prism elsewhere: not convex.
        Mesh& m = d.mesh();
        m.drawPolyline({{5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {6, 1, 0}, {6, 2, 0}, {5, 2, 0}}, true);
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive && m.faceInteriorPoint(f).x > 4.5 && std::fabs(m.faces[f].normal.z) > 0.99 && m.faceInteriorPoint(f).z < 0.01) {
                m.pushPull(f, m.faces[f].normal.z < 0 ? -1 : 1);
                break;
            }
        // A component of two boxes (a group) and one named info_player_start.
        Selection two;
        Mesh pair;
        box(pair, Vec3(10, 0, 0), Vec3(11, 1, 1));
        box(pair, Vec3(12, 0, 0), Vec3(13, 1, 1));
        appendMesh(m, pair, Mat4(), nullptr, nullptr, false, &two);
        makeComponent(d.state, two, "Crates");
        Mesh marker;
        box(marker, Vec3(-0.2, -0.2, 0), Vec3(0.2, 0.2, 0.4));
        Selection ms;
        appendMesh(m, marker, Mat4::translation({20 * u * 64, 0, 0}), nullptr, nullptr, false, &ms);
        makeComponent(d.state, ms, "info_player_start");
        SceneObject lamp;
        lamp.pos = Vec3(1, 1, 2);
        lamp.color = Color(1, 0.5f, 0);
        lamp.intensity = 3;
        d.addObject(lamp);
        QStringList msgs;
        const QString vmf = QString::fromUtf8(valve::vmfText(d, {}, &msgs));
        std::printf("    %s\n", qPrintable(msgs.join(QStringLiteral(" | "))));
        CHECK(vmf.contains(QStringLiteral("\"material\" \"TOOLS/TOOLSNODRAW\"")));
        CHECK(vmf.count(QStringLiteral("\tsolid\n")) == 3);               // the cube and the two crates
        CHECK(msgs.join(' ').contains(QStringLiteral("not convex")));       // the L
        CHECK(vmf.count(QStringLiteral("\ngroup\n")) == 1 && vmf.count(QStringLiteral("\"groupid\"")) == 2);
        CHECK(vmf.contains(QStringLiteral("\"classname\" \"info_player_start\"")));
        CHECK(!msgs.join(' ').contains(QStringLiteral("No info_player_start")));
        static const QRegularExpression origin(QStringLiteral("\"classname\" \"info_player_start\"[\\s\\S]*?\"origin\" \"([^\"]*)\""));
        const auto om = origin.match(vmf);
        CHECK(om.hasMatch() && std::fabs(parsePoint(om.captured(1)).x - 20 * 64) < 0.01);
        CHECK(vmf.contains(QStringLiteral("\"classname\" \"light\"")) && vmf.contains(QStringLiteral("\"_light\" \"255 128 0 300\"")));
        std::printf("[ ok ] materials, convexity, groups, entities and lights\n");
    }
    {
        // Sealing box, the size limit, SMD and QC.
        Document d;
        box(d.mesh(), Vec3(0, 0, 0), Vec3(500, 1, 1)); // 500 m: beyond 16384 units
        valve::VmfOptions o;
        o.sealBox = true;
        QStringList msgs;
        const QString vmf = QString::fromUtf8(valve::vmfText(d, o, &msgs));
        CHECK(vmf.count(QStringLiteral("\tsolid\n")) == 7 && vmf.count(QStringLiteral("TOOLS/TOOLSSKYBOX")) == 36);
        CHECK(msgs.join(' ').contains(QStringLiteral("16384")));
        QTemporaryDir dir;
        Document cube;
        box(cube.mesh(), Vec3(0, 0, 0), Vec3(1, 1, 1));
        const QString path = QDir(dir.path()).filePath(QStringLiteral("crate.smd"));
        QStringList sm;
        CHECK(valve::writeSmd(cube, path, {}, &sm));
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QString smd = QString::fromUtf8(f.readAll());
        CHECK(smd.startsWith(QStringLiteral("version 1\nnodes\n0 \"root\" -1\nend\nskeleton\n")));
        CHECK(smd.count(QStringLiteral("\ngraygrid\n")) == 12); // 6 quads, 2 triangles each
        CHECK(smd.contains(QStringLiteral(" 39.3701 ")));        // 1 m in inches
        QFile qc(QDir(dir.path()).filePath(QStringLiteral("crate.qc")));
        CHECK(qc.open(QIODevice::ReadOnly));
        const QString q = QString::fromUtf8(qc.readAll());
        CHECK(q.contains(QStringLiteral("$modelname \"gnusketch/crate.mdl\"")) && q.contains(QStringLiteral("$body body \"crate.smd\"")));
        CHECK(valve::sourceMaterial("dev-dev_measurewall01d", "x") == "dev/dev_measurewall01d");
        CHECK(valve::sourceMaterial("", "dev/graygrid") == "dev/graygrid");
        std::printf("[ ok ] sealing box, size warning, SMD and QC\n");
    }
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
