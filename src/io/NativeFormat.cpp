// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeFormat.h"

#include "ImageUtil.h"
#include "core/Components.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace gs {

namespace {

QJsonArray vec(const Vec3& v) { return QJsonArray{v.x, v.y, v.z}; }
Vec3 vec(const QJsonValue& j) {
    QJsonArray a = j.toArray();
    return a.size() >= 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3();
}
QJsonArray ids(const std::vector<Id>& v) {
    QJsonArray a;
    for (Id i : v) a.append(i);
    return a;
}
std::vector<Id> ids(const QJsonValue& j) {
    std::vector<Id> r;
    for (const auto& x : j.toArray()) r.push_back(x.toInt());
    return r;
}

QJsonObject sideToJson(const FaceSide& s) {
    QJsonObject o;
    o["material"] = s.material;
    if (s.tex.valid) {
        QJsonObject uv;
        uv["origin"] = vec(s.tex.origin);
        uv["u"] = vec(s.tex.u);
        uv["v"] = vec(s.tex.v);
        o["uv"] = uv;
    }
    if (s.axis.length2() > 0.5) o["axis"] = vec(s.axis);
    return o;
}

FaceSide sideFromJson(const QJsonObject& o) {
    FaceSide s;
    s.material = o.value("material").toInt(kNone);
    if (o.contains("uv")) {
        QJsonObject uv = o["uv"].toObject();
        s.tex.valid = true;
        s.tex.origin = vec(uv["origin"]);
        s.tex.u = vec(uv["u"]);
        s.tex.v = vec(uv["v"]);
    }
    if (o.contains("axis")) s.axis = vec(o["axis"]).normalized();
    return s;
}

} // namespace

namespace {

QJsonObject textureToJson(const Texture& tex) {
    QJsonObject t;
    t["mime"] = QString::fromStdString(tex.mime);
    t["width"] = tex.worldWidth;
    t["height"] = tex.worldHeight;
    QByteArray bytes(reinterpret_cast<const char*>(tex.data->data()), (qsizetype)tex.data->size());
    t["data"] = QString::fromLatin1(bytes.toBase64());
    return t;
}

bool textureFromJson(const QJsonObject& t, Texture& out) {
    Texture tex;
    if (!textureFromBytes(QByteArray::fromBase64(t["data"].toString().toLatin1()), tex)) return false;
    tex.worldWidth = t.value("width").toDouble(1.0);
    tex.worldHeight = t.value("height").toDouble(1.0);
    out = tex;
    return true;
}

const char* objectKindKey(SceneObject::Kind k) {
    switch (k) {
    case SceneObject::SpotLight: return "spot";
    case SceneObject::DirectionalLight: return "directional";
    case SceneObject::ViewCamera: return "camera";
    default: return "point";
    }
}

QJsonObject objectToJson(const SceneObject& o) {
    QJsonObject j;
    j["kind"] = objectKindKey(o.kind);
    j["name"] = QString::fromStdString(o.name);
    j["pos"] = vec(o.pos);
    if (o.aims()) j["target"] = vec(o.target);
    if (o.hidden) j["hidden"] = true;
    if (o.isLight()) {
        j["enabled"] = o.enabled;
        j["color"] = QJsonArray{o.color.r, o.color.g, o.color.b, o.color.a};
        j["intensity"] = o.intensity;
        if (o.kind != SceneObject::DirectionalLight) j["range"] = o.range;
        if (o.kind == SceneObject::SpotLight) {
            j["coneAngle"] = o.coneAngle;
            j["softness"] = o.softness;
        }
    } else {
        j["fov"] = o.fov;
        j["perspective"] = o.perspective;
    }
    return j;
}

SceneObject objectFromJson(const QJsonObject& j) {
    SceneObject o;
    const QString k = j["kind"].toString();
    o.kind = k == "spot" ? SceneObject::SpotLight : k == "directional" ? SceneObject::DirectionalLight
           : k == "camera" ? SceneObject::ViewCamera : SceneObject::PointLight;
    o.name = j["name"].toString().toStdString();
    o.pos = vec(j["pos"]);
    o.target = j.contains("target") ? vec(j["target"]) : o.pos;
    o.hidden = j["hidden"].toBool();
    o.enabled = j["enabled"].toBool(true);
    QJsonArray c = j["color"].toArray();
    if (c.size() >= 3) o.color = Color((float)c[0].toDouble(), (float)c[1].toDouble(), (float)c[2].toDouble());
    o.intensity = j["intensity"].toDouble(o.intensity);
    o.range = j["range"].toDouble(o.range);
    o.coneAngle = j["coneAngle"].toDouble(o.coneAngle);
    o.softness = j["softness"].toDouble(o.softness);
    o.fov = j["fov"].toDouble(o.fov);
    o.perspective = j["perspective"].toBool(true);
    return o;
}

} // namespace

