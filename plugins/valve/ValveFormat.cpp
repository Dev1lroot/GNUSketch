// SPDX-License-Identifier: GPL-3.0-or-later
#include "ValveFormat.h"

#include "core/Components.h"
#include "io/GltfExport.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace gs::valve {

namespace {

QByteArray num(double v) {
    if (std::fabs(v) < 5e-7) v = 0;
    QByteArray s = QByteArray::number(v, 'f', 4);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) s.chop(1);
    return s;
}

QByteArray vec(const Vec3& p) { return "(" + num(p.x) + " " + num(p.y) + " " + num(p.z) + ")"; }

bool save(const QString& path, const QByteArray& bytes, QStringList* messages) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (messages) messages->append(f.errorString());
        return false;
    }
    f.write(bytes);
    if (!f.commit()) {
        if (messages) messages->append(f.errorString());
        return false;
    }
    return true;
}

// The point entity a component's name asks for ("ENT.npc_zombie" too), or "".
std::string entityClass(std::string name) {
    for (const char* prefix : {"ENT.", "ENT_", "ent.", "ent_"})
        if (name.rfind(prefix, 0) == 0) name = name.substr(4);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    // A component's own copies get "#2" style suffixes: the class is the part before.
    if (auto hash = lower.find('#'); hash != std::string::npos) lower = lower.substr(0, hash);
    while (!lower.empty() && lower.back() == ' ') lower.pop_back();
    for (const char* p : {"info_", "light", "npc_", "weapon_", "item_", "env_", "ambient_", "point_", "prop_", "monster_"})
        if (lower.rfind(p, 0) == 0) return lower;
    return {};
}

// Pieces of a mesh: faces connected through edges.
std::vector<std::vector<Id>> pieces(const Mesh& m) {
    std::vector<int> part(m.faces.size(), -1);
    std::vector<std::vector<Id>> out;
    for (Id f0 = 0; f0 < (Id)m.faces.size(); ++f0) {
        if (!m.faces[f0].alive || m.faces[f0].hidden || part[f0] >= 0) continue;
        out.emplace_back();
        std::vector<Id> stack{f0};
        part[f0] = (int)out.size() - 1;
        while (!stack.empty()) {
            const Id f = stack.back();
            stack.pop_back();
            out.back().push_back(f);
            for (Id e : m.faceEdges(f))
                for (Id g : m.edgeFaces(e))
                    if (m.faces[g].alive && !m.faces[g].hidden && part[g] < 0) {
                        part[g] = part[f0];
                        stack.push_back(g);
                    }
        }
    }
    return out;
}

struct Side {
    Vec3 p[3]; // world, units; clockwise seen from outside
    Vec3 n;    // outward
    std::string material;
    double uScale = 0.25, vScale = 0.25;
};

struct Brush {
    std::vector<Side> sides;
    int group = 0; // Hammer group id (0 = none)
};

// Hammer's world-aligned texture axes for a face facing n.
void textureAxes(const Vec3& n, Vec3& u, Vec3& v) {
    const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (az >= ax && az >= ay) u = Vec3(1, 0, 0), v = Vec3(0, -1, 0);
    else if (ax >= ay) u = Vec3(0, 1, 0), v = Vec3(0, 0, -1);
    else u = Vec3(1, 0, 0), v = Vec3(0, 0, -1);
}

