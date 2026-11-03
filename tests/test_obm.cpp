// SPDX-License-Identifier: GPL-3.0-or-later
// Open Block Model plugin (plugins/obm): read and written back unchanged; written
// again after its cubes were worked out afresh, the same faces and pictures; a cube
// moved, moved in the file.
//
//   gs_obm_tests                 the built-in models
//   gs_obm_tests <folder>...     and every .obm under these folders (GNUBlocks'
//                                assets/, its build/obm/ from tools/obm_convert.py)
#include "ObmFormat.h"

#include "core/Components.h"
#include "io/ImageUtil.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace gs;
using namespace gs::obm;

static int failures = 0;
#define CHECK(c, msg)                                                         \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, std::string(msg).c_str()); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static QString writeText(const QTemporaryDir& dir, const QString& name, const std::string& text) {
    const QString p = dir.filePath(name);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return p;
    f.write(text.data(), qint64(text.size()));
    return p;
}
static std::string readText(const QString& p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    return std::string(b.constData(), size_t(b.size()));
}

// Every face of the model in world space: its corners, and what of its picture is at each.
struct WorldFace { std::vector<double> key; std::string mat; };
static std::vector<WorldFace> worldFaces(const Document& doc) {
    std::vector<WorldFace> out;
    auto add = [&](const Mesh& mesh, const Mat4& xf, bool shown) {
        if (!shown) return;
        for (const Face& F : mesh.faces) {
            if (!F.alive) continue;
            for (int side = 0; side < 2; ++side) {
                const FaceSide& S = side ? F.back : F.front;
                if (side && S.material == kNone) continue;
                const Material* m = doc.material(S.material);
                std::vector<std::array<double, 5>> c;
                for (Id v : F.outer) {
                    const Vec3 w = xf.transformPoint(mesh.pos(v));
                    Vec2 t = m && m->hasTexture() ? faceTexCoord(F, side == 0, mesh.pos(v), m) : Vec2(0, 0);
                    auto r = [](double x) { return std::round(x * 1e6) / 1e6 + 0.0; }; // (noise must not reorder the corners)
                    c.push_back({r(w.x), r(w.y), r(w.z), r(t.x), r(t.y)});
                }
                std::sort(c.begin(), c.end());
                WorldFace wf;
                for (auto& k : c) wf.key.insert(wf.key.end(), k.begin(), k.end());
                wf.mat = m ? m->name : "-";
                out.push_back(wf);
            }
        }
    };
    add(doc.state.mesh, Mat4(), true);
    for (const PlacedInstance& p : placedInstances(doc.state)) add(doc.state.defs[p.def].mesh, p.xf, true);
    std::sort(out.begin(), out.end(), [](const WorldFace& a, const WorldFace& b) { return a.mat != b.mat ? a.mat < b.mat : a.key < b.key; });
    return out;
}
static bool sameFaces(const std::vector<WorldFace>& a, const std::vector<WorldFace>& b, std::string* why) {
    if (a.size() != b.size()) return *why = std::to_string(a.size()) + " faces, then " + std::to_string(b.size()), false;
    // (paired by nearest: a rounding in the file can reorder two faces that touch)
    std::vector<bool> used(b.size(), false);
    for (const WorldFace& f : a) {
        bool found = false;
        for (size_t j = 0; j < b.size() && !found; ++j) {
            if (used[j] || b[j].mat != f.mat || b[j].key.size() != f.key.size()) continue;
            // (corner for corner, in any order: each of this one's near one of that one's)
            bool same = true;
            for (size_t k = 0; k < f.key.size() && same; k += 5) {
                bool near = false;
                for (size_t q = 0; q < b[j].key.size() && !near; q += 5) {
                    near = true;
                    for (size_t i = 0; i < 5 && near; ++i) near = std::abs(f.key[k + i] - b[j].key[q + i]) < 2e-4;
                }
                same = near;
            }
            if (same) used[j] = found = true;
        }
        if (!found) {
            // (and the one most like it: what differs)
            double best = 1e300;
            const WorldFace* near = nullptr;
            for (const WorldFace& g : b) {
                if (g.key.size() != f.key.size()) continue;
                double d = 0;
                for (size_t k = 0; k < f.key.size(); ++k) d += k % 5 < 3 ? std::abs(f.key[k] - g.key[k]) : 0;
                if (d < best) best = d, near = &g;
            }
            auto show = [](const WorldFace& w) {
                std::string t;
                for (size_t k = 0; k < w.key.size(); ++k) t += (k % 5 == 0 ? " (" : ",") + std::to_string(w.key[k]).substr(0, 7) + (k % 5 == 4 ? ")" : "");
                return t;
            };
            *why = "a face of " + f.mat + " is not the same:\n   was" + show(f) + (near ? "\n   now" + show(*near) : std::string());
            return false;
        }
    }
    return true;
}

