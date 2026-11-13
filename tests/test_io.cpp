// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for file formats (needs QtGui for image codecs only).
#include "core/Components.h"
#include "core/Document.h"
#include "io/ArchMaterials.h"
#include "io/AssetLibrary.h"
#include "io/EntityScript.h"
#include "core/Animation.h"
#include "io/GltfExport.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "io/ObjIO.h"
#include "io/StlExport.h"
#include "io/PrintExport.h"
#include "io/PrintImport.h"
#include "core/Solids.h"

#include <QCoreApplication>
#include <cstring>
#include <map>
#include <QDir>
#include <QElapsedTimer>
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

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) < tol; }

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
    {
        // A door panel and a button, the way the Properties window sets them up.
        auto makeThing = [](ModelState& st, const Vec3& at, const Vec3& size, const Vec3& moved, const char* cls, const std::string& name,
                            const std::string& target, const QString& script = {}) {
            ComponentDef def;
            def.name = cls;
            def.uid = name + "uid";
            def.mesh.drawPolyline({{0, 0, 0}, {size.x, 0, 0}, {size.x, 0, size.z}, {0, 0, size.z}}, true);
            AnimState a, b;
            a.name = "A";
            b.name = "B";
            for (Id v = 0; v < (Id)def.mesh.vertices.size(); ++v) {
                a.verts.push_back(v);
                a.pos.push_back(def.mesh.vertices[v].p);
                b.verts.push_back(v);
                b.pos.push_back(def.mesh.vertices[v].p + moved);
            }
            def.states = {a, b};
            st.defs.push_back(def);
            Instance in;
            in.def = (Id)st.defs.size() - 1;
            in.xf = Mat4::translation(at);
            in.entity.classname = cls;
            in.entity.targetname = name;
            if (!target.empty()) in.entity.targets.push_back({target, 0});
            in.entity.script = script.toStdString();
            st.instances.push_back(in);
            return (int)st.instances.size() - 1;
        };
        auto heightOf = [](const ModelState& st, const AnimEntity& e) { return st.instances[e.parts[0].instance].xf.at(2, 3); };

        // Saved and loaded, with a mesh that gets renumbered on the way.
        {
            Document d;
            Mesh& m = d.mesh();
            (void)m;
            makeThing(d.state, Vec3(1, 2, 3), Vec3(1, 0, 2), Vec3(0, 0, 2), "func_door", "gate", "next", QStringLiteral("// mine\n"));
            ComponentDef& def = d.state.defs[0];
            // A dead vertex at the start: the file holds the mesh compacted.
            def.mesh.vertices.insert(def.mesh.vertices.begin(), def.mesh.vertices[0]);
            def.mesh.vertices[0].alive = false;
            for (Edge& e : def.mesh.edges) { ++e.v[0]; ++e.v[1]; }
            for (Face& f : def.mesh.faces) for (Id& v : f.outer) ++v;
            for (AnimState& s : def.states) for (Id& v : s.verts) ++v;
            def.mesh.touch();
            d.state.instances[0].entity.startState = "B";
            ModelState back;
            CHECK(modelFromJson(modelToJson(d.state), back));
            CHECK(back.defs.size() == 1 && back.defs[0].states.size() == 2 && back.defs[0].states[1].name == "B");
            const AnimState& s = back.defs[0].states[1];
            CHECK(s.verts.size() == 4);
            bool match = true;
            for (size_t k = 0; k < s.verts.size(); ++k) match = match && s.pos[k].nearlyEquals(back.defs[0].mesh.pos(s.verts[k]) + Vec3(0, 0, 2), 1e-9);
            CHECK(match);
            const EntityProps& e = back.instances[0].entity;
            CHECK(e.classname == "func_door" && e.targetname == "gate" && e.targets.size() == 1 && e.targets[0].name == "next" && e.startState == "B" && e.script == "// mine\n");
            // And in a prefab.
            QTemporaryDir data;
            qputenv("GNUSKETCH_DATA_DIR", data.path().toUtf8());
            const QString path = savePrefab(back, 0, "Gate", {}, nullptr, e);
            ModelState pre;
            CHECK(loadPrefab(path, pre) && pre.defs[0].states.size() == 2 && pre.instances[0].entity == e);
            qunsetenv("GNUSKETCH_DATA_DIR");
            std::printf("[ ok ] states and entities save/load\n");
        }

        // A linked door opens by its button only; an unlinked one by touch and use.
        ModelState st;
        makeThing(st, Vec3(0, 0, 0), Vec3(1, 0, 2), Vec3(0, 0, 2), "func_door", "gate", "");
        makeThing(st, Vec3(3, 0, 0), Vec3(0.2, 0, 0.2), Vec3(0, 0.05, 0), "func_button", "btn", "gate");
        makeThing(st, Vec3(6, 0, 0), Vec3(1, 0, 2), Vec3(0, 0, 2), "func_door", "free", "");
        EntityWorld world(st);
        CHECK(world.count() == 3);
        CollisionWorld col;
        col.buildFromModel(st);
        world.start(col);
        CHECK(world.errors().isEmpty());
        if (!world.errors().isEmpty()) std::printf("%s\n", qPrintable(world.errors().join('\n')));
        CHECK(world.linked(0) && !world.linked(1) && !world.linked(2));
        CHECK(!world.usable(0) && world.usable(1) && world.usable(2));
        CHECK(col.bodyCount() == 3);
        auto run = [&](double seconds) {
            for (double t = 0; t < seconds; t += 0.02) world.update(0.02);
        };
        world.use(0);
        world.setTouching(0, true);
        run(0.5);
        CHECK(near(heightOf(st, world.entity(0)), 0)); // not by touch, not by use
        world.setTouching(0, false);
        world.use(1); // the button
        run(0.05);
        CHECK(near(heightOf(st, world.entity(0)), 0)); // the button first goes in
        run(0.5);
        const double midway = heightOf(st, world.entity(0));
        CHECK(midway > 0.05 && midway < 1.95);
        run(0.8);
        CHECK(near(heightOf(st, world.entity(0)), 2)); // open, 1 s after the button's press
        {
            // The collision body went along.
            Vec3 mn, mx;
            col.bodyBounds(world.entity(0).parts[0].body, mn, mx);
            CHECK(near(mn.z, 2, 1e-9));
        }
        run(4.0);
        CHECK(near(heightOf(st, world.entity(0)), 0)); // and closed itself
        world.setTouching(2, true);
        run(1.2);
        CHECK(near(heightOf(st, world.entity(2)), 2)); // the free door by touch
        world.setTouching(2, false);
        run(4.5);
        CHECK(near(heightOf(st, world.entity(2)), 0));
        world.use(2);
        run(1.2);
        CHECK(near(heightOf(st, world.entity(2)), 2)); // and by use
        std::printf("[ ok ] a door follows its button, as in Hammer\n");

        // The whole API, used by a script.
        {
            ModelState s2;
            makeThing(s2, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "a", "b", QStringLiteral(R"JS(
var hits = 0, ticks = 0;
on("start", function () {
    if (self.name !== "a" || self.classname !== "func_actor" || self.target !== "b") throw new Error("bad fields");
    if (self.states.join() !== "A,B" || self.state !== "A" || self.moving) throw new Error("bad state");
    self.play(["B", "A"], { duration: 0.5, easing: "linear", loop: true });
});
on("tick", function (dt) { ticks += dt; });
every(0.25, function () { hits++; });
var once = after(0.1, function () { throw new Error("cancelled timers do not run"); });
cancel(once);
after(1.7, function () {
    if (hits < 5) throw new Error("every ran " + hits);
    if (Math.abs(ticks - game.time) > 0.05) throw new Error("ticks " + ticks);
    self.stop();
    self.fire(42);
});
)JS"));
            makeThing(s2, Vec3(5, 0, 0), Vec3(1, 0, 1), Vec3(0, 0, 3), "func_actor", "b", "", QStringLiteral(R"JS(
on("trigger", function (arg) {
    if (arg !== 42) throw new Error("bad arg");
    if (find("a").length !== 1 || find("a")[0].name !== "a" || find("none").length) throw new Error("bad find");
    self.set("B");
});
)JS"));
            EntityWorld w2(s2);
            CollisionWorld c2;
            c2.buildFromModel(s2);
            w2.start(c2);
            for (int i = 0; i < 40; ++i) w2.update(0.02); // 0.8 s: on the way back
            const double h = heightOf(s2, w2.entity(0));
            CHECK(near(h, 0.4, 0.05)); // up in 0.5 s, then 0.3 s of the way down
            for (int i = 0; i < 60; ++i) w2.update(0.02);
            CHECK(near(heightOf(s2, w2.entity(1)), 3)); // fired, set
            const double stopped = heightOf(s2, w2.entity(0));
            for (int i = 0; i < 10; ++i) w2.update(0.02);
            CHECK(near(heightOf(s2, w2.entity(0)), stopped)); // stop() holds still
            CHECK(w2.errors().isEmpty());
            if (!w2.errors().isEmpty()) std::printf("%s\n", qPrintable(w2.errors().join('\n')));
            std::printf("[ ok ] the script API\n");
        }

        // Broken scripts report, and do not take the world down.
        {
            ModelState s3;
            makeThing(s3, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "syntax", "", QStringLiteral("var a = ;\n"));
            makeThing(s3, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "unknown", "", QStringLiteral("on(\"start\", function () { self.tween(\"NOPE\", 1); });"));
            makeThing(s3, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "fine", "", QStringLiteral("on(\"start\", function () { self.tween(\"B\", 0.1); });"));
            makeThing(s3, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "stuck", "", QStringLiteral("on(\"use\", function () { while (true) {} });"));
            EntityWorld w3(s3);
            CollisionWorld c3;
            c3.buildFromModel(s3);
            w3.start(c3);
            for (int i = 0; i < 20; ++i) w3.update(0.02);
            CHECK(w3.errors().size() == 2);
            CHECK(w3.errors().join(' ').contains("syntax") && w3.errors().join(' ').contains("NOPE"));
            CHECK(near(heightOf(s3, w3.entity(2)), 1)); // the good one still runs
            QElapsedTimer timer;
            timer.start();
            w3.use(3); // an endless loop: interrupted, not a frozen game
            CHECK(timer.elapsed() < 5000);
            for (int i = 0; i < 5; ++i) w3.update(0.02);
            std::printf("[ ok ] broken scripts\n");
        }
        // Every preset parses and the check reports lines.
        CHECK(!scriptPresets().empty());
        for (const ScriptPreset& p : scriptPresets()) {
            QString err;
            CHECK(checkEntityScript(p.code, &err));
            if (!err.isEmpty()) std::printf("%s: %s\n", qPrintable(p.name), qPrintable(err));
        }
        for (const char* cls : {"func_door", "func_button", "func_actor"}) CHECK(!defaultScript(cls).isEmpty());
        QString err;
        CHECK(!checkEntityScript(QStringLiteral("var ok = 1;\nvar = 2;\n"), &err));
        CHECK(err.startsWith(QStringLiteral("Line 2")));
        // No way out of the sandbox.
        {
            ModelState s4;
            makeThing(s4, Vec3(), Vec3(1, 0, 1), Vec3(0, 0, 1), "func_actor", "x", "", QStringLiteral(
                "if (typeof Qt !== 'undefined' || typeof require !== 'undefined' || typeof process !== 'undefined' || typeof XMLHttpRequest !== 'undefined') throw new Error('leak');"));
            EntityWorld w4(s4);
            CollisionWorld c4;
            c4.buildFromModel(s4);
            w4.start(c4);
            CHECK(w4.errors().isEmpty());
        }
        // User presets.
        QTemporaryDir data;
        qputenv("GNUSKETCH_DATA_DIR", data.path().toUtf8());
        const size_t n0 = scriptPresets().size();
        CHECK(!saveScriptPreset("My/Door", "func_door", "// hi\n").isEmpty());
        const auto presets = scriptPresets();
        CHECK(presets.size() == n0 + 1 && presets.back().name == "My_Door" && presets.back().classname == "func_door" && !presets.back().builtIn);
        qunsetenv("GNUSKETCH_DATA_DIR");
        std::printf("[ ok ] presets\n");
    }
    {
        // The whole door: a frame prefab with a scripted door inside, which has a handle.
        auto plate = [](ModelState& st, const char* name, const Vec3& o, const Vec3& size) {
            ComponentDef d;
            d.name = name;
            d.uid = std::string("uid-") + name;
            d.mesh.drawPolyline({o, o + Vec3(size.x, 0, 0), o + Vec3(size.x, 0, size.z), o + Vec3(0, 0, size.z)}, true);
            st.defs.push_back(d);
            return (Id)st.defs.size() - 1;
        };
        Document d;
        ModelState& st = d.state;
        const Id frame = plate(st, "Frame", Vec3(-1, 0, 0), Vec3(0.5, 0, 2));
        const Id doorDef = plate(st, "Door", Vec3(0, 0, 0), Vec3(1, 0, 2));
        const Id handleDef = plate(st, "Handle", Vec3(0.8, 0, 1), Vec3(0.1, 0, 0.1));
        Instance root;
        root.def = frame;
        root.xf = Mat4::translation(Vec3(4, 0, 0));
        st.instances.push_back(root);
        Instance door;
        door.def = doorDef;
        door.parent = frame;
        door.entity.classname = "func_door";
        door.entity.targetname = "door";
        st.instances.push_back(door);
        Instance handle;
        handle.def = handleDef;
        handle.parent = doorDef;
        st.instances.push_back(handle);
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
        open.children.emplace_back(2, Mat4::translation(Vec3(0, 0, 2)));
        st.defs[doorDef].states = {closed, open};

        QTemporaryDir data;
        qputenv("GNUSKETCH_DATA_DIR", data.path().toUtf8());
        const QString path = savePrefab(st, frame, "Whole Door", {});
        ModelState pre;
        QString err, name;
        CHECK(loadPrefab(path, pre, &name, nullptr, &err));
        CHECK(name == "Whole Door" && pre.defs.size() == 3 && pre.instances.size() == 3);
        CHECK(pre.instances[0].parent == kNone && pre.instances[0].def == 0);
        CHECK(pre.defs[1].states.size() == 2 && pre.defs[1].states[1].children.size() == 1);
        const Id handleInPrefab = pre.defs[1].states[1].children[0].first;
        CHECK(handleInPrefab >= 0 && pre.instances[handleInPrefab].parent == 1);
        CHECK(pre.instances[1].entity.classname == "func_door" || pre.instances[2].entity.classname == "func_door");
        qunsetenv("GNUSKETCH_DATA_DIR");

        // Placed in a new model next to a button that sets off two things, the second later.
        Document w;
        const Id placedDef = importDefinition(w, pre, 0);
        Instance placed;
        placed.def = placedDef;
        placed.xf = Mat4::translation(Vec3(10, 0, 0));
        w.state.instances.push_back(placed);
        const Id btnDef = plate(w.state, "Button", Vec3(0, 0, 1), Vec3(0.2, 0, 0.2));
        AnimState up, down;
        up.name = "UP";
        down.name = "DOWN";
        for (Id v = 0; v < (Id)w.state.defs[btnDef].mesh.vertices.size(); ++v) {
            up.verts.push_back(v);
            up.pos.push_back(w.state.defs[btnDef].mesh.vertices[v].p);
            down.verts.push_back(v);
            down.pos.push_back(w.state.defs[btnDef].mesh.vertices[v].p + Vec3(0, 0.05, 0));
        }
        w.state.defs[btnDef].states = {up, down};
        Instance btn;
        btn.def = btnDef;
        btn.entity.classname = "func_button";
        btn.entity.targets = {{"door", 0}, {"lamp", 1.5}};
        w.state.instances.push_back(btn);
        const Id lampDef = plate(w.state, "Lamp", Vec3(0, 0, 0), Vec3(1, 0, 1));
        w.state.defs[lampDef].states = {up, down};
        for (AnimState& as : w.state.defs[lampDef].states) {
            as.verts = {0, 1, 2, 3};
            as.pos.resize(4);
            for (int k = 0; k < 4; ++k) as.pos[k] = w.state.defs[lampDef].mesh.vertices[k].p + (as.name == "DOWN" ? Vec3(0, 0, 5) : Vec3());
        }
        Instance lamp;
        lamp.def = lampDef;
        lamp.xf = Mat4::translation(Vec3(20, 0, 0));
        lamp.entity.classname = "func_actor";
        lamp.entity.targetname = "lamp";
        lamp.entity.script = "on(\"trigger\", function () { self.set(\"DOWN\"); });";
        w.state.instances.push_back(lamp);
        // Saved and loaded, as Game Mode does.
        ModelState game;
        CHECK(modelFromJson(modelToJson(w.state), game));
        EntityWorld world(game);
        CHECK(world.count() == 3);
        int doorE = -1, btnE = -1, lampE = -1;
        for (int i = 0; i < world.count(); ++i) {
            const std::string& c = world.entity(i).props.classname;
            (c == "func_door" ? doorE : c == "func_button" ? btnE : lampE) = i;
        }
        CollisionWorld col;
        col.buildFromModel(game);
        world.start(col);
        CHECK(world.errors().isEmpty());
        if (!world.errors().isEmpty()) std::printf("%s\n", qPrintable(world.errors().join('\n')));
        CHECK(world.linked(doorE) && world.linked(lampE) && !world.usable(doorE) && world.usable(btnE));
        // The frame stays static; the door is at the placed spot.
        auto z0 = [&](int e, int part) { return game.instances[world.entity(e).parts[part].instance].xf.at(2, 3); };
        CHECK(world.entity(doorE).parts.size() == 2);
        CHECK(near(game.instances[world.entity(doorE).parts[0].instance].xf.at(0, 3), 10));
        auto run = [&](double s) { for (double t = 0; t < s; t += 0.02) world.update(0.02); };
        world.use(doorE);
        run(0.3);
        CHECK(near(z0(doorE, 0), 0)); // linked: the button only
        world.use(btnE);
        run(1.3); // pressed at 0.12, the door is up 1 s later
        CHECK(near(z0(doorE, 0), 2));
        CHECK(near(z0(doorE, 1), 2)); // and the handle in it
        CHECK(near(z0(lampE, 0), 0)); // the lamp waits for its delay
        run(0.5);
        CHECK(near(game.defs.empty() ? 0 : game.instances[world.entity(lampE).parts[0].instance].xf.transformPoint(Vec3()).z, 5)); // 1.5 s after the press
        // self.targets is there for scripts, and the first still reads as .target.
        {
            ModelState s2;
            const Id dd = plate(s2, "A", Vec3(), Vec3(1, 0, 1));
            Instance a;
            a.def = dd;
            a.entity.classname = "func_actor";
            a.entity.targets = {{"x", 0.25}, {"y", 0}};
            a.entity.script = "if (self.target !== 'x' || self.targets.length !== 2 || self.targets[0].delay !== 0.25) throw new Error('bad targets');";
            s2.instances.push_back(a);
            EntityWorld w2(s2);
            CollisionWorld c2;
            c2.buildFromModel(s2);
            w2.start(c2);
            CHECK(w2.errors().isEmpty());
        }
        std::printf("[ ok ] a whole door prefab, a button with delayed targets\n");

        // One definition both on its own and inside another survives saving.
        {
            ModelState shared;
            const Id inner = plate(shared, "Inner", Vec3(), Vec3(1, 0, 1));
            const Id outer = plate(shared, "Outer", Vec3(0, 3, 0), Vec3(1, 0, 1));
            Instance alone;
            alone.def = inner;
            shared.instances.push_back(alone);
            Instance holder;
            holder.def = outer;
            shared.instances.push_back(holder);
            Instance nestedOne;
            nestedOne.def = inner;
            nestedOne.parent = outer;
            shared.instances.push_back(nestedOne);
            ModelState back;
            CHECK(modelFromJson(modelToJson(shared), back));
            CHECK(back.defs.size() == 2 && back.instances.size() == 3);
            CHECK(flattenModel(back).aliveFaceCount() == 3);
        }
        // Files written before there were several targets still load.
        {
            ModelState old;
            const Id dd = plate(old, "A", Vec3(), Vec3(1, 0, 1));
            Instance a;
            a.def = dd;
            a.entity.classname = "func_button";
            old.instances.push_back(a);
            QJsonObject root = QJsonDocument::fromJson(modelToJson(old)).object();
            QJsonArray insts = root["instances"].toArray();
            QJsonObject inst = insts[0].toObject(), ent = inst["entity"].toObject();
            ent["target"] = "door9";
            inst["entity"] = ent;
            insts[0] = inst;
            root["instances"] = insts;
            ModelState back;
            CHECK(modelFromJson(QJsonDocument(root).toJson(), back));
            CHECK(back.instances[0].entity.targets.size() == 1 && back.instances[0].entity.targets[0].name == "door9");
            // A file cannot make a component contain itself.
            inst["parent"] = 0;
            insts[0] = inst;
            root["instances"] = insts;
            ModelState evil;
            CHECK(modelFromJson(QJsonDocument(root).toJson(), evil));
            CHECK(evil.instances.empty());
        }
    }
    {
        // One group with a button and a door, placed twice: each button its own door.
        ModelState st;
        auto plate = [&](ModelState& m, const char* name, const Vec3& o) {
            ComponentDef d;
            d.name = name;
            d.mesh.drawPolyline({o, o + Vec3(1, 0, 0), o + Vec3(1, 0, 1), o + Vec3(0, 0, 1)}, true);
            AnimState a, b;
            a.name = "A";
            b.name = "B";
            for (Id v = 0; v < (Id)d.mesh.vertices.size(); ++v) {
                a.verts.push_back(v);
                a.pos.push_back(d.mesh.vertices[v].p);
                b.verts.push_back(v);
                b.pos.push_back(d.mesh.vertices[v].p + Vec3(0, 0, 3));
            }
            d.states = {a, b};
            m.defs.push_back(d);
            return (Id)m.defs.size() - 1;
        };
        ComponentDef groupDef;
        groupDef.name = "Room";
        st.defs.push_back(groupDef);
        const Id doorDef = plate(st, "Door", Vec3()), buttonDef = plate(st, "Button", Vec3(2, 0, 0));
        Instance door;
        door.def = doorDef;
        door.parent = 0;
        door.entity.classname = "func_door";
        door.entity.targetname = "door";
        st.instances.push_back(door);
        Instance button;
        button.def = buttonDef;
        button.parent = 0;
        button.entity.classname = "func_button";
        button.entity.targetname = "button";
        button.entity.targets = {{"door", 0}};
        st.instances.push_back(button);
        Instance roomA, roomB;
        roomA.def = roomB.def = 0;
        roomA.entity.targetname = "front";
        roomB.entity.targetname = "back";
        roomB.xf = Mat4::translation(Vec3(0, 20, 0));
        st.instances.push_back(roomA);
        st.instances.push_back(roomB);
        // And one outside, reaching into a named group.
        const Id m = plate(st, "Master", Vec3(50, 0, 0));
        Instance master;
        master.def = m;
        master.entity.classname = "func_button";
        master.entity.targets = {{"back/door", 0}};
        st.instances.push_back(master);
        ModelState game;
        CHECK(modelFromJson(modelToJson(st), game));
        EntityWorld world(game);
        CollisionWorld col;
        col.buildFromModel(game);
        world.start(col);
        CHECK(world.count() == 5);
        CHECK(world.errors().isEmpty());
        std::vector<int> doors, buttons;
        int masterE = -1;
        for (int i = 0; i < world.count(); ++i) {
            const AnimEntity& e = world.entity(i);
            if (e.props.classname == "func_door") doors.push_back(i);
            else if (e.props.targetname == "button") buttons.push_back(i);
            else masterE = i;
        }
        CHECK(doors.size() == 2 && buttons.size() == 2 && masterE >= 0);
        auto y = [&](int e) { return game.instances[world.entity(e).parts[0].instance].xf.at(1, 3); };
        auto z = [&](int e) { return game.instances[world.entity(e).parts[0].instance].xf.transformPoint(Vec3()).z; };
        const int front = y(doors[0]) < 10 ? doors[0] : doors[1], back = front == doors[0] ? doors[1] : doors[0];
        const int frontBtn = y(buttons[0]) < 10 ? buttons[0] : buttons[1];
        auto run = [&](double s) { for (double t = 0; t < s; t += 0.02) world.update(0.02); };
        world.use(frontBtn);
        run(1.3);
        CHECK(z(front) > 2.9);
        CHECK(z(back) < 0.1); // the other room's door did not move
        run(5);
        CHECK(z(front) < 0.1);
        world.use(masterE);
        run(1.3);
        CHECK(z(back) > 2.9);
        CHECK(z(front) < 0.1);
        std::printf("[ ok ] names are namespaced by their group\n");
    }
    {
        // Physics through the whole path: saved, loaded, falling in the game world.
        ModelState st;
        ComponentDef crate;
        crate.name = "Crate";
        const Vec3 o(0, 0, 5);
        crate.mesh.drawPolyline({o, o + Vec3(1, 0, 0), o + Vec3(1, 1, 0), o + Vec3(0, 1, 0)}, true);
        for (Face& f : crate.mesh.faces)
            if (f.alive && f.normal.z < 0) crate.mesh.reverseFace((Id)(&f - crate.mesh.faces.data()));
        crate.mesh.touch();
        st.defs.push_back(crate);
        Instance in;
        in.def = 0;
        in.entity.physics = true;
        in.entity.mass = 42;
        in.entity.friction = 0.3;
        in.entity.bounce = 0.2;
        st.instances.push_back(in);
        Instance still;
        still.def = 0;
        still.xf = Mat4::translation(Vec3(5, 0, 0));
        st.instances.push_back(still);
        ModelState game;
        CHECK(modelFromJson(modelToJson(st), game));
        CHECK(game.instances[0].entity.physics && game.instances[0].entity.mass == 42 && game.instances[0].entity.friction == 0.3);
        EntityWorld world(game);
        CollisionWorld col;
        col.buildFromModel(game);
        world.start(col);
        CHECK(world.count() == 1 && world.isPhysical(0) && !world.usable(0));
        CHECK(world.physics().count() == 1 && world.physics().mass(0) == 42);
        CHECK(col.triangles().size() == 2); // only the other one stays static
        for (int i = 0; i < 180; ++i) world.update(1.0 / 60);
        Vec3 mn, mx;
        col.bodyBounds(world.entity(0).parts[0].body, mn, mx);
        CHECK(near(mn.z, 0, 0.1)); // fell to the ground
        CHECK(world.errors().isEmpty());
        std::printf("[ ok ] physics in the game world\n");
    }
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);

    {
        // 3D printing exports keep the model's unit.
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {0.02, 0, 0}, {0.02, 0.01, 0}, {0, 0.01, 0}}, true); // 20 x 10 mm
        Id f = kNone;
        for (Id g = 0; g < (Id)d.mesh().faces.size(); ++g)
            if (d.mesh().faces[g].alive) f = g;
        d.mesh().pushPull(f, d.mesh().faces[f].normal.z < 0 ? -0.005 : 0.005); // 5 mm tall
        Material red;
        red.name = "Red & Hot";
        red.color = Color(1, 0, 0);
        d.materials().push_back(red);
        for (Face& F : d.mesh().faces) F.front.material = 0;
        const PrintMesh pm = buildPrintMesh(d, LengthUnit::Millimeters);
        CHECK(pm.verts.size() == 8 && pm.tris.size() == 12); // welded: a closed solid
        // Each edge is used by exactly two triangles, in opposite directions.
        std::map<std::pair<int, int>, int> uses;
        for (const auto& t : pm.tris)
            for (int k = 0; k < 3; ++k) uses[{t[k], t[(k + 1) % 3]}]++;
        bool manifold = true;
        for (const auto& [e, n] : uses) manifold = manifold && n == 1 && uses.count({e.second, e.first}) == 1;
        CHECK(manifold);
        double maxX = 0;
        for (const Vec3& v : pm.verts) maxX = std::max(maxX, v.x);
        CHECK(std::fabs(maxX - 20) < 1e-6);

        CHECK(exportPrint(d, dir.filePath("p.3mf"), PrintFormat::ThreeMF, LengthUnit::Millimeters));
        QFile z(dir.filePath("p.3mf"));
        CHECK(z.open(QIODevice::ReadOnly));
        const QByteArray zip = z.readAll();
        CHECK(zip.startsWith("PK\x03\x04"));
        CHECK(zip.contains("3D/3dmodel.model") && zip.contains("[Content_Types].xml") && zip.contains("_rels/.rels"));
        // An uncompressed copy says what the model says.
        CHECK(exportPrint(d, dir.filePath("p.amf"), PrintFormat::Amf, LengthUnit::Inches));
        QFile a(dir.filePath("p.amf"));
        CHECK(a.open(QIODevice::ReadOnly));
        const QByteArray amf = a.readAll();
        CHECK(amf.contains("unit=\"inch\"") && amf.contains("Red &amp; Hot"));
        CHECK(amf.contains("<x>0.787401575</x>")); // 20 mm in inches
        CHECK(exportPrint(d, dir.filePath("cm.amf"), PrintFormat::Amf, LengthUnit::Centimeters));
        QFile acm(dir.filePath("cm.amf"));
        CHECK(acm.open(QIODevice::ReadOnly) && acm.readAll().contains("unit=\"millimeter\"")); // AMF has no cm

        CHECK(exportPrint(d, dir.filePath("p.stl"), PrintFormat::StlBinary, LengthUnit::Centimeters));
        QFile st(dir.filePath("p.stl"));
        CHECK(st.open(QIODevice::ReadOnly));
        const QByteArray stl = st.readAll();
        CHECK(stl.size() == 84 + 50 * 12);
        CHECK(stl.left(80).contains("units=cm"));
        float far = 0;
        for (int i = 0; i < 12; ++i)
            for (int k = 0; k < 3; ++k) {
                float x;
                std::memcpy(&x, stl.constData() + 84 + i * 50 + 12 + k * 12, 4);
                far = std::max(far, x);
            }
        CHECK(std::fabs(far - 2.0f) < 1e-5); // 20 mm = 2 cm
        CHECK(exportPrint(d, dir.filePath("a.stl"), PrintFormat::StlAscii, LengthUnit::Millimeters));
        CHECK(exportPrint(d, dir.filePath("p.ply"), PrintFormat::Ply, LengthUnit::Millimeters));
        CHECK(exportPrint(d, dir.filePath("p.off"), PrintFormat::Off, LengthUnit::Millimeters));
        QFile off(dir.filePath("p.off"));
        CHECK(off.open(QIODevice::ReadOnly) && off.readAll().contains("\n8 12 0\n"));
        Document empty;
        QString err;
        CHECK(!exportPrint(empty, dir.filePath("e.3mf"), PrintFormat::ThreeMF, LengthUnit::Millimeters, &err) && !err.isEmpty());
        std::printf("[ ok ] 3D printing exports keep units\n");
    }

    {
        // 3D printing imports: what was exported comes back the same size.
        Document d;
        d.mesh().drawPolyline({{0, 0, 0}, {0.02, 0, 0}, {0.02, 0.01, 0}, {0, 0.01, 0}}, true);
        Id f = kNone;
        for (Id g = 0; g < (Id)d.mesh().faces.size(); ++g)
            if (d.mesh().faces[g].alive) f = g;
        d.mesh().pushPull(f, d.mesh().faces[f].normal.z < 0 ? -0.005 : 0.005);
        Material red;
        red.name = "Red";
        red.color = Color(1, 0, 0);
        d.materials().push_back(red);
        for (Face& F : d.mesh().faces) F.front.material = 0;
        struct Case { PrintFormat fmt; LengthUnit unit; const char* file; bool colours; };
        const Case cases[] = {{PrintFormat::ThreeMF, LengthUnit::Inches, "r.3mf", true}, {PrintFormat::Amf, LengthUnit::Millimeters, "r.amf", true},
                              {PrintFormat::StlBinary, LengthUnit::Centimeters, "r.stl", false}, {PrintFormat::StlAscii, LengthUnit::Millimeters, "ra.stl", false},
                              {PrintFormat::Ply, LengthUnit::Millimeters, "r.ply", true}, {PrintFormat::Off, LengthUnit::Meters, "r.off", false}};
        bool allOk = true;
        for (const Case& c : cases) {
            const QString path = dir.filePath(QString::fromLatin1(c.file));
            CHECK(exportPrint(d, path, c.fmt, c.unit));
            double unit = 0;
            CHECK(printFileUnit(path, unit)); // they all say their unit (ours carry it)
            PrintModel pm;
            QString err;
            const bool read = readPrintFile(path, 1.0, pm, &err);
            if (!read) std::printf("  %s: %s\n", c.file, qPrintable(err));
            CHECK(read && pm.tris.size() == 12);
            Vec3 mn(1e9, 1e9, 1e9), mx(-1e9, -1e9, -1e9);
            for (const Vec3& v : pm.verts)
                for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], v[k]), mx[k] = std::max(mx[k], v[k]);
            const bool sized = std::fabs(mx.x - mn.x - 0.02) < 1e-6 && std::fabs(mx.y - mn.y - 0.01) < 1e-6 && std::fabs(mx.z - mn.z - 0.005) < 1e-6;
            if (!sized) std::printf("  %s: size %g x %g x %g\n", c.file, mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
            CHECK(sized);
            if (c.colours) CHECK(pm.colors.size() == 1 && std::fabs(pm.colors[0].r - 1) < 1e-3 && pm.colors[0].g < 1e-3);
            Document e;
            e.beginOp("Import");
            const Id inst = importPrintModel(e, pm, "Box");
            e.commitOp();
            CHECK(inst != kNone);
            if (inst == kNone) { allOk = false; continue; }
            const Mesh& m = e.state.defs[e.state.instances[inst].def].mesh;
            // Welded and joined again: a box of six faces, closed.
            if (m.aliveFaceCount() != 6) std::printf("  %s: %d faces\n", c.file, m.aliveFaceCount());
            CHECK(m.aliveFaceCount() == 6);
            CHECK(solidVolume(m) > 0 && std::fabs(solidVolume(m) - 0.02 * 0.01 * 0.005) < 1e-12);
            if (c.colours) CHECK(e.state.materials.size() == 1 && m.faces[0].front.material == 0);
            allOk = allOk && m.aliveFaceCount() == 6;
        }
        // A file without a unit is read in the unit given.
        {
            QFile off(dir.filePath("plain.off"));
            CHECK(off.open(QIODevice::WriteOnly));
            off.write("OFF 4 2 0\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n4 0 1 2 3\n3 0 2 1\n");
            off.close();
            double u;
            CHECK(!printFileUnit(dir.filePath("plain.off"), u));
            PrintModel pm;
            CHECK(readPrintFile(dir.filePath("plain.off"), 0.0254, pm) && pm.tris.size() == 3);
            double maxX = 0;
            for (const Vec3& v : pm.verts) maxX = std::max(maxX, v.x);
            CHECK(std::fabs(maxX - 0.254) < 1e-9);
        }
        // A 3MF as other programs write it: an object made of components with
        // transforms, a colour group, centimeters.
        {
            const QByteArray model =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<model unit=\"centimeter\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
                "xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/material/2015/02\"><resources>"
                "<m:colorgroup id=\"5\"><m:color color=\"#00FF00\"/><m:color color=\"#0000FFFF\"/></m:colorgroup>"
                "<object id=\"1\" type=\"model\" pid=\"5\" pindex=\"1\"><mesh><vertices>"
                "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"1\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"1\" z=\"0\"/><vertex x=\"0\" y=\"0\" z=\"1\"/>"
                "</vertices><triangles><triangle v1=\"0\" v2=\"2\" v3=\"1\" pid=\"5\" p1=\"0\"/><triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"
                "<triangle v1=\"0\" v2=\"3\" v3=\"2\"/><triangle v1=\"1\" v2=\"2\" v3=\"3\"/></triangles></mesh></object>"
                "<object id=\"2\" type=\"model\"><components><component objectid=\"1\"/>"
                "<component objectid=\"1\" transform=\"1 0 0 0 1 0 0 0 1 5 0 0\"/></components></object>"
                "</resources><build><item objectid=\"2\" transform=\"1 0 0 0 1 0 0 0 1 0 0 2\"/></build></model>";
            const QByteArray zip = makeZip({{QStringLiteral("3D/3dmodel.model"), model}});
            QFile z(dir.filePath("other.3mf"));
            CHECK(z.open(QIODevice::WriteOnly));
            z.write(zip);
            z.close();
            PrintModel pm;
            QString err;
            CHECK(readPrintFile(dir.filePath("other.3mf"), 1.0, pm, &err));
            CHECK(pm.tris.size() == 8 && pm.verts.size() == 8);
            double maxX = 0, minZ = 1e9;
            for (const Vec3& v : pm.verts) maxX = std::max(maxX, v.x), minZ = std::min(minZ, v.z);
            CHECK(std::fabs(maxX - 0.06) < 1e-9 && std::fabs(minZ - 0.02) < 1e-9); // 6 cm along, lifted 2 cm
            CHECK(pm.colors.size() == 2); // the one green triangle and the blue default
        }
        std::printf("%s 3D printing imports read back what was exported\n", allOk ? "[ ok ]" : "[FAIL]");
    }

    return g_failed ? 1 : 0;
}