QJsonObject materialToJson(const Material& m) {
    QJsonObject o;
    o["name"] = QString::fromStdString(m.name);
    o["color"] = QJsonArray{m.color.r, m.color.g, m.color.b, m.color.a};
    o["opacity"] = m.opacity;
    if (m.hasTexture()) o["texture"] = textureToJson(m.texture);
    if (m.pbr) o["pbr"] = true;
    if (m.advanced()) {
        o["baseStrength"] = m.baseStrength;
        o["emission"] = QJsonArray{m.emission.r, m.emission.g, m.emission.b, m.emission.a};
        if (m.flipNormalY) o["flipNormalY"] = true;
        if (m.blend) o["blend"] = true;
        QJsonObject maps;
        for (int i = 0; i < MapSlotCount; ++i) {
            QJsonObject e;
            e["strength"] = m.maps[i].strength;
            if (m.maps[i].tex.valid()) e["texture"] = textureToJson(m.maps[i].tex);
            maps[QString::fromLatin1(mapSlotKey((MapSlot)i))] = e;
        }
        o["maps"] = maps;
    }
    if (!m.shader.empty()) {
        o["shaderOn"] = m.shaderOn;
        o["shader"] = QString::fromStdString(m.shader);
        QJsonObject props;
        for (const auto& [name, v] : m.shaderProps) props[QString::fromStdString(name)] = QJsonArray{v[0], v[1], v[2], v[3]};
        if (!props.isEmpty()) o["shaderProps"] = props;
    }
    return o;
}

Material materialFromJson(const QJsonObject& o) {
    Material m;
    m.name = o["name"].toString().toStdString();
    QJsonArray c = o["color"].toArray();
    if (c.size() >= 3)
        m.color = Color((float)c[0].toDouble(), (float)c[1].toDouble(), (float)c[2].toDouble(),
                        c.size() > 3 ? (float)c[3].toDouble() : 1.0f);
    m.opacity = (float)o.value("opacity").toDouble(1.0);
    if (o.contains("texture")) textureFromJson(o["texture"].toObject(), m.texture);
    m.pbr = o["pbr"].toBool();
    m.baseStrength = (float)o.value("baseStrength").toDouble(1.0);
    QJsonArray em = o["emission"].toArray();
    if (em.size() >= 3) m.emission = Color((float)em[0].toDouble(), (float)em[1].toDouble(), (float)em[2].toDouble());
    m.flipNormalY = o["flipNormalY"].toBool();
    m.blend = o["blend"].toBool();
    const QJsonObject maps = o["maps"].toObject();
    for (int i = 0; i < MapSlotCount; ++i) {
        const QJsonObject e = maps[QString::fromLatin1(mapSlotKey((MapSlot)i))].toObject();
        m.maps[i].strength = (float)e.value("strength").toDouble(m.maps[i].strength);
        if (e.contains("texture")) textureFromJson(e["texture"].toObject(), m.maps[i].tex);
    }
    m.shaderOn = o["shaderOn"].toBool();
    m.shader = o["shader"].toString().toStdString();
    const QJsonObject props = o["shaderProps"].toObject();
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QJsonArray a = it.value().toArray();
        std::array<float, 4> v{0, 0, 0, 1};
        for (int i = 0; i < 4 && i < a.size(); ++i) v[i] = (float)a[i].toDouble();
        m.shaderProps[it.key().toStdString()] = v;
    }
    return m;
}

