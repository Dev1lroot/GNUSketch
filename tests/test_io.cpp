// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for file formats (needs QtGui for image codecs only).
#include "core/Document.h"
#include "io/GltfExport.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "io/ObjIO.h"
#include "io/StlExport.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace gs;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) ++g_passed;                                                          \
        else { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void makeModel(Document& d) {
    Mesh& m = d.mesh();
    m.drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}, true);
    m.drawPolyline({{0.5, 0.25, 0}, {1.5, 0.25, 0}, {1.5, 0.75, 0}, {0.5, 0.75, 0}}, true);
    QImage img(8, 4, QImage::Format_RGB32);
    img.fill(Qt::red);
    Material mat;
    mat.name = "Red Brick";
    mat.texture = textureFromImage(img);
    Id mid = d.addMaterial(mat);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) {
            m.faces[f].front.material = mid;
            m.faces[f].back.tex = fitTextureToFace(m, f, false);
        }
    m.drawLine({3, 0, 0}, {3, 0, 1}); // loose edge
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;

    {
        Document d;
        makeModel(d);
        QString p = dir.filePath("a.gsketch");
        QJsonObject view;
        view["test"] = 42;
        CHECK(saveNative(d, p, view));
        Document e;
        QJsonObject v2;
        QString err;
        CHECK(loadNative(e, p, &v2, &err));
        CHECK(v2["test"].toInt() == 42);
        CHECK(e.mesh().aliveFaceCount() == 2);
        CHECK(e.mesh().aliveEdgeCount() == 9);
        CHECK(e.state.materials.size() == 1);
        CHECK(e.state.materials[0].hasTexture());
        CHECK(e.state.materials[0].texture.pixelWidth == 8);
        int holes = 0, texd = 0;
        for (const Face& f : e.mesh().faces) {
            holes += (int)f.holes.size();
            texd += f.back.tex.valid;
        }
        CHECK(holes == 1);
        CHECK(texd == 2);
        // Editing after load still works (lookup tables rebuilt).
        e.mesh().drawLine({1, 0, 0}, {1, 0.25, 0});
        e.mesh().drawLine({1, 0.75, 0}, {1, 1, 0});
        CHECK(e.mesh().aliveFaceCount() == 3);
        std::printf("[ ok ] native roundtrip\n");
    }
    {
        Document d;
        makeModel(d);
        QString p = dir.filePath("b.obj");
        CHECK(exportObj(d, p));
        CHECK(QFile::exists(dir.filePath("b.mtl")));
        CHECK(QFile::exists(dir.filePath("b_Red_Brick.png")));
        Document e;
        int n = importObj(e, p);
        CHECK(n >= 2);
        CHECK(e.state.materials.size() == 2); // gs_default + Red_Brick
        bool tex = false;
        for (const auto& m : e.state.materials) tex |= m.hasTexture();
        CHECK(tex);
        Vec3 mn, mx;
        bool any;
        e.mesh().bounds(mn, mx, any);
        CHECK(any && std::fabs(mx.x - 3) < 1e-6 && std::fabs(mx.y - 1) < 1e-6 && std::fabs(mx.z - 1) < 1e-6);
        std::printf("[ ok ] obj roundtrip\n");
    }
    {
        Document d;
        makeModel(d);
        QString p = dir.filePath("c.gltf");
        CHECK(exportGltf(d, p));
        QFile f(p);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonDocument jd = QJsonDocument::fromJson(f.readAll());
        CHECK(jd.isObject());
        CHECK(jd.object()["asset"].toObject()["version"].toString() == "2.0");
        CHECK(exportStl(d, dir.filePath("d.stl")));
        CHECK(QFile(dir.filePath("d.stl")).size() == 84 + 50 * (2 + 8));
        std::printf("[ ok ] gltf/stl export\n");
    }
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