// The cubes worked out afresh (as if every one had been changed): the extras' records dropped.
static void forget(Document& doc) {
    auto drop = [](std::string& extra) {
        const auto at = extra.find("\"cubes\":[");
        if (at == std::string::npos) return;
        // (the records list: replaced by an empty one, by bracket depth)
        size_t i = at + 9;
        int depth = 1;
        bool str = false;
        for (; i < extra.size() && depth; ++i) {
            const char c = extra[i];
            if (str) {
                if (c == '\\') ++i;
                else if (c == '"') str = false;
            } else if (c == '"') str = true;
            else if (c == '[') ++depth;
            else if (c == ']') --depth;
        }
        extra = extra.substr(0, at + 9) + "]" + extra.substr(i);
    };
    drop(doc.state.extra);
    for (ComponentDef& d : doc.state.defs) drop(d.extra);
}

// One model: read, written (unchanged: the same text), and written afresh (the same faces).
static void roundTrip(const QString& path, const QTemporaryDir& tmp, bool exact, int* exactOk, int* faceOk) {
    Document a;
    QStringList msgs;
    if (!importObm(a, path, &msgs)) {
        CHECK(false, path.toStdString() + ": not read: " + msgs.join("; ").toStdString());
        return;
    }
    const QString out = tmp.filePath("out.obm");
    CHECK(exportObm(a, out, &msgs), path.toStdString() + ": not written");
    const std::string before = readText(path), after = readText(out);
    if (exact) {
        if (before == after) ++*exactOk;
        else {
            size_t i = 0;
            while (i < before.size() && i < after.size() && before[i] == after[i]) ++i;
            CHECK(false, path.toStdString() + ": written back differently at byte " + std::to_string(i) + ": ..." +
                             before.substr(i > 40 ? i - 40 : 0, 80) + "... / ..." + after.substr(i > 40 ? i - 40 : 0, 80) + "...");
        }
    }
    // afresh
    Document b;
    importObm(b, path, &msgs);
    forget(b);
    const QString fresh = tmp.filePath("fresh.obm");
    CHECK(exportObm(b, fresh, &msgs), path.toStdString() + ": not written afresh");
    Document c;
    qputenv("GNUSKETCH_OBM_TEXTURES", texturesFolder(path).toLocal8Bit());   // (it isn't beside a models folder: the source's pictures)
    CHECK(importObm(c, fresh, &msgs), path.toStdString() + ": the fresh one not read");
    qunsetenv("GNUSKETCH_OBM_TEXTURES");
    std::string why;
    if (sameFaces(worldFaces(a), worldFaces(c), &why)) ++*faceOk;
    else CHECK(false, path.toStdString() + ": written afresh, " + why);
}

static const char* kBlock = R"({
 "format": "obm",
 "version": 1,
 "id": "test:block/lamp",
 "kind": "block",
 "anchor": "corner",
 "meta": {
  "name": "lamp"
 },
 "texture_size": [
  32,
  32
 ],
 "textures": [
  {
   "name": "lamp",
   "path": "block/lamp"
  }
 ],
 "groups": [
  {
   "name": "base",
   "pivot": [
    8,
    0,
    8
   ],
   "tags": [
    "spin:y"
   ],
   "cubes": [
    {
     "name": "foot",
     "from": [
      2,
      0,
      2
     ],
     "to": [
      14,
      2,
      14
     ],
     "faces": {
      "north": {
       "uv": [
        0,
        0,
        24,
        4
       ],
       "texture": 0,
       "cull": "north"
      },
      "east": {
       "uv": [
        0,
        4,
        24,
        8
       ],
       "rotation": 90,
       "texture": 0
      },
      "up": {
       "uv": [
        8,
        8,
        32,
        32
       ],
       "rotation": 180,
       "texture": 0
      },
      "down": {
       "uv": [
        8,
        8,
        32,
        32
       ],
       "texture": 0
      }
     }
    }
   ],
   "groups": [
    {
     "name": "arm",
     "pivot": [
      8,
      2,
      8
     ],
     "rotation": [
      0,
      0,
      22.5
     ],
     "cubes": [
      {
       "name": "pole",
       "from": [
        7,
        2,
        7
       ],
       "to": [
        9,
        12,
        9
       ],
       "pivot": [
        8,
        7,
        8
       ],
       "rotation": [
        0,
        45,
        0
       ],
       "faces": {
        "north": {
         "uv": [
          0,
          8,
          4,
          28
         ],
         "texture": 0
        },
        "south": {
         "uv": [
          4,
          8,
          0,
          28
         ],
         "texture": 0
        },
        "west": {
         "uv": [
          0,
          8,
          4,
          28
         ],
         "rotation": 270,
         "texture": 0
        }
       }
      },
      {
       "name": "shade",
       "from": [
        4,
        12,
        8
       ],
       "to": [
        12,
        16,
        8
       ],
       "faces": {
        "north": {
         "uv": [
          16,
          0,
          32,
          8
         ],
         "texture": 0
        },
        "south": {
         "uv": [
          32,
          0,
          16,
          8
         ],
         "texture": 0
        }
       },
       "tags": [
        "emissive"
       ]
      }
     ]
    }
   ]
  }
 ],
 "cubes": [
  {
   "name": "loose",
   "from": [
    0,
    0,
    0
   ],
   "to": [
    1,
    1,
    1
   ],
   "faces": {
    "up": {
     "uv": [
      0,
      0,
      2,
      2
     ],
     "texture": 0
    }
   }
  }
 ],
 "animations": [
  {
   "name": "turn",
   "loop": "loop",
   "length": 1,
   "bones": {
    "arm": {
     "rotation": [
      {
       "t": 0,
       "v": [
        0,
        0,
        0
       ],
       "interp": "linear"
      }
     ]
    }
   }
  }
 ],
 "extensions": {
  "kept": true
 }
})";