QJsonObject meshToJson(const Mesh& meshIn) {
    Mesh mesh = meshIn;
    mesh.compact();
    QJsonObject root;
    QJsonArray verts, edges, faces;
    QJsonArray paint;
    for (Id i = 0; i < (Id)mesh.vertices.size(); ++i) {
        const Vertex& v = mesh.vertices[i];
        verts.append(vec(v.p));
        for (const PaintWeight& w : v.paint) paint.append(QJsonArray{i, w.material, w.weight});
    }
    for (const Edge& e : mesh.edges) {
        QJsonObject o;
        o["v"] = QJsonArray{e.v[0], e.v[1]};
        if (e.soft) o["soft"] = true;
        if (e.smooth) o["smooth"] = true;
        if (e.hidden) o["hidden"] = true;
        if (e.curve != kNone) o["curve"] = e.curve;
        edges.append(o);
    }
    for (const Face& f : mesh.faces) {
        QJsonObject o;
        o["outer"] = ids(f.outer);
        if (!f.holes.empty()) {
            QJsonArray hs;
            for (const auto& h : f.holes) hs.append(ids(h));
            o["holes"] = hs;
        }
        o["normal"] = vec(f.normal);
        if (f.hidden) o["hidden"] = true;
        o["front"] = sideToJson(f.front);
        o["back"] = sideToJson(f.back);
        faces.append(o);
    }
    root["vertices"] = verts;
    root["edges"] = edges;
    root["faces"] = faces;
    if (!paint.isEmpty()) root["paint"] = paint;
    root["nextCurve"] = mesh.nextCurve();
    return root;
}

bool meshFromJson(const QJsonObject& root, Mesh& mesh) {
    mesh = Mesh();
    for (const auto& v : root["vertices"].toArray()) {
        Vertex vx;
        vx.p = vec(v);
        mesh.vertices.push_back(vx);
    }
    const Id nv = (Id)mesh.vertices.size();
    for (const auto& pj : root["paint"].toArray()) {
        QJsonArray a = pj.toArray();
        if (a.size() != 3) continue;
        Id v = a[0].toInt(-1);
        float w = (float)a[2].toDouble();
        if (v < 0 || v >= nv || w <= 0) continue;
        mesh.vertices[v].paint.push_back({a[1].toInt(kNone), std::min(w, 1.0f)});
    }
    for (const auto& ej : root["edges"].toArray()) {
        QJsonObject o = ej.toObject();
        QJsonArray va = o["v"].toArray();
        if (va.size() != 2) continue;
        Edge e;
        e.v[0] = va[0].toInt();
        e.v[1] = va[1].toInt();
        if (e.v[0] < 0 || e.v[1] < 0 || e.v[0] >= nv || e.v[1] >= nv || e.v[0] == e.v[1]) continue;
        e.soft = o["soft"].toBool();
        e.smooth = o["smooth"].toBool();
        e.hidden = o["hidden"].toBool();
        e.curve = o.value("curve").toInt(kNone);
        mesh.edges.push_back(e);
    }
    for (const auto& fj : root["faces"].toArray()) {
        QJsonObject o = fj.toObject();
        Face f;
        f.outer = ids(o["outer"]);
        for (const auto& h : o["holes"].toArray()) f.holes.push_back(ids(h));
        bool ok = f.outer.size() >= 3;
        for (Id v : f.outer) ok = ok && v >= 0 && v < nv;
        for (const auto& h : f.holes)
            for (Id v : h) ok = ok && v >= 0 && v < nv;
        if (!ok) continue;
        f.normal = vec(o["normal"]).normalized();
        f.hidden = o["hidden"].toBool();
        f.front = sideFromJson(o["front"].toObject());
        f.back = sideFromJson(o["back"].toObject());
        mesh.faces.push_back(std::move(f));
    }
    mesh.compact(); // rebuilds lookup tables
    // Make sure every face loop edge exists, and normals are valid.
    for (Id f = 0; f < (Id)mesh.faces.size(); ++f) {
        if (mesh.faces[f].normal.length2() < 0.5) mesh.recomputeNormal(f);
        auto ensure = [&](const std::vector<Id>& L) {
            for (size_t i = 0; i < L.size(); ++i) mesh.addEdgeRaw(L[i], L[(i + 1) % L.size()]);
        };
        Face copy = mesh.faces[f];
        ensure(copy.outer);
        for (const auto& h : copy.holes) ensure(h);
    }
    mesh.setNextCurve(std::max<Id>(root.value("nextCurve").toInt(1), 1));
    for (const Edge& e : mesh.edges)
        if (e.curve >= mesh.nextCurve()) mesh.setNextCurve(e.curve + 1);
    mesh.touch();
    return true;
}

