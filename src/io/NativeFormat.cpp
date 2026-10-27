// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeFormat.h"

#include "ImageUtil.h"
#include "core/Components.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <map>

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

QJsonObject meshToJson(const Mesh& meshIn) {
    Mesh mesh = meshIn;
    mesh.compact();
    QJsonObject root;
    QJsonArray verts, edges, faces;
    for (const Vertex& v : mesh.vertices) verts.append(vec(v.p));
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
    for (const Material& m : state.materials) {
        QJsonObject o;
        o["name"] = QString::fromStdString(m.name);
        o["color"] = QJsonArray{m.color.r, m.color.g, m.color.b, m.color.a};
        o["opacity"] = m.opacity;
        if (m.hasTexture()) {
            QJsonObject t;
            t["mime"] = QString::fromStdString(m.texture.mime);
            t["width"] = m.texture.worldWidth;
            t["height"] = m.texture.worldHeight;
            QByteArray bytes(reinterpret_cast<const char*>(m.texture.data->data()), (qsizetype)m.texture.data->size());
            t["data"] = QString::fromLatin1(bytes.toBase64());
            o["texture"] = t;
        }
        mats.append(o);
    }
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

    // Only definitions that are still placed somewhere, renumbered.
    std::map<Id, Id> defMap;
    QJsonArray comps, insts;
    for (const Instance& in : state.instances) {
        if (!in.alive || in.def < 0 || in.def >= (Id)state.defs.size()) continue;
        auto it = defMap.find(in.def);
        if (it == defMap.end()) {
            const ComponentDef& d = state.defs[in.def];
            QJsonObject c = meshToJson(d.mesh);
            c["name"] = QString::fromStdString(d.name);
            c["uid"] = QString::fromStdString(d.uid);
            it = defMap.emplace(in.def, (Id)comps.size()).first;
            comps.append(c);
        }
        QJsonObject o;
        o["component"] = it->second;
        QJsonArray xf;
        for (double v : in.xf.m) xf.append(v);
        o["transform"] = xf;
        if (in.hidden) o["hidden"] = true;
        insts.append(o);
    }
    if (!comps.isEmpty()) {
        root["components"] = comps;
        root["instances"] = insts;
    }
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
    for (const auto& mj : root["materials"].toArray()) {
        QJsonObject o = mj.toObject();
        Material m;
        m.name = o["name"].toString().toStdString();
        QJsonArray c = o["color"].toArray();
        if (c.size() >= 3)
            m.color = Color((float)c[0].toDouble(), (float)c[1].toDouble(), (float)c[2].toDouble(),
                            c.size() > 3 ? (float)c[3].toDouble() : 1.0f);
        m.opacity = (float)o.value("opacity").toDouble(1.0);
        if (o.contains("texture")) {
            QJsonObject t = o["texture"].toObject();
            QByteArray bytes = QByteArray::fromBase64(t["data"].toString().toLatin1());
            Texture tex;
            if (textureFromBytes(bytes, tex)) {
                tex.worldWidth = t.value("width").toDouble(1.0);
                tex.worldHeight = t.value("height").toDouble(1.0);
                m.texture = tex;
            }
        }
        st.materials.push_back(m);
    }
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
        if (d.uid.empty()) d.uid = newComponentUid();
        for (Face& fc : d.mesh.faces) {
            if (fc.front.material >= (Id)st.materials.size()) fc.front.material = kNone;
            if (fc.back.material >= (Id)st.materials.size()) fc.back.material = kNone;
        }
        st.defs.push_back(std::move(d));
    }
    for (const auto& ij : root["instances"].toArray()) {
        QJsonObject o = ij.toObject();
        Instance in;
        in.def = o["component"].toInt(kNone);
        if (in.def < 0 || in.def >= (Id)st.defs.size()) continue;
        QJsonArray xf = o["transform"].toArray();
        if (xf.size() == 16)
            for (int i = 0; i < 16; ++i) in.xf.m[i] = xf[i].toDouble();
        in.hidden = o["hidden"].toBool();
        st.instances.push_back(in);
    }
    for (const auto& gj : root["guides"].toArray()) {
        QJsonObject o = gj.toObject();
        Guide g;
        g.kind = o["kind"].toString() == "point" ? Guide::Point : Guide::Line;
        g.p = vec(o["p"]);
        g.dir = vec(o["dir"]).normalized();
        st.guides.push_back(g);
    }
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

} // namespace gs
