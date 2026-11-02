// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for file formats (needs QtGui for image codecs only).
#include "core/Components.h"
#include "core/Document.h"
#include "io/ArchMaterials.h"
#include "io/AssetLibrary.h"
#include "io/GltfExport.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "io/ObjIO.h"
#include "io/StlExport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
        // Texture projection axes (sculpted terrain) survive saving.
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}, true);
        for (Face& f : d.mesh().faces)
            if (f.alive) { f.front.axis = Vec3(0, 0, 1); f.back.axis = Vec3(0, 0, -1); }
        QString p = dir.filePath("axis.gsketch");
        CHECK(saveNative(d, p, {}));
        Document e;
        CHECK(loadNative(e, p, nullptr));
        int axes = 0;
        for (const Face& f : e.mesh().faces)
            axes += f.alive && f.front.axis.nearlyEquals(Vec3(0, 0, 1)) && f.back.axis.nearlyEquals(Vec3(0, 0, -1));
        CHECK(axes == 1);
    }

    {
        // Texture Brush paint weights survive saving.
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}, true);
        d.mesh().vertices[0].paint = {{0, 0.75f}, {1, 0.125f}};
        Vec3 p0 = d.mesh().vertices[0].p;
        QString p = dir.filePath("paint.gsketch");
        CHECK(saveNative(d, p, {}));
        Document e;
        CHECK(loadNative(e, p, nullptr));
        int found = 0;
        for (const Vertex& v : e.mesh().vertices)
            if (v.alive && v.p.nearlyEquals(p0) && v.paint.size() == 2 && v.paint[0].material == 0 &&
                std::fabs(v.paint[0].weight - 0.75f) < 1e-6 && v.paint[1].material == 1)
                ++found;
            else if (v.alive && !v.paint.empty())
                found = -100;
        CHECK(found == 1);
    }

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
    {
        // Components survive save/load (also when saved while one is open) and
        // are exported as real geometry.
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, true);
        Selection s;
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive) s.faces.insert(f);
        Id a = makeComponent(d.state, s, "Tile");
        Id b = cloneInstance(d.state, a);
        d.state.instances[b].xf = Mat4::translation(Vec3(3, 0, 0)) * d.state.instances[b].xf;
        Id dead = cloneInstance(d.state, a);
        d.state.instances[dead].alive = false;
        d.openComponent(b);
        QString p = dir.filePath("comp.gsketch");
        CHECK(saveNative(d, p, {}));
        CHECK(d.editingComponent()); // saving does not close the open component
        Document e;
        CHECK(loadNative(e, p, nullptr));
        CHECK(e.state.defs.size() == 1 && e.state.instances.size() == 2 && e.state.editing == kNone);
        CHECK(e.state.defs[0].name == "Tile" && e.state.defs[0].uid == d.state.defs[0].uid);
        Vec3 mn, mx;
        CHECK(instanceBounds(e.state, 1, mn, mx));
        CHECK(mn.nearlyEquals(Vec3(3, 0, 0), 1e-9) && mx.nearlyEquals(Vec3(4, 1, 0), 1e-9));
        Mesh flat = flattenModel(e.state);
        CHECK(flat.aliveFaceCount() == 2);
        CHECK(exportStl(e, dir.filePath("comp.stl")));
        CHECK(QFile(dir.filePath("comp.stl")).size() == 84 + 50 * 4);
        std::printf("[ ok ] components save/load/export\n");
    }
    {
        // PBR materials, custom shaders, lights and cameras survive saving.
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::blue);
        Document d;
        Material m;
        m.name = "Fancy";
        m.pbr = true;
        m.baseStrength = 0.75f;
        m.texture = textureFromImage(img);
        m.emission = Color(0.5f, 0.25f, 0);
        m.flipNormalY = true;
        m.blend = true;
        for (int i = 0; i < MapSlotCount; ++i) {
            m.maps[i].tex = textureFromImage(img);
            m.maps[i].strength = 0.1f * (i + 1);
        }
        m.setTileSize(2, 3);
        m.shaderOn = true;
        m.shader = "void surface(inout Surface s) { s.albedo = vec3(1.0); }\n// äö\n";
        m.shaderProps["_Tint"] = {0.1f, 0.2f, 0.3f, 1};
        d.addMaterial(m);
        SceneObject spot;
        spot.kind = SceneObject::SpotLight;
        spot.name = "Key";
        spot.pos = Vec3(1, 2, 3);
        spot.target = Vec3(0, 0, 0);
        spot.color = Color(1, 0.5f, 0.25f);
        spot.intensity = 7;
        spot.range = 9;
        spot.coneAngle = 33;
        spot.softness = 0.5;
        spot.enabled = false;
        d.addObject(spot);
        SceneObject cam;
        cam.kind = SceneObject::ViewCamera;
        cam.name = "Shot";
        cam.pos = Vec3(5, 5, 5);
        cam.target = Vec3(1, 1, 1);
        cam.fov = 60;
        cam.perspective = false;
        d.addObject(cam);
        SceneObject gone = cam;
        gone.alive = false;
        d.state.objects.push_back(gone);

        QString p = dir.filePath("pbr.gsketch");
        CHECK(saveNative(d, p, {}));
        Document e;
        CHECK(loadNative(e, p, nullptr));
        CHECK(e.state.materials.size() == 1);
        CHECK(sameMaterial(e.state.materials[0], d.state.materials[0]));
        CHECK(e.state.objects.size() == 2); // deleted objects are not saved
        const SceneObject& s0 = e.state.objects[0];
        CHECK(s0.kind == SceneObject::SpotLight && s0.name == "Key" && s0.pos.nearlyEquals(Vec3(1, 2, 3), 1e-9) && !s0.enabled);
        CHECK(s0.intensity == 7 && s0.range == 9 && s0.coneAngle == 33 && s0.softness == 0.5);
        const SceneObject& s1 = e.state.objects[1];
        CHECK(s1.kind == SceneObject::ViewCamera && s1.fov == 60 && !s1.perspective && s1.target.nearlyEquals(Vec3(1, 1, 1), 1e-9));

        // A plain material stays small in the file.
        Material plain;
        plain.name = "Plain";
        const QJsonObject pj = materialToJson(plain);
        CHECK(!pj.contains("maps") && !pj.contains("pbr") && !pj.contains("shader"));
        std::printf("[ ok ] PBR materials, lights and cameras save/load\n");

        // glTF: the maps, lights and cameras come along.
        Document g;
        g.mesh().drawPolyline({{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}}, true);
        Material gm;
        gm.name = "G";
        gm.maps[MapMetallic].tex = textureFromImage(img);
        gm.maps[MapRoughness].tex = textureFromImage(img);
        gm.maps[MapRoughness].strength = 0.5f;
        gm.maps[MapNormal].tex = textureFromImage(img);
        gm.maps[MapOcclusion].tex = textureFromImage(img);
        gm.emission = Color(1, 1, 0);
        const Id gid = g.addMaterial(gm);
        for (Face& f : g.mesh().faces)
            if (f.alive) f.front.material = gid;
        spot.enabled = true;
        g.addObject(spot);
        g.addObject(cam);
        const QString gp = dir.filePath("pbr.gltf");
        CHECK(exportGltf(g, gp));
        QFile gf(gp);
        CHECK(gf.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(gf.readAll()).object();
        const QJsonObject mat = root["materials"].toArray()[0].toObject();
        const QJsonObject pbr = mat["pbrMetallicRoughness"].toObject();
        CHECK(pbr.contains("metallicRoughnessTexture") && pbr["roughnessFactor"].toDouble() == 0.5);
        CHECK(mat.contains("normalTexture") && mat.contains("occlusionTexture"));
        CHECK(mat["emissiveFactor"].toArray().size() == 3);
        CHECK(root["images"].toArray().size() == 3);
        CHECK(root["extensionsUsed"].toArray().contains(QStringLiteral("KHR_lights_punctual")));
        const QJsonArray lights = root["extensions"].toObject()["KHR_lights_punctual"].toObject()["lights"].toArray();
        CHECK(lights.size() == 1 && lights[0].toObject()["type"].toString() == "spot");
        CHECK(root["cameras"].toArray().size() == 1 && root["cameras"].toArray()[0].toObject()["type"].toString() == "orthographic");
        CHECK(root["nodes"].toArray().size() == 3 && root["scenes"].toArray()[0].toObject()["nodes"].toArray().size() == 3);
        // The spot aims from (1,2,3) to the origin; its node's -Z must point that way (Y-up axes).
        const QJsonObject lightNode = root["nodes"].toArray()[1].toObject();
        const QJsonArray q = lightNode["rotation"].toArray();
        CHECK(q.size() == 4);
        {
            const double x = q[0].toDouble(), y = q[1].toDouble(), z = q[2].toDouble(), w = q[3].toDouble();
            CHECK(std::fabs(x * x + y * y + z * z + w * w - 1) < 1e-9);
            const Vec3 aim(-2 * (x * z + w * y), -2 * (y * z - w * x), -(1 - 2 * (x * x + y * y)));
            CHECK(aim.nearlyEquals(Vec3(-1, -3, 2).normalized(), 1e-9));
        }
        std::printf("[ ok ] glTF PBR, lights and cameras\n");
    }

    {
        // The standard place: <XDG data home>/GNUSketch/prefabs.
        {
            QTemporaryDir xdg;
            const QByteArray old = qgetenv("XDG_DATA_HOME");
            qputenv("XDG_DATA_HOME", xdg.path().toUtf8());
            CHECK(assetDir("prefabs") == QDir(xdg.path()).filePath("GNUSketch/prefabs"));
            CHECK(QFileInfo(assetDir("prefabs")).isDir());
            qputenv("XDG_DATA_HOME", old);
        }
        // Prefabs live in the data directory, as ordinary model files.
        QTemporaryDir data;
        qputenv("GNUSKETCH_DATA_DIR", data.path().toUtf8());
        CHECK(prefabFiles().isEmpty());
        CHECK(assetDir("prefabs").startsWith(data.path()));
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, true);
        Material mat;
        mat.name = "Oak";
        mat.pbr = true;
        const Id mid = d.addMaterial(mat);
        Selection sel;
        for (Id f = 0; f < (Id)d.mesh().faces.size(); ++f)
            if (d.mesh().faces[f].alive) {
                d.mesh().faces[f].front.material = mid;
                sel.faces.insert(f);
            }
        const Id inst = makeComponent(d.state, sel, "Table");
        const Id def = d.state.instances[inst].def;
        QImage thumb(8, 8, QImage::Format_RGB32);
        thumb.fill(Qt::green);
        QString err;
        const QString p1 = savePrefab(d.state, def, "Table", thumb, &err);
        CHECK(!p1.isEmpty() && p1.endsWith("/prefabs/Table.gsketch"));
        // Never overwrites; odd names stay inside the folder.
        const QString p2 = savePrefab(d.state, def, "Table", thumb);
        CHECK(p2.endsWith("/Table 2.gsketch") && QFile::exists(p1));
        const QString p3 = savePrefab(d.state, def, "../../evil/na:me", {});
        CHECK(QFileInfo(p3).absolutePath() == QFileInfo(p1).absolutePath());
        CHECK(prefabFiles().size() == 3);
        ModelState st;
        QString name;
        QImage back;
        CHECK(loadPrefab(p1, st, &name, &back, &err));
        CHECK(name == "Table" && back.width() == 8 && back.pixel(0, 0) == thumb.pixel(0, 0));
        CHECK(st.defs.size() == 1 && st.instances.size() == 1 && st.materials.size() == 1);
        CHECK(st.defs[0].uid == d.state.defs[def].uid);
        // It is a normal model that can be opened.
        Document opened;
        CHECK(loadNative(opened, p1, nullptr));
        CHECK(opened.state.instances.size() == 1);
        // Any other model dropped into the folder works too.
        Document loose;
        loose.mesh().drawPolyline({{5, 5, 1}, {6, 5, 1}, {6, 6, 1}, {5, 6, 1}}, true);
        const QString p4 = QDir(assetDir("prefabs")).filePath("Handmade.gsketch");
        CHECK(saveNative(loose, p4, {}));
        ModelState hand, hand2;
        CHECK(loadPrefab(p4, hand, &name));
        CHECK(loadPrefab(p4, hand2));
        CHECK(name == "Handmade" && hand.defs.size() == 1 && hand.defs[0].uid == hand2.defs[0].uid);
        Vec3 mn, mx;
        bool any = false;
        hand.defs[0].mesh.bounds(mn, mx, any);
        CHECK(any && mn.nearlyEquals(Vec3(0, 0, 0), 1e-9) && mx.nearlyEquals(Vec3(1, 1, 0), 1e-9));
        // Nothing to place.
        Document empty;
        const QString p5 = QDir(assetDir("prefabs")).filePath("Empty.gsketch");
        CHECK(saveNative(empty, p5, {}));
        CHECK(!loadPrefab(p5, hand, nullptr, nullptr, &err) && err.contains("no geometry"));
        CHECK(!loadPrefab(dir.filePath("missing.gsketch"), hand));
        std::printf("[ ok ] prefab files\n");

        // Material library.
        Material lib;
        lib.name = "Gold/Leaf";
        lib.pbr = true;
        lib.maps[MapMetallic].strength = 1;
        const QString mp = saveLibraryMaterial(lib);
        CHECK(!mp.isEmpty() && libraryMaterialFiles().contains(mp));
        Material lb;
        CHECK(loadLibraryMaterial(mp, lb) && sameMaterial(lb, lib));
        CHECK(!loadLibraryMaterial(p1, lb)); // a model is not a material
        CHECK(!builtinPbrMaterials().empty());

        // A material from a set of image files.
        QImage red(4, 2, QImage::Format_RGB32);
        red.fill(Qt::red);
        QStringList files;
        for (const char* n : {"oak_albedo.png", "oak_Normal.png", "oak_Roughness.png", "oak_Metallic_2k.png"}) {
            files << dir.filePath(n);
            CHECK(red.save(files.back()));
        }
        Material fromFiles;
        CHECK(materialFromImages(files, fromFiles, &err));
        CHECK(fromFiles.name == "oak" && fromFiles.pbr && fromFiles.hasTexture());
        CHECK(fromFiles.hasMap(MapNormal) && fromFiles.hasMap(MapRoughness) && fromFiles.hasMap(MapMetallic) && !fromFiles.hasMap(MapHeight));
        CHECK(fromFiles.maps[MapMetallic].strength == 1 && fromFiles.maps[MapRoughness].strength == 1);
        Material none;
        CHECK(!materialFromImages({dir.filePath("nope.png")}, none, &err));
        // Image files of the textures folder are found.
        CHECK(red.save(QDir(assetDir("textures")).filePath("wall.png")));
        CHECK(textureFiles({}).size() == 1);
        CHECK(textureFiles({dir.path()}).contains(files[0]));
        qunsetenv("GNUSKETCH_DATA_DIR");
        std::printf("[ ok ] material library\n");
    }
    {
        // The architectural materials: real maps, sizes in meters, tiling edges match.
        CHECK(architecturalMaterialCount() >= 12);
        for (int i = 0; i < architecturalMaterialCount(); ++i) {
            const Material& m = architecturalMaterial(i);
            CHECK(m.name == architecturalMaterialName(i) && m.pbr && m.hasTexture());
            CHECK(m.hasMap(MapNormal) && m.hasMap(MapRoughness));
            double w, h;
            m.tileSize(w, h);
            CHECK(w >= 0.5 && w <= 3 && h >= 0.5 && h <= 3);
            CHECK(&architecturalMaterial(i) == &m);
            const QImage base = decodeTexture(m.texture);
            CHECK(!base.isNull() && base.width() == 512);
            const QImage nrm = decodeTexture(m.maps[MapNormal].tex);
            CHECK(qBlue(nrm.pixel(100, 100)) > 128); // points out of the surface
            CHECK(m.texture.data->size() < 400 * 1024); // keeps saved models small
        }
        Document d;
        for (int i = 0; i < architecturalMaterialCount(); ++i) d.addMaterial(architecturalMaterial(i));
        ModelState back;
        CHECK(modelFromJson(modelToJson(d.state), back) && back.materials.size() == d.state.materials.size());
        CHECK(sameMaterial(back.materials[0], d.state.materials[0]));
        std::printf("[ ok ] architectural materials\n");
    }
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