static const char* kEntity = R"({
 "format": "obm",
 "version": 1,
 "kind": "entity",
 "anchor": "center",
 "texture_size": [
  64,
  32
 ],
 "textures": [
  {
   "name": "beast",
   "path": "entity/beast"
  }
 ],
 "groups": [
  {
   "name": "body",
   "pivot": [
    0,
    12,
    0
   ],
   "rotation": [
    90,
    0,
    0
   ],
   "tags": [
    "animation:body"
   ],
   "cubes": [
    {
     "name": "c",
     "from": [
      -5,
      2,
      -3
     ],
     "to": [
      5,
      18,
      5
     ],
     "box_uv": [
      28,
      8
     ]
    }
   ]
  },
  {
   "name": "head",
   "pivot": [
    0,
    12,
    -6
   ],
   "cubes": [
    {
     "name": "c",
     "from": [
      -4,
      8,
      -14
     ],
     "to": [
      4,
      16,
      -6
     ],
     "faces": {
      "north": {
       "uv": [
        8,
        8,
        16,
        16
       ],
       "texture": 0
      },
      "up": {
       "uv": [
        8,
        0,
        16,
        8
       ],
       "texture": 0
      },
      "down": {
       "uv": [
        16,
        8,
        24,
        0
       ],
       "rotation": 90,
       "texture": 0
      }
     }
    }
   ]
  }
 ]
})";

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    // (a folder laid out as the game's: models/ beside textures/, the pictures in it)
    QDir(tmp.path()).mkpath("models/block"), QDir(tmp.path()).mkpath("textures/block"), QDir(tmp.path()).mkpath("textures/entity");
    QImage img(32, 32, QImage::Format_ARGB32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) img.setPixel(x, y, qRgba(x * 8, y * 8, 128, 255));
    img.save(tmp.filePath("textures/block/lamp.png"));
    img.scaled(64, 32).save(tmp.filePath("textures/entity/beast.png"));
    CHECK(texturesFolder(tmp.filePath("models/block/x.obm")) == QDir::cleanPath(tmp.filePath("textures")), "the textures folder found");

    int exactOk = 0, faceOk = 0;
    const QString block = writeText(tmp, "models/block/lamp.obm", kBlock), entity = writeText(tmp, "models/block/beast.obm", kEntity);
    roundTrip(block, tmp, true, &exactOk, &faceOk);
    roundTrip(entity, tmp, true, &exactOk, &faceOk);
    CHECK(exactOk == 2 && faceOk == 2, "the built-in models");

    {   // read: groups are components, inside each other; the pictures read
        Document d;
        QStringList msgs;
        importObm(d, block, &msgs);
        CHECK(d.state.defs.size() == 2 && d.state.instances.size() == 2, "two groups, two components");
        CHECK(d.state.instances[1].parent == d.state.instances[0].def, "the arm inside the base");
        CHECK(d.state.materials.size() == 1 && d.state.materials[0].hasTexture() && d.state.materials[0].name == "block/lamp", "its picture, by its path");
        CHECK(msgs.isEmpty(), "nothing to say: " + msgs.join("; ").toStdString());
        int flat = 0;
        for (const Face& f : d.state.defs[1].mesh.faces) flat += f.alive && f.back.material != kNone;
        CHECK(flat == 1, "a flat cube's two sides: one face, front and back");

        // tags set here (Properties > OBM): written
        setGroupTags(d, 0, {"spin:y", "socket:lamp"});
        setCubeTags(d, 0, 0, {"emissive"});
        setModelTags(d, {"lit"});
        GroupInfo gi;
        CHECK(groupInfo(d, 0, gi) && gi.tags.size() == 2 && gi.cubes.size() == 1 && gi.cubes[0].tags == std::vector<std::string>{"emissive"},
              "tags read back as set");
        const QString tagged = tmp.filePath("tagged.obm");
        exportObm(d, tagged, &msgs);
        const std::string tt = readText(tagged);
        CHECK(tt.find("\"socket:lamp\"") != std::string::npos && tt.find("\"lit\"") != std::string::npos, "group and model tags written");
        CHECK(tt.find("\"name\": \"foot\",") != std::string::npos && tt.find("\"emissive\"", tt.find("\"name\": \"foot\"")) < tt.find("\"name\": \"arm\""),
              "the cube's tag written with it");

        // a cube moved one pixel east: moved in the file, all else as it was
        Mesh& m = d.state.defs[0].mesh;
        std::set<Id> verts;
        for (Id v = 0; v < (Id)m.vertices.size(); ++v) verts.insert(v);
        m.transformVertices(verts, Mat4::translation(Vec3(1.0 / 16, 0, 0)), false);
        const QString out = tmp.filePath("moved.obm");
        exportObm(d, out, &msgs);
        const std::string t = readText(out);
        CHECK(t.find("\"name\": \"foot\"") != std::string::npos, "the moved cube keeps its name");
        CHECK(t.find("\"cull\": \"north\"") != std::string::npos, "and its face's cull");
        const size_t foot = t.find("\"name\": \"foot\"");
        CHECK(t.find("\"from\": [\n      3,\n      0,\n      2\n     ]", foot) != std::string::npos, "from one pixel east: " + t.substr(foot, 200));
        CHECK(t.find("\"name\": \"pole\"") != std::string::npos && t.find("\"rotation\": [\n        0,\n        45,\n        0\n       ]") != std::string::npos,
              "the cube not moved: as it was");
        CHECK(t.find("\"kept\": true") != std::string::npos && t.find("\"animations\"") != std::string::npos, "what GNUSketch can't show: kept");
    }
    {   // made here: a box drawn and pushed up is a cube
        Document d;
        Material mat;
        mat.name = "block/lamp";
        loadTextureFile(tmp.filePath("textures/block/lamp.png"), mat.texture);
        const Id mi = d.addMaterial(mat);
        Mesh& m = d.mesh();
        m.drawPolyline({{0, 0, 0}, {0.5, 0, 0}, {0.5, 0.25, 0}, {0, 0.25, 0}}, true);
        Id f0 = kNone;
        for (Id f = 0; f < (Id)m.faces.size(); ++f)
            if (m.faces[f].alive) f0 = f;
        m.pushPull(f0, 0.125);
        for (Face& f : m.faces)
            if (f.alive) f.front.material = mi;
        const QString out = tmp.filePath("models/block/made.obm");
        QStringList msgs;
        CHECK(exportObm(d, out, &msgs), "a model made here written");
        const std::string t = readText(out);
        const QJsonObject made = QJsonDocument::fromJson(QByteArray::fromStdString(t)).object();
        const QJsonArray cubes = made["cubes"].toArray();
        std::vector<double> size;
        if (cubes.size() == 1)
            for (int k = 0; k < 3; ++k)
                size.push_back(std::abs(cubes[0]["to"].toArray()[k].toDouble() - cubes[0]["from"].toArray()[k].toDouble()));
        CHECK(size == std::vector<double>({8, 2, 4}) && !cubes[0].toObject().contains("rotation") && cubes[0]["faces"].toObject().size() == 6,
              "one 8 x 2 x 4 px cube, not turned, six faces: " + t);
        Document back;
        CHECK(importObm(back, out, &msgs), "and read again");
        std::string why;
        CHECK(sameFaces(worldFaces(d), worldFaces(back), &why), "the same box, the same pictures: " + why);
    }

    // the folders given: every model in them
    int all = 0;
    exactOk = faceOk = 0;
    for (int i = 1; i < argc; ++i) {
        QDirIterator it(QString::fromLocal8Bit(argv[i]), {"*.obm"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            ++all;
            roundTrip(it.next(), tmp, true, &exactOk, &faceOk);
        }
    }
    if (all) std::printf("%d models: %d written back unchanged, %d the same faces written afresh\n", all, exactOk, faceOk);
    std::printf(failures ? "%d FAILURES\n" : "obm: ok\n", failures);
    return failures ? 1 : 0;
}
