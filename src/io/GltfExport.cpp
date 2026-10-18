// SPDX-License-Identifier: GPL-3.0-or-later
#include "GltfExport.h"

#include "core/Triangulate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cstring>
#include <map>

namespace gs {

FaceTriangles triangulateFace(const Mesh& m, Id f, bool frontSide, const Material* mat) {
    FaceTriangles r;
    const Face& F = m.faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    std::vector<std::vector<Vec2>> loops;
    auto add = [&](const std::vector<Id>& L) {
        std::vector<Vec2> l;
        for (Id id : L) {
            const Vec3& p = m.pos(id);
            l.emplace_back(p.dot(u), p.dot(v));
            r.pos.push_back(p);
            r.uv.push_back(faceTexCoord(F, frontSide, p, mat));
        }
        loops.push_back(std::move(l));
    };
    add(F.outer);
    for (const auto& h : F.holes) add(h);
    r.tris = triangulatePolygon(loops);
    return r;
}

bool exportGltf(const Document& doc, const QString& path, QString* error) {
    const Mesh& m = doc.mesh();
    QByteArray bin;
    auto align4 = [&] { while (bin.size() % 4) bin.append('\0'); };

    QJsonArray bufferViews, accessors, meshesPrims, materials, images, textures, samplers;
    std::map<Id, int> matIndex;

    auto addMaterial = [&](Id mid) -> int {
        auto it = matIndex.find(mid);
        if (it != matIndex.end()) return it->second;
        const Material* mat = doc.material(mid);
        QJsonObject pbr;
        Color c = mat ? mat->color : defaultFrontColor();
        float a = mat ? mat->opacity : 1.0f;
        QJsonObject mo;
        if (mat && mat->hasTexture()) {
            QByteArray bytes(reinterpret_cast<const char*>(mat->texture.data->data()), (qsizetype)mat->texture.data->size());
            QJsonObject img;
            img["uri"] = QStringLiteral("data:%1;base64,").arg(QString::fromStdString(mat->texture.mime)) +
                         QString::fromLatin1(bytes.toBase64());
            images.append(img);
            QJsonObject tex;
            tex["source"] = images.size() - 1;
            tex["sampler"] = 0;
            textures.append(tex);
            QJsonObject ti;
            ti["index"] = textures.size() - 1;
            pbr["baseColorTexture"] = ti;
            pbr["baseColorFactor"] = QJsonArray{1, 1, 1, a};
        } else {
            pbr["baseColorFactor"] = QJsonArray{c.r, c.g, c.b, a};
        }
        pbr["metallicFactor"] = 0.0;
        pbr["roughnessFactor"] = 0.9;
        mo["pbrMetallicRoughness"] = pbr;
        mo["name"] = mat ? QString::fromStdString(mat->name) : QStringLiteral("Default");
        mo["doubleSided"] = true;
        if (a < 1.0f) mo["alphaMode"] = "BLEND";
        materials.append(mo);
        int idx = materials.size() - 1;
        matIndex[mid] = idx;
        return idx;
    };

    std::map<Id, std::vector<Id>> byMat;
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && !m.faces[f].hidden) byMat[m.faces[f].front.material].push_back(f);