QByteArray modelToJson(const ModelState& stateIn, const QJsonObject& view) {
    const ModelState state = closedState(stateIn);
    QJsonObject root = meshToJson(state.mesh);
    root["format"] = "GNUSketch";
    root["version"] = 1;
    root["units"] = "meter";
    root["up"] = "Z";

    QJsonArray mats;
    for (const Material& m : state.materials) mats.append(materialToJson(m));
    root["materials"] = mats;

    QJsonArray guides;
    for (const Guide& g : state.guides) {
        QJsonObject o;
        o["kind"] = g.kind == Guide::Line ? "line" : "point";
        o["p"] = vec(g.p);
        if (g.kind == Guide::Line) o["dir"] = vec(g.dir);
        guides.append(o);
    }
    root["guides"] = guides;

    // Only definitions in use, renumbered, parents first; only their instances.
    std::map<Id, Id> defMap, instMap;
    std::vector<Id> defOrder, instOrder;
    {
        std::map<Id, std::vector<Id>> kids;
        for (Id i = 0; i < (Id)state.instances.size(); ++i) {
            const Instance& in = state.instances[i];
            if (in.alive && in.parent != kNone && in.def >= 0 && in.def < (Id)state.defs.size()) kids[in.parent].push_back(i);
        }
        // Reversed post-order: a parent comes before all that is inside it, even a
        // definition that is also used elsewhere. The first root's stays number 0.
        std::set<Id> seen;
        std::function<void(Id)> visit = [&](Id d) {
            if (!seen.insert(d).second) return;
            for (auto it = kids[d].rbegin(); it != kids[d].rend(); ++it) visit(state.instances[*it].def);
            defOrder.push_back(d);
        };
        for (Id i = (Id)state.instances.size() - 1; i >= 0; --i) {
            const Instance& in = state.instances[i];
            if (in.alive && in.parent == kNone && in.def >= 0 && in.def < (Id)state.defs.size()) visit(in.def);
        }
        std::reverse(defOrder.begin(), defOrder.end());
        for (Id k = 0; k < (Id)defOrder.size(); ++k) defMap[defOrder[k]] = k;
        for (Id i = 0; i < (Id)state.instances.size(); ++i) {
            const Instance& in = state.instances[i];
            if (!in.alive || in.def < 0 || in.def >= (Id)state.defs.size()) continue;
            if (in.parent != kNone && !defMap.count(in.parent)) continue;
            instMap[i] = (Id)instOrder.size();
            instOrder.push_back(i);
        }
    }
    QJsonArray comps, insts;
    for (Id dd : defOrder) {
        const ComponentDef& d = state.defs[dd];
        QJsonObject c = meshToJson(d.mesh);
        c["name"] = QString::fromStdString(d.name);
        c["uid"] = QString::fromStdString(d.uid);
        if (!d.prefab.empty()) c["prefab"] = QString::fromStdString(d.prefab);
        if (!d.states.empty()) {
            // Vertex ids as in the file, which holds the mesh compacted.
            Mesh copy = d.mesh;
            std::vector<Id> vmap;
            copy.compact(&vmap);
            QJsonArray states;
            for (const AnimState& as : d.states) {
                QJsonArray vs, ps, kidStates;
                for (size_t k = 0; k < as.verts.size(); ++k) {
                    const Id v = as.verts[k];
                    if (v < 0 || v >= (Id)vmap.size() || vmap[v] == kNone) continue;
                    vs.append(vmap[v]);
                    ps.append(as.pos[k].x);
                    ps.append(as.pos[k].y);
                    ps.append(as.pos[k].z);
                }
                for (const auto& [id, xf] : as.children) {
                    auto it = instMap.find(id);
                    if (it == instMap.end()) continue;
                    QJsonArray t;
                    for (double v : xf.m) t.append(v);
                    kidStates.append(QJsonObject{{"instance", it->second}, {"transform", t}});
                }
                QJsonObject so;
                so["name"] = QString::fromStdString(as.name);
                so["vertices"] = vs;
                so["positions"] = ps;
                if (!kidStates.isEmpty()) so["children"] = kidStates;
                states.append(so);
            }
            c["states"] = states;
        }
        comps.append(c);
    }
    for (Id i : instOrder) {
        const Instance& in = state.instances[i];
        QJsonObject o;
        o["component"] = defMap[in.def];
        if (in.parent != kNone) o["parent"] = defMap[in.parent];
        QJsonArray xf;
        for (double v : in.xf.m) xf.append(v);
        o["transform"] = xf;
        if (in.hidden) o["hidden"] = true;
        if (in.entity.isLive() || !in.entity.targetname.empty()) {
            QJsonObject e;
            e["classname"] = QString::fromStdString(in.entity.classname);
            e["targetname"] = QString::fromStdString(in.entity.targetname);
            QJsonArray targets;
            for (const EntityTarget& t : in.entity.targets) targets.append(QJsonObject{{"name", QString::fromStdString(t.name)}, {"delay", t.delay}});
            if (!targets.isEmpty()) e["targets"] = targets;
            e["startState"] = QString::fromStdString(in.entity.startState);
            e["script"] = QString::fromStdString(in.entity.script);
            if (in.entity.physics) {
                e["physics"] = true;
                e["mass"] = in.entity.mass;
                e["friction"] = in.entity.friction;
                e["bounce"] = in.entity.bounce;
            }
            o["entity"] = e;
        }
        insts.append(o);
    }
    if (!comps.isEmpty()) {
        root["components"] = comps;
        root["instances"] = insts;
    }
    QJsonArray objects;
    for (const SceneObject& o : state.objects)
        if (o.alive) objects.append(objectToJson(o));
    if (!objects.isEmpty()) root["objects"] = objects;
    root["view"] = view;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool saveNative(const Document& doc, const QString& path, const QJsonObject& view, QString* error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(modelToJson(doc.state, view));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool modelFromJson(const QByteArray& json, ModelState& st, QJsonObject* view, QString* error) {
    QJsonParseError pe;
    QJsonDocument jd = QJsonDocument::fromJson(json, &pe);
    if (jd.isNull() || !jd.isObject()) {
        if (error) *error = QStringLiteral("Invalid file: %1").arg(pe.errorString());
        return false;
    }
    QJsonObject root = jd.object();
    if (root["format"].toString() != "GNUSketch") {
        if (error) *error = QStringLiteral("Not a GNUSketch model");
        return false;
    }
    st = ModelState();
    meshFromJson(root, st.mesh);
    for (const auto& mj : root["materials"].toArray()) st.materials.push_back(materialFromJson(mj.toObject()));
    // Drop dangling material references.
    for (Face& fc : st.mesh.faces) {
        if (fc.front.material >= (Id)st.materials.size()) fc.front.material = kNone;
        if (fc.back.material >= (Id)st.materials.size()) fc.back.material = kNone;
    }
    for (const auto& cj : root["components"].toArray()) {
        QJsonObject o = cj.toObject();
        ComponentDef d;
        meshFromJson(o, d.mesh);
        d.name = o["name"].toString().toStdString();
        d.uid = o["uid"].toString().toStdString();
        d.prefab = o["prefab"].toString().toStdString();
        if (d.uid.empty()) d.uid = newComponentUid();
        for (Face& fc : d.mesh.faces) {
            if (fc.front.material >= (Id)st.materials.size()) fc.front.material = kNone;
            if (fc.back.material >= (Id)st.materials.size()) fc.back.material = kNone;
        }
        for (const auto& sj : o["states"].toArray()) {
            const QJsonObject so = sj.toObject();
            AnimState as;
            as.name = so["name"].toString().toStdString();
            const QJsonArray vs = so["vertices"].toArray(), ps = so["positions"].toArray();
            if (ps.size() != vs.size() * 3) continue;
            for (int k = 0; k < vs.size(); ++k) {
                const Id v = vs[k].toInt(kNone);
                if (v < 0 || v >= (Id)d.mesh.vertices.size()) continue;
                as.verts.push_back(v);
                as.pos.emplace_back(ps[k * 3].toDouble(), ps[k * 3 + 1].toDouble(), ps[k * 3 + 2].toDouble());
            }
            for (const auto& kj : so["children"].toArray()) {
                const QJsonObject ko = kj.toObject();
                const QJsonArray t = ko["transform"].toArray();
                if (t.size() != 16) continue;
                Mat4 xf;
                for (int k = 0; k < 16; ++k) xf.m[k] = t[k].toDouble();
                as.children.emplace_back(ko["instance"].toInt(kNone), xf);
            }
            d.states.push_back(std::move(as));
        }
        st.defs.push_back(std::move(d));
    }
    std::map<int, Id> fileInstance; // instance number in the file -> id here
    int fileIndex = -1;
    for (const auto& ij : root["instances"].toArray()) {
        ++fileIndex;
        QJsonObject o = ij.toObject();
        Instance in;
        in.def = o["component"].toInt(kNone);
        if (in.def < 0 || in.def >= (Id)st.defs.size()) continue;
        in.parent = o.value("parent").toInt(kNone);
        // Parents come before what is inside them: a file cannot make a
        // component contain itself.
        if (in.parent != kNone && (in.parent < 0 || in.parent >= in.def)) continue;
        QJsonArray xf = o["transform"].toArray();
        if (xf.size() == 16)
            for (int i = 0; i < 16; ++i) in.xf.m[i] = xf[i].toDouble();
        in.hidden = o["hidden"].toBool();
        if (o.contains("entity")) {
            const QJsonObject e = o["entity"].toObject();
            in.entity.classname = e["classname"].toString().toStdString();
            in.entity.targetname = e["targetname"].toString().toStdString();
            for (const auto& tj : e["targets"].toArray()) {
                const QJsonObject to = tj.toObject();
                in.entity.targets.push_back({to["name"].toString().toStdString(), std::max(0.0, to["delay"].toDouble())});
            }
            // Files from before there could be several.
            if (!e["target"].toString().isEmpty()) in.entity.targets.push_back({e["target"].toString().toStdString(), 0});
            in.entity.startState = e["startState"].toString().toStdString();
            in.entity.script = e["script"].toString().toStdString();
            in.entity.physics = e["physics"].toBool();
            in.entity.mass = e.value("mass").toDouble(in.entity.mass);
            in.entity.friction = e.value("friction").toDouble(in.entity.friction);
            in.entity.bounce = e.value("bounce").toDouble(in.entity.bounce);
        }
        st.instances.push_back(in);
        fileInstance[fileIndex] = (Id)st.instances.size() - 1;
    }
    for (ComponentDef& d : st.defs)
        for (AnimState& as : d.states)
            for (auto& [id, xf] : as.children) {
                auto it = fileInstance.find(id);
                id = it == fileInstance.end() ? kNone : it->second;
            }
    for (const auto& gj : root["guides"].toArray()) {
        QJsonObject o = gj.toObject();
        Guide g;
        g.kind = o["kind"].toString() == "point" ? Guide::Point : Guide::Line;
        g.p = vec(o["p"]);
        g.dir = vec(o["dir"]).normalized();
        st.guides.push_back(g);
    }
    for (const auto& oj : root["objects"].toArray()) st.objects.push_back(objectFromJson(oj.toObject()));
    if (view) *view = root["view"].toObject();
    return true;
}

bool loadNative(Document& doc, const QString& path, QJsonObject* view, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    ModelState st;
    if (!modelFromJson(f.readAll(), st, view, error)) return false;
    doc.state = std::move(st);
    doc.selection.clear();
    doc.filePath = path.toStdString();
    doc.modified = false;
    doc.currentMaterial = kNone;
    ++doc.materialsRevision;
    doc.clearHistory();
    return true;
}

namespace {
QJsonArray colorJson(const Color& c) { return QJsonArray{c.r, c.g, c.b, c.a}; }
Color colorFrom(const QJsonValue& j, const Color& def) {
    QJsonArray a = j.toArray();
    return a.size() == 4 ? Color((float)a[0].toDouble(), (float)a[1].toDouble(), (float)a[2].toDouble(), (float)a[3].toDouble()) : def;
}
} // namespace

QJsonObject styleToJson(const RenderStyle& s) {
    QJsonObject st;
    st["faceStyle"] = (int)s.faceStyle;
    st["xray"] = s.xray;
    st["edges"] = s.edges;
    st["backEdges"] = s.backEdges;
    st["profiles"] = s.profiles;
    st["profileWidth"] = s.profileWidth;
    st["axes"] = s.showAxes;
    st["guides"] = s.showGuides;
    st["objects"] = s.showObjects;
    st["sky"] = s.sky;
    st["ground"] = s.ground;
    st["shadows"] = s.shadows;
    st["ambientOcclusion"] = s.ambientOcclusion;
    st["antialiasing"] = s.antialiasing;
    st["backfaceCulling"] = s.backfaceCulling;
    st["sun"] = s.sun;
    st["sunAzimuth"] = s.sunAzimuth;
    st["sunAltitude"] = s.sunAltitude;
    st["sunIntensity"] = s.sunIntensity;
    st["skyColor"] = colorJson(s.skyColor);
    st["groundColor"] = colorJson(s.groundColor);
    st["background"] = colorJson(s.background);
    st["frontColor"] = colorJson(s.frontColor);
    st["backColor"] = colorJson(s.backColor);
    return st;
}

void styleFromJson(const QJsonObject& st, RenderStyle& s) {
    s.faceStyle = (RenderStyle::FaceStyle)std::clamp(st["faceStyle"].toInt((int)s.faceStyle), 0, 4);
    s.xray = st["xray"].toBool(s.xray);
    s.edges = st["edges"].toBool(s.edges);
    s.backEdges = st["backEdges"].toBool(s.backEdges);
    s.profiles = st["profiles"].toBool(s.profiles);
    s.profileWidth = (float)st["profileWidth"].toDouble(s.profileWidth);
    s.showAxes = st["axes"].toBool(s.showAxes);
    s.showGuides = st["guides"].toBool(s.showGuides);
    s.showObjects = st["objects"].toBool(s.showObjects);
    s.sky = st["sky"].toBool(s.sky);
    s.ground = st["ground"].toBool(s.ground);
    s.shadows = st["shadows"].toBool(s.shadows);
    s.ambientOcclusion = st["ambientOcclusion"].toBool(s.ambientOcclusion);
    s.antialiasing = st["antialiasing"].toBool(s.antialiasing);
    s.backfaceCulling = st["backfaceCulling"].toBool(s.backfaceCulling);
    s.sun = st["sun"].toBool(s.sun);
    s.sunAzimuth = st["sunAzimuth"].toDouble(s.sunAzimuth);
    s.sunAltitude = st["sunAltitude"].toDouble(s.sunAltitude);
    s.sunIntensity = st["sunIntensity"].toDouble(s.sunIntensity);
    s.skyColor = colorFrom(st["skyColor"], s.skyColor);
    s.groundColor = colorFrom(st["groundColor"], s.groundColor);
    s.background = colorFrom(st["background"], s.background);
    s.frontColor = colorFrom(st["frontColor"], s.frontColor);
    s.backColor = colorFrom(st["backColor"], s.backColor);
}

} // namespace gs