// A closed, convex piece as a brush; false (with the reason) otherwise.
bool makeBrush(const Document& doc, const Mesh& m, const std::vector<Id>& faces, const Mat4& xf, const VmfOptions& o, Brush& out,
               std::string& why) {
    const std::set<Id> in(faces.begin(), faces.end());
    // Closed: every edge of the piece between two of its faces.
    for (Id f : faces)
        for (Id e : m.faceEdges(f)) {
            int n = 0;
            for (Id g : m.edgeFaces(e)) n += in.count(g) ? 1 : 0;
            if (n != 2) {
                why = "not closed";
                return false;
            }
        }
    std::vector<Vec3> pts;
    std::set<Id> verts;
    for (Id f : faces)
        for (Id v : m.faceVertices(f)) verts.insert(v);
    for (Id v : verts) pts.push_back(xf.transformPoint(m.pos(v)) * kUnitsPerMeter);
    const bool mirrored = xf.det3() < 0;
    std::vector<Vec3> seenN;
    std::vector<double> seenD;
    for (Id f : faces) {
        const Face& F = m.faces[f];
        std::vector<Vec3> loop;
        for (Id v : F.outer) loop.push_back(xf.transformPoint(m.pos(v)) * kUnitsPerMeter);
        if (loop.size() < 3) continue;
        Vec3 n = newellNormal((int)loop.size(), [&](int i) { return loop[i]; });
        if (n.length() < 1e-12) continue;
        n = n.normalized();
        if (mirrored) std::reverse(loop.begin(), loop.end()), n = -n;
        // Outward: the rest of the piece lies behind the face.
        const double d = n.dot(loop[0]);
        for (const Vec3& p : pts)
            if (n.dot(p) - d > 0.01) {
                why = "not convex";
                return false;
            }
        // Coplanar faces of the piece are one brush side.
        bool dup = false;
        for (size_t i = 0; i < seenN.size(); ++i) dup = dup || (seenN[i].dot(n) > 1 - 1e-6 && std::fabs(seenD[i] - d) < 0.01);
        if (dup) continue;
        seenN.push_back(n);
        seenD.push_back(d);
        // Three points far apart, clockwise seen from outside: a, c, b of a
        // counter-clockwise loop.
        const size_t k = loop.size();
        Side s;
        s.p[0] = loop[0];
        s.p[1] = loop[(2 * k) / 3];
        s.p[2] = loop[k / 3];
        s.n = n;
        const Material* mat = doc.material(F.front.material);
        s.material = sourceMaterial(mat ? mat->name : std::string(), o.defaultMaterial);
        if (mat && mat->hasTexture() && mat->texture.pixelWidth > 0) {
            s.uScale = mat->texture.worldWidth * kUnitsPerMeter / mat->texture.pixelWidth;
            s.vScale = mat->texture.worldHeight * kUnitsPerMeter / std::max(1, mat->texture.pixelHeight);
        }
        out.sides.push_back(s);
    }
    if (out.sides.size() < 4) {
        why = "fewer than 4 sides";
        return false;
    }
    return true;
}

struct Writer {
    QByteArray text;
    int nextId = 1;
    int id() { return nextId++; }
    void line(int indent, const QByteArray& s) { text += QByteArray(indent, '\t') + s + '\n'; }
    void kv(int indent, const char* k, const QByteArray& v) { line(indent, QByteArray("\"") + k + "\" \"" + v + "\""); }
    void editor(int indent, const char* color, int group) {
        line(indent, "editor");
        line(indent, "{");
        kv(indent + 1, "color", color);
        if (group) kv(indent + 1, "groupid", QByteArray::number(group));
        kv(indent + 1, "visgroupshown", "1");
        kv(indent + 1, "visgroupautoshown", "1");
        line(indent, "}");
    }
    void brush(const Brush& b, const VmfOptions& o) {
        line(1, "solid");
        line(1, "{");
        kv(2, "id", QByteArray::number(id()));
        for (const Side& s : b.sides) {
            Vec3 u, v;
            textureAxes(s.n, u, v);
            line(2, "side");
            line(2, "{");
            kv(3, "id", QByteArray::number(id()));
            kv(3, "plane", vec(s.p[0]) + " " + vec(s.p[1]) + " " + vec(s.p[2]));
            kv(3, "material", QByteArray::fromStdString(s.material).toUpper());
            kv(3, "uaxis", "[" + num(u.x) + " " + num(u.y) + " " + num(u.z) + " 0] " + num(s.uScale));
            kv(3, "vaxis", "[" + num(v.x) + " " + num(v.y) + " " + num(v.z) + " 0] " + num(s.vScale));
            kv(3, "rotation", "0");
            kv(3, "lightmapscale", QByteArray::number(o.lightmapScale));
            kv(3, "smoothing_groups", "0");
            line(2, "}");
        }
        editor(2, "0 180 193", b.group);
        line(1, "}");
    }
};