    for (const auto& [mid, flist] : byMat) {
        const Material* mat = doc.material(mid);
        std::vector<float> P, N, T;
        std::vector<uint32_t> I;
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (Id f : flist) {
            FaceTriangles ft = triangulateFace(m, f, true, mat);
            Vec3 n = m.faces[f].normal;
            uint32_t base = (uint32_t)(P.size() / 3);
            for (size_t i = 0; i < ft.pos.size(); ++i) {
                Vec3 p(ft.pos[i].x, ft.pos[i].z, -ft.pos[i].y); // Z-up -> Y-up
                float pf[3] = {(float)p.x, (float)p.y, (float)p.z};
                for (int k = 0; k < 3; ++k) {
                    P.push_back(pf[k]);
                    mn[k] = std::min(mn[k], pf[k]);
                    mx[k] = std::max(mx[k], pf[k]);
                }
                N.push_back((float)n.x); N.push_back((float)n.z); N.push_back((float)-n.y);
                T.push_back((float)ft.uv[i].x);
                T.push_back((float)(1.0 - ft.uv[i].y)); // glTF uv origin is top-left
            }
            for (const auto& t : ft.tris)
                for (int k : t) I.push_back(base + (uint32_t)k);
        }
        if (I.empty()) continue;
        auto addView = [&](const void* data, size_t bytes, int target) {
            align4();
            QJsonObject bv;
            bv["buffer"] = 0;
            bv["byteOffset"] = (qint64)bin.size();
            bv["byteLength"] = (qint64)bytes;
            bv["target"] = target;
            bin.append(reinterpret_cast<const char*>(data), (qsizetype)bytes);
            bufferViews.append(bv);
            return bufferViews.size() - 1;
        };
        auto addAccessor = [&](int view, int compType, int count, const char* type) {
            QJsonObject a;
            a["bufferView"] = view;
            a["componentType"] = compType;
            a["count"] = count;
            a["type"] = type;
            accessors.append(a);
            return accessors.size() - 1;
        };
        int vp = addView(P.data(), P.size() * 4, 34962);
        int ap = addAccessor(vp, 5126, (int)P.size() / 3, "VEC3");
        {
            QJsonObject a = accessors[ap].toObject();
            a["min"] = QJsonArray{mn[0], mn[1], mn[2]};
            a["max"] = QJsonArray{mx[0], mx[1], mx[2]};
            accessors[ap] = a;
        }
        int an = addAccessor(addView(N.data(), N.size() * 4, 34962), 5126, (int)N.size() / 3, "VEC3");
        int at = addAccessor(addView(T.data(), T.size() * 4, 34962), 5126, (int)T.size() / 2, "VEC2");
        int ai = addAccessor(addView(I.data(), I.size() * 4, 34963), 5125, (int)I.size(), "SCALAR");
        QJsonObject attrs;
        attrs["POSITION"] = ap;
        attrs["NORMAL"] = an;
        attrs["TEXCOORD_0"] = at;
        QJsonObject prim;
        prim["attributes"] = attrs;
        prim["indices"] = ai;
        prim["material"] = addMaterial(mid);
        meshesPrims.append(prim);
    }

    QJsonObject root;
    QJsonObject asset;
    asset["version"] = "2.0";
    asset["generator"] = "GNUSketch";
    root["asset"] = asset;
    root["scene"] = 0;
    QJsonObject scene;
    scene["nodes"] = QJsonArray{0};
    root["scenes"] = QJsonArray{scene};
    QJsonObject node;
    node["mesh"] = 0;
    node["name"] = "Model";
    root["nodes"] = QJsonArray{node};
    QJsonObject meshObj;
    meshObj["primitives"] = meshesPrims;
    meshObj["name"] = "Model";
    if (!meshesPrims.isEmpty()) root["meshes"] = QJsonArray{meshObj};
    else root["nodes"] = QJsonArray{QJsonObject{{"name", "Model"}}};
    if (!materials.isEmpty()) root["materials"] = materials;
    if (!images.isEmpty()) {
        QJsonObject sampler;
        sampler["magFilter"] = 9729;
        sampler["minFilter"] = 9987;
        sampler["wrapS"] = 10497;
        sampler["wrapT"] = 10497;
        samplers.append(sampler);
        root["samplers"] = samplers;
        root["images"] = images;
        root["textures"] = textures;
    }
    if (!bin.isEmpty()) {
        QJsonObject buf;
        buf["byteLength"] = (qint64)bin.size();
        buf["uri"] = QStringLiteral("data:application/octet-stream;base64,") + QString::fromLatin1(bin.toBase64());
        root["buffers"] = QJsonArray{buf};
        root["bufferViews"] = bufferViews;
        root["accessors"] = accessors;
    }

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace gs
