// SPDX-License-Identifier: GPL-3.0-or-later
#include "GltfExport.h"

#include "core/Components.h"
#include "core/Triangulate.h"
#include "ImageUtil.h"

#include <QBuffer>
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
    const Mesh flat = flattenModel(doc.state);
    const Mesh& m = flat;
    QByteArray bin;
    auto align4 = [&] { while (bin.size() % 4) bin.append('\0'); };

    QJsonArray bufferViews, accessors, meshesPrims, materials, images, textures, samplers;
    std::map<Id, int> matIndex;

    auto addImage = [&](const QByteArray& bytes, const QString& mime) -> QJsonObject {
        QJsonObject img;
        img["uri"] = QStringLiteral("data:%1;base64,").arg(mime) + QString::fromLatin1(bytes.toBase64());
        images.append(img);
        QJsonObject tex;
        tex["source"] = images.size() - 1;
        tex["sampler"] = 0;
        textures.append(tex);
        QJsonObject ti;
        ti["index"] = textures.size() - 1;
        return ti;
    };
    auto pngBytes = [](const QImage& img) {
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        return bytes;
    };
    auto rawBytes = [](const Texture& t) {
        return QByteArray(reinterpret_cast<const char*>(t.data->data()), (qsizetype)t.data->size());
    };

    auto addMaterial = [&](Id mid) -> int {
        auto it = matIndex.find(mid);
        if (it != matIndex.end()) return it->second;
        const Material* mat = doc.material(mid);
        QJsonObject pbr;
        Color c = mat ? mat->color : defaultFrontColor();
        float a = mat ? mat->opacity : 1.0f;
        QJsonObject mo;
        const bool useBase = !mat || !mat->advanced() || mat->baseStrength >= 0.5f;
        if (mat && mat->hasTexture() && useBase) {
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
        if (mat && mat->advanced()) {
            // glTF keeps roughness (green) and metallic (blue) in one image.
            const bool hasM = mat->hasMap(MapMetallic), hasR = mat->hasMap(MapRoughness);
            pbr["metallicFactor"] = mat->maps[MapMetallic].strength;
            pbr["roughnessFactor"] = mat->maps[MapRoughness].strength;
            if (hasM || hasR) {
                QImage mi = hasM ? decodeTexture(mat->maps[MapMetallic].tex) : QImage();
                QImage ri = hasR ? decodeTexture(mat->maps[MapRoughness].tex) : QImage();
                const QSize size = mi.isNull() ? ri.size() : ri.isNull() ? mi.size() : mi.size().expandedTo(ri.size());
                if (!mi.isNull()) mi = mi.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                if (!ri.isNull()) ri = ri.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                QImage orm(size, QImage::Format_RGB32);
                for (int y = 0; y < size.height(); ++y)
                    for (int x = 0; x < size.width(); ++x)
                        orm.setPixel(x, y, qRgb(255, ri.isNull() ? 255 : qRed(ri.pixel(x, y)), mi.isNull() ? 255 : qRed(mi.pixel(x, y))));
                pbr["metallicRoughnessTexture"] = addImage(pngBytes(orm), QStringLiteral("image/png"));
            }
            if (mat->hasMap(MapNormal)) {
                QJsonObject ti;
                if (mat->flipNormalY) {
                    QImage n = decodeTexture(mat->maps[MapNormal].tex).convertToFormat(QImage::Format_RGB32);
                    for (int y = 0; y < n.height(); ++y)
                        for (int x = 0; x < n.width(); ++x) {
                            const QRgb c = n.pixel(x, y);
                            n.setPixel(x, y, qRgb(qRed(c), 255 - qGreen(c), qBlue(c)));
                        }
                    ti = addImage(pngBytes(n), QStringLiteral("image/png"));
                } else {
                    ti = addImage(rawBytes(mat->maps[MapNormal].tex), QString::fromStdString(mat->maps[MapNormal].tex.mime));
                }
                ti["scale"] = mat->maps[MapNormal].strength;
                mo["normalTexture"] = ti;
            }
            if (mat->hasMap(MapOcclusion)) {
                QJsonObject ti = addImage(rawBytes(mat->maps[MapOcclusion].tex), QString::fromStdString(mat->maps[MapOcclusion].tex.mime));
                ti["strength"] = mat->maps[MapOcclusion].strength;
                mo["occlusionTexture"] = ti;
            }
            const float es = mat->maps[MapEmission].strength;
            if (mat->emission.r > 0 || mat->emission.g > 0 || mat->emission.b > 0) {
                const float k = std::min(es, 1.0f);
                mo["emissiveFactor"] = QJsonArray{mat->emission.r * k, mat->emission.g * k, mat->emission.b * k};
                if (mat->hasMap(MapEmission))
                    mo["emissiveTexture"] = addImage(rawBytes(mat->maps[MapEmission].tex), QString::fromStdString(mat->maps[MapEmission].tex.mime));
            }
        }
        mo["pbrMetallicRoughness"] = pbr;
        mo["name"] = mat ? QString::fromStdString(mat->name) : QStringLiteral("Default");
        mo["doubleSided"] = true;
        if (a < 1.0f || (mat && mat->blended())) mo["alphaMode"] = "BLEND";
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

    // Lights (KHR_lights_punctual) and cameras as extra nodes. Both look along
    // their local -Z, in the file's Y-up axes.
    {
        QJsonArray nodes = root["nodes"].toArray(), sceneNodes = scene["nodes"].toArray(), lights, cameras;
        auto yUp = [](const Vec3& v) { return Vec3(v.x, v.z, -v.y); };
        for (const SceneObject& o : doc.state.objects) {
            if (!o.alive || (o.isLight() && !o.enabled)) continue;
            QJsonObject n;
            n["name"] = QString::fromStdString(o.name);
            const Vec3 pos = yUp(o.pos);
            n["translation"] = QJsonArray{pos.x, pos.y, pos.z};
            if (o.aims()) {
                // Rotation whose -Z axis points along the aim, Y kept up.
                const Vec3 back = -(yUp(o.target) - pos).normalized();
                Vec3 x = Vec3(0, 1, 0).cross(back);
                x = x.length2() < 1e-9 ? Vec3(1, 0, 0) : x.normalized();
                const Vec3 y = back.cross(x);
                const double m00 = x.x, m01 = y.x, m02 = back.x, m10 = x.y, m11 = y.y, m12 = back.y, m20 = x.z, m21 = y.z, m22 = back.z;
                const double tr = m00 + m11 + m22;
                double q[4];
                if (tr > 0) {
                    const double s = std::sqrt(tr + 1) * 2;
                    q[3] = s / 4; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
                } else if (m00 > m11 && m00 > m22) {
                    const double s = std::sqrt(1 + m00 - m11 - m22) * 2;
                    q[3] = (m21 - m12) / s; q[0] = s / 4; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
                } else if (m11 > m22) {
                    const double s = std::sqrt(1 + m11 - m00 - m22) * 2;
                    q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = s / 4; q[2] = (m12 + m21) / s;
                } else {
                    const double s = std::sqrt(1 + m22 - m00 - m11) * 2;
                    q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = s / 4;
                }
                n["rotation"] = QJsonArray{q[0], q[1], q[2], q[3]};
            }
            if (o.isLight()) {
                QJsonObject l;
                l["name"] = QString::fromStdString(o.name);
                l["color"] = QJsonArray{o.color.r, o.color.g, o.color.b};
                l["intensity"] = o.intensity;
                if (o.kind == SceneObject::DirectionalLight) {
                    l["type"] = "directional";
                } else {
                    l["range"] = o.range;
                    l["type"] = o.kind == SceneObject::SpotLight ? "spot" : "point";
                    if (o.kind == SceneObject::SpotLight) {
                        const double outer = o.coneAngle * 0.5 * kPi / 180;
                        QJsonObject spot;
                        spot["outerConeAngle"] = outer;
                        spot["innerConeAngle"] = outer * (1 - std::clamp(o.softness, 0.0, 1.0));
                        l["spot"] = spot;
                    }
                }
                lights.append(l);
                n["extensions"] = QJsonObject{{"KHR_lights_punctual", QJsonObject{{"light", lights.size() - 1}}}};
            } else {
                QJsonObject c;
                c["name"] = QString::fromStdString(o.name);
                if (o.perspective) {
                    c["type"] = "perspective";
                    c["perspective"] = QJsonObject{{"yfov", o.fov * kPi / 180}, {"znear", 0.01}};
                } else {
                    const double mag = (o.target - o.pos).length() * std::tan(o.fov * 0.5 * kPi / 180);
                    c["type"] = "orthographic";
                    c["orthographic"] = QJsonObject{{"xmag", mag}, {"ymag", mag}, {"znear", 0.01}, {"zfar", 1000.0}};
                }
                cameras.append(c);
                n["camera"] = cameras.size() - 1;
            }
            sceneNodes.append(nodes.size());
            nodes.append(n);
        }
        root["nodes"] = nodes;
        scene["nodes"] = sceneNodes;
        root["scenes"] = QJsonArray{scene};
        if (!cameras.isEmpty()) root["cameras"] = cameras;
        if (!lights.isEmpty()) {
            root["extensionsUsed"] = QJsonArray{"KHR_lights_punctual"};
            root["extensions"] = QJsonObject{{"KHR_lights_punctual", QJsonObject{{"lights", lights}}}};
        }
    }
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