QByteArray colorAndBrightness(const Color& c, double brightness) {
    auto b = [](float v) { return QByteArray::number(std::clamp(int(std::lround(v * 255)), 0, 255)); };
    return b(c.r) + " " + b(c.g) + " " + b(c.b) + " " + QByteArray::number(std::max(1, int(std::lround(brightness))));
}

// Hammer angles "pitch yaw roll" for a direction.
QByteArray angles(const Vec3& d) {
    const double yaw = std::atan2(d.y, d.x) * 180 / kPi;
    const double pitch = -std::asin(std::clamp(d.normalized().z, -1.0, 1.0)) * 180 / kPi;
    return num(pitch) + " " + num(yaw) + " 0";
}

} // namespace

std::string sourceMaterial(const std::string& nameIn, const std::string& fallback) {
    std::string name = nameIn;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty() || name == "Default") return fallback;
    // Valve's naming for materials in a model: "dev-graygrid" is dev/graygrid.
    if (name.find('/') == std::string::npos)
        if (auto dash = name.find('-'); dash != std::string::npos) name[dash] = '/';
    for (char& c : name)
        if (c == ' ') c = '_';
    return name;
}

QByteArray vmfText(const Document& doc, const VmfOptions& o, QStringList* messages) {
    const ModelState st = closedState(doc.state);
    Writer w;
    w.text += "// Exported by GNUSketch (Valve Source export, after Valve's SketchUp Source Tools)\n";
    w.text += "versioninfo\n{\n\t\"editorversion\" \"400\"\n\t\"editorbuild\" \"6157\"\n\t\"mapversion\" \"1\"\n\t\"formatversion\" \"100\"\n\t\"prefab\" \"0\"\n}\n";
    w.text += "visgroups\n{\n}\n";
    w.text += "viewsettings\n{\n\t\"bSnapToGrid\" \"1\"\n\t\"bShowGrid\" \"1\"\n\t\"bShowLogicalGrid\" \"0\"\n\t\"nGridSpacing\" \"64\"\n\t\"bShow3DGrid\" \"0\"\n}\n";
    w.line(0, "world");
    w.line(0, "{");
    w.kv(1, "id", QByteArray::number(w.id()));
    w.kv(1, "mapversion", "1");
    w.kv(1, "classname", "worldspawn");
    w.kv(1, "skyname", QByteArray::fromStdString(o.skyname));
    w.kv(1, "maxpropscreenwidth", "-1");
    w.kv(1, "detailvbsp", "detail.vbsp");
    w.kv(1, "detailmaterial", "detail/detailsprites");

    int brushes = 0, skipped = 0;
    std::map<std::string, int> reasons;
    std::vector<std::pair<std::string, Mat4>> pointEntities; // class, placement
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    auto grow = [&](const Brush& b) {
        for (const Side& s : b.sides)
            for (const Vec3& p : s.p)
                for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], p[k]), mx[k] = std::max(mx[k], p[k]);
    };
    auto exportMesh = [&](const Mesh& m, const Mat4& xf, int group) {
        for (const auto& piece : pieces(m)) {
            Brush b;
            b.group = group;
            std::string why;
            if (!makeBrush(doc, m, piece, xf, o, b, why)) {
                ++skipped;
                ++reasons[why];
                continue;
            }
            grow(b);
            w.brush(b, o);
            ++brushes;
        }
    };
    exportMesh(st.mesh, Mat4(), 0);
    std::vector<int> groupIds;
    for (const PlacedInstance& p : placedInstances(st)) {
        const ComponentDef& def = st.defs[p.def];
        const std::string cls = entityClass(def.name);
        if (!cls.empty()) {
            // Where it stands: the bottom centre of the component.
            Vec3 lo, hi;
            bool any = false;
            definitionBounds(st, p.def, lo, hi, any);
            const Vec3 foot = any ? Vec3((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z) : Vec3();
            pointEntities.push_back({cls, p.xf * Mat4::translation(foot)});
            continue;
        }
        // A component with several pieces: one Hammer group.
        const int nPieces = (int)pieces(def.mesh).size();
        int group = 0;
        if (nPieces > 1) {
            group = 1000000 + (int)groupIds.size();
            groupIds.push_back(group);
        }
        exportMesh(def.mesh, p.xf, group);
    }
    if (o.sealBox && brushes > 0) {
        // Six slabs of skybox around everything, so the map compiles without leaks.
        const double gap = 128, t = 16;
        const Vec3 lo = mn - Vec3(gap, gap, gap), hi = mx + Vec3(gap, gap, gap);
        auto slab = [&](Vec3 a, Vec3 b) {
            Brush s;
            const Vec3 c[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {a.x, b.y, a.z},
                               {a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
            // Faces as counter-clockwise loops seen from outside: a, c, b clockwise.
            const int q[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 4, 7, 3}, {1, 2, 6, 5}};
            const Vec3 ns[6] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
            for (int i = 0; i < 6; ++i) {
                Side sd;
                sd.p[0] = c[q[i][0]];
                sd.p[1] = c[q[i][2]];
                sd.p[2] = c[q[i][1]];
                sd.n = ns[i];
                sd.material = "tools/toolsskybox";
                s.sides.push_back(sd);
            }
            w.brush(s, o);
        };
        slab({lo.x - t, lo.y - t, lo.z - t}, {hi.x + t, hi.y + t, lo.z});
        slab({lo.x - t, lo.y - t, hi.z}, {hi.x + t, hi.y + t, hi.z + t});
        slab({lo.x - t, lo.y - t, lo.z}, {hi.x + t, lo.y, hi.z});
        slab({lo.x - t, hi.y, lo.z}, {hi.x + t, hi.y + t, hi.z});
        slab({lo.x - t, lo.y, lo.z}, {lo.x, hi.y, hi.z});
        slab({hi.x, lo.y, lo.z}, {hi.x + t, hi.y, hi.z});
    }
    w.line(0, "}");
    for (int g : groupIds) {
        w.line(0, "group");
        w.line(0, "{");
        w.kv(1, "id", QByteArray::number(g));
        w.line(1, "editor");
        w.line(1, "{");
        w.kv(2, "color", "0 180 193");
        w.kv(2, "visgroupshown", "1");
        w.kv(2, "visgroupautoshown", "1");
        w.line(1, "}");
        w.line(0, "}");
    }

    // Point entities: named components, then the lights and the sun.
    bool playerStart = false, anyLight = false;
    auto entity = [&](const QByteArray& cls, const Vec3& origin, const QByteArray& ang,
                      const std::vector<std::pair<const char*, QByteArray>>& keys) {
        w.line(0, "entity");
        w.line(0, "{");
        w.kv(1, "id", QByteArray::number(w.id()));
        w.kv(1, "classname", cls);
        w.kv(1, "angles", ang);
        for (const auto& [k, v] : keys) w.kv(1, k, v);
        w.kv(1, "origin", num(origin.x) + " " + num(origin.y) + " " + num(origin.z));
        w.line(1, "editor");
        w.line(1, "{");
        w.kv(2, "color", "0 255 0");
        w.kv(2, "visgroupshown", "1");
        w.kv(2, "visgroupautoshown", "1");
        w.kv(2, "logicalpos", "[0 0]");
        w.line(1, "}");
        w.line(0, "}");
        playerStart = playerStart || cls == "info_player_start";
        anyLight = anyLight || cls.startsWith("light");
    };
    for (const auto& [cls, xf] : pointEntities) {
        const Vec3 origin = xf.transformPoint(Vec3()) * kUnitsPerMeter;
        const QByteArray c = QByteArray::fromStdString(cls);
        std::vector<std::pair<const char*, QByteArray>> keys;
        if (c == "light") keys = {{"_light", "255 255 255 500"}, {"_lightHDR", "-1 -1 -1 1"}, {"_lightscaleHDR", "1"}, {"_quadratic_attn", "1"}};
        if (c == "light_environment")
            keys = {{"_ambient", "64 64 64 200"}, {"_ambientHDR", "-1 -1 -1 1"}, {"_AmbientScaleHDR", "1"}, {"_light", "255 255 255 500"},
                    {"_lightHDR", "-1 -1 -1 1"}, {"_lightscaleHDR", "1"}, {"pitch", "-90"}, {"SunSpreadAngle", "5"}};
        entity(c, origin, angles(xf.transformDir(Vec3(1, 0, 0))), keys);
    }
    for (const SceneObject& ob : st.objects) {
        if (!ob.alive || !ob.isLight() || !ob.enabled) continue;
        const Vec3 origin = ob.pos * kUnitsPerMeter;
        // Brightness: GNUSketch's intensity (at 1 m) to Hammer's scale.
        const QByteArray light = colorAndBrightness(ob.color, ob.intensity * 100);
        const Vec3 aim = (ob.target - ob.pos).length2() > 1e-12 ? (ob.target - ob.pos).normalized() : Vec3(0, 0, -1);
        if (ob.kind == SceneObject::PointLight) {
            entity("light", origin, "0 0 0", {{"_light", light}, {"_lightHDR", "-1 -1 -1 1"}, {"_lightscaleHDR", "1"}, {"_quadratic_attn", "1"}});
        } else if (ob.kind == SceneObject::SpotLight) {
            const QByteArray outer = num(ob.coneAngle / 2), inner = num(ob.coneAngle / 2 * (1 - std::clamp(ob.softness, 0.0, 1.0)));
            entity("light_spot", origin, angles(aim),
                   {{"_light", light}, {"_lightHDR", "-1 -1 -1 1"}, {"_lightscaleHDR", "1"}, {"_quadratic_attn", "1"}, {"_cone", outer},
                    {"_inner_cone", inner}, {"pitch", num(-std::asin(std::clamp(aim.z, -1.0, 1.0)) * 180 / kPi)}});
        } else {
            entity("light_environment", origin, angles(aim),
                   {{"_light", colorAndBrightness(ob.color, ob.intensity * 400)}, {"_ambient", "64 64 64 200"},
                    {"pitch", num(-std::asin(std::clamp(aim.z, -1.0, 1.0)) * 180 / kPi)}, {"SunSpreadAngle", "5"}});
        }
    }
    if (doc.style.sun && doc.style.sunAltitude > 0 && !anyLight) {
        const Vec3 toSun = doc.style.sunDirection();
        entity("light_environment", (mx + mn) * 0.5 + Vec3(0, 0, std::max(64.0, (mx.z - mn.z) * 0.5)), angles(-toSun),
               {{"_light", "255 248 230 400"}, {"_ambient", "150 170 200 80"}, {"pitch", num(-doc.style.sunAltitude)}, {"SunSpreadAngle", "5"}});
    }
    w.text += "cameras\n{\n\t\"activecamera\" \"-1\"\n}\n";
    w.text += "cordon\n{\n\t\"mins\" \"(-1024 -1024 -1024)\"\n\t\"maxs\" \"(1024 1024 1024)\"\n\t\"active\" \"0\"\n}\n";

    if (messages) {
        messages->append(QStringLiteral("%1 brushes, %2 point entities written.").arg(brushes).arg(pointEntities.size()));
        if (skipped) {
            QStringList why;
            for (const auto& [r, n] : reasons) why << QStringLiteral("%1 %2").arg(n).arg(QString::fromStdString(r));
            messages->append(QStringLiteral("%1 pieces were left out (%2): Hammer brushes must be closed and convex. Split them into convex "
                                            "parts, or export them as a model (Valve SMD) instead.")
                                 .arg(skipped)
                                 .arg(why.join(QStringLiteral(", "))));
        }
        if (!playerStart) messages->append(QStringLiteral("No info_player_start: add a component named info_player_start where the player starts."));
        if (!anyLight && !(doc.style.sun && doc.style.sunAltitude > 0))
            messages->append(QStringLiteral("No light: the map will be dark. Add a light, or turn the sun on."));
        if (brushes && (std::max({mx.x, mx.y, mx.z}) > 16384 || std::min({mn.x, mn.y, mn.z}) < -16384))
            messages->append(QStringLiteral("The model goes beyond Hammer's limit of 16384 units (416 m) from the origin: make it smaller, or "
                                            "use a 3D skybox for distant geometry."));
    }
    return w.text;
}

bool writeVmf(const Document& doc, const QString& path, const VmfOptions& o, QStringList* messages) {
    return save(path, vmfText(doc, o, messages), messages);
}

QByteArray smdText(const Document& doc, QStringList* messages) {
    const Mesh m = flattenModel(doc.state);
    QByteArray out = "version 1\nnodes\n0 \"root\" -1\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\ntriangles\n";
    int tris = 0;
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        const Material* mat = doc.material(F.front.material);
        std::string name = sourceMaterial(mat ? mat->name : std::string(), "graygrid");
        // A model's materials are names inside its $cdmaterials folder.
        if (auto slash = name.rfind('/'); slash != std::string::npos) name = name.substr(slash + 1);
        const QByteArray matLine = QByteArray::fromStdString(name).toLower();
        const FaceTriangles ft = triangulateFace(m, f, true, mat);
        for (const auto& t : ft.tris) {
            Vec3 a = ft.pos[t[0]], b = ft.pos[t[1]], c = ft.pos[t[2]];
            int order[3] = {t[0], t[1], t[2]};
            if ((b - a).cross(c - a).dot(F.normal) < 0) std::swap(order[1], order[2]);
            out += matLine + "\n";
            for (int k : order) {
                const Vec3 p = ft.pos[k] * kUnitsPerMeter;
                const Vec2 uv = ft.uv[k];
                out += "0 " + num(p.x) + " " + num(p.y) + " " + num(p.z) + " " + num(F.normal.x) + " " + num(F.normal.y) + " " +
                       num(F.normal.z) + " " + num(uv.x) + " " + num(uv.y) + "\n";
            }
            ++tris;
        }
    }
    out += "end\n";
    if (messages) {
        messages->append(QStringLiteral("%1 triangles written.").arg(tris));
        if (tris > 10000) messages->append(QStringLiteral("Over 10000 triangles: a heavy model for a prop."));
    }
    return out;
}

QByteArray qcText(const QString& smdFileName, const SmdOptions& o) {
    const QByteArray base = QFileInfo(smdFileName).completeBaseName().toUtf8();
    QByteArray qc = "// Compile with studiomdl (the game's bin folder): studiomdl " + base + ".qc\n";
    qc += "$modelname \"gnusketch/" + base + ".mdl\"\n";
    qc += "$cdmaterials \"models/gnusketch/\"\n";
    qc += "$body body \"" + smdFileName.toUtf8() + "\"\n";
    if (o.staticProp) qc += "$staticprop\n";
    qc += "$surfaceprop \"" + QByteArray::fromStdString(o.surfaceProp) + "\"\n";
    qc += "$sequence idle \"" + smdFileName.toUtf8() + "\" fps 30\n";
    qc += "$collisionmodel \"" + smdFileName.toUtf8() + "\"\n{\n\t$concave\n\t$maxconvexpieces 64\n}\n";
    return qc;
}

bool writeSmd(const Document& doc, const QString& path, const SmdOptions& o, QStringList* messages) {
    if (!save(path, smdText(doc, messages), messages)) return false;
    if (o.writeQc) {
        const QFileInfo fi(path);
        const QString qc = fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".qc"));
        if (!save(qc, qcText(fi.fileName(), o), messages)) return false;
        if (messages) messages->append(QStringLiteral("Wrote %1 for studiomdl.").arg(QFileInfo(qc).fileName()));
    }
    return true;
}

} // namespace gs::valve
