// SPDX-License-Identifier: GPL-3.0-or-later
#include "ObjIO.h"

#include "ImageUtil.h"
#include "core/Components.h"
#include "core/Triangulate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <map>

namespace gs {

namespace {

QString num(double v) { return QString::number(v, 'g', 10); }

QString safeName(const std::string& s, int idx) {
    QString n = QString::fromStdString(s);
    n.replace(QRegularExpression("[^A-Za-z0-9_\\-]"), "_");
    if (n.isEmpty()) n = QStringLiteral("material_%1").arg(idx);
    return n;
}

Vec3 toFile(const Vec3& p, bool yUp, double s) { return yUp ? Vec3(p.x, p.z, -p.y) * s : p * s; }
Vec3 fromFile(const Vec3& p, bool yUp, double s) { return (yUp ? Vec3(p.x, -p.z, p.y) : p) * s; }

} // namespace

bool exportObj(const Document& doc, const QString& path, const ObjExportOptions& opt, QString* error) {
    const Mesh flat = flattenModel(doc.state);
    const Mesh& m = flat;
    QFileInfo fi(path);
    const QString base = fi.completeBaseName();
    const QString dir = fi.absolutePath();
    const QString mtlName = base + ".mtl";

    // Materials used (front side; faces without material use the default).
    std::map<Id, QString> matNames;
    QString mtl;
    QTextStream ms(&mtl);
    ms << "# GNUSketch material library\n";
    ms << "newmtl gs_default\nKd 1 1 1\nd 1\n\n";
    for (Id i = 0; i < (Id)doc.state.materials.size(); ++i) {
        const Material& mat = doc.state.materials[i];
        QString n = safeName(mat.name, i);
        matNames[i] = n;
        ms << "newmtl " << n << "\n";
        ms << "Kd " << num(mat.color.r) << " " << num(mat.color.g) << " " << num(mat.color.b) << "\n";
        ms << "d " << num(mat.opacity) << "\n";
        if (mat.hasTexture() && opt.exportTextures) {
            QString tex = base + "_" + n + "." + QString::fromStdString(mat.texture.fileExtension());
            QFile tf(dir + "/" + tex);
            if (tf.open(QIODevice::WriteOnly))
                tf.write(reinterpret_cast<const char*>(mat.texture.data->data()), (qint64)mat.texture.data->size());
            ms << "map_Kd " << tex << "\n";
        }
        ms << "\n";
    }

    QString out;
    QTextStream os(&out);
    os << "# Exported by GNUSketch\n";
    os << "mtllib " << mtlName << "\n";

    // Vertex positions (shared).
    std::vector<int> vIndex(m.vertices.size(), 0);
    int vc = 0;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
        if (!m.vertices[v].alive) continue;
        vIndex[v] = ++vc;
        Vec3 p = toFile(m.vertices[v].p, opt.yUp, opt.scale);
        os << "v " << num(p.x) << " " << num(p.y) << " " << num(p.z) << "\n";
    }

    // Group faces by front material.
    std::map<Id, std::vector<Id>> byMat;
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) byMat[m.faces[f].front.material].push_back(f);

    int vtc = 0, vnc = 0;
    for (const auto& [mid, flist] : byMat) {
        const Material* mat = doc.material(mid);
        os << "usemtl " << (mat ? matNames[mid] : QStringLiteral("gs_default")) << "\n";
        for (Id f : flist) {
            const Face& F = m.faces[f];
            Vec3 n = toFile(F.normal, opt.yUp, 1.0).normalized();
            os << "vn " << num(n.x) << " " << num(n.y) << " " << num(n.z) << "\n";
            ++vnc;
            auto vt = [&](Id v) {
                Vec2 uv = faceTexCoord(F, true, m.pos(v), mat);
                os << "vt " << num(uv.x) << " " << num(uv.y) << "\n";
                return ++vtc;
            };
            if (F.holes.empty()) {
                std::vector<int> t;
                for (Id v : F.outer) t.push_back(vt(v));
                os << "f";
                for (size_t i = 0; i < F.outer.size(); ++i) os << " " << vIndex[F.outer[i]] << "/" << t[i] << "/" << vnc;
                os << "\n";
            } else {
                Vec3 u, w;
                planeBasis(F.normal, u, w);
                std::vector<std::vector<Vec2>> loops;
                std::vector<Id> flat;
                auto add = [&](const std::vector<Id>& L) {
                    std::vector<Vec2> l;
                    for (Id id : L) {
                        l.emplace_back(m.pos(id).dot(u), m.pos(id).dot(w));
                        flat.push_back(id);
                    }
                    loops.push_back(l);
                };
                add(F.outer);
                for (const auto& h : F.holes) add(h);
                std::vector<int> t;
                for (Id v : flat) t.push_back(vt(v));
                for (const auto& tri : triangulatePolygon(loops)) {
                    os << "f";
                    for (int k : tri) os << " " << vIndex[flat[k]] << "/" << t[k] << "/" << vnc;
                    os << "\n";
                }
            }
        }
    }

    if (opt.exportEdges) {
        bool header = false;
        for (Id e = 0; e < (Id)m.edges.size(); ++e) {
            if (!m.edges[e].alive || !m.edgeFaces(e).empty()) continue;
            bool loose = true;
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive) loose = false;
            if (!loose) continue;
            if (!header) { os << "g loose_edges\n"; header = true; }
            os << "l " << vIndex[m.edges[e].v[0]] << " " << vIndex[m.edges[e].v[1]] << "\n";
        }
    }

    QSaveFile mf(dir + "/" + mtlName);
    if (mf.open(QIODevice::WriteOnly)) {
        mf.write(mtl.toUtf8());
        mf.commit();
    }
    QSaveFile of(path);
    if (!of.open(QIODevice::WriteOnly)) {
        if (error) *error = of.errorString();
        return false;
    }
    of.write(out.toUtf8());
    if (!of.commit()) {
        if (error) *error = of.errorString();
        return false;
    }
    return true;
}

namespace {

// Planar texture mapping reproducing three (position, uv) pairs exactly.
bool fitTexMap(const Vec3& n, const Vec3 p[3], const Vec2 uv[3], TexMap& out) {
    Vec3 e1, e2;
    planeBasis(n, e1, e2);
    Vec3 d1 = p[1] - p[0], d2 = p[2] - p[0];
    double a11 = d1.dot(e1), a12 = d1.dot(e2), a21 = d2.dot(e1), a22 = d2.dot(e2);
    double det = a11 * a22 - a12 * a21;
    if (std::fabs(det) < 1e-14) return false;
    auto solve = [&](double b1, double b2, double& x, double& y) {
        x = (b1 * a22 - a12 * b2) / det;
        y = (a11 * b2 - b1 * a21) / det;
    };
    double ux, uy, vx, vy;
    solve(uv[1].x - uv[0].x, uv[2].x - uv[0].x, ux, uy);
    solve(uv[1].y - uv[0].y, uv[2].y - uv[0].y, vx, vy);
    Vec3 U = e1 * ux + e2 * uy, V = e1 * vx + e2 * vy;
    // Find w in plane with U.w = u0, V.w = v0.
    double det2 = ux * vy - uy * vx;
    if (std::fabs(det2) < 1e-20) return false;
    double al = (uv[0].x * vy - uy * uv[0].y) / det2;
    double be = (ux * uv[0].y - uv[0].x * vx) / det2;
    Vec3 w = e1 * al + e2 * be;
    out.valid = true;
    out.origin = p[0] - w;
    out.u = U;
    out.v = V;
    return true;
}

} // namespace

int importObj(Document& doc, const QString& path, bool yUp, double scale, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return -1;
    }
    QFileInfo fi(path);
    std::vector<Vec3> pos;
    std::vector<Vec2> uvs;
    std::map<QString, Id> matIds;
    Id curMat = kNone;
    Mesh& mesh = doc.mesh();
    int faceCount = 0;

    auto loadMtl = [&](const QString& file) {
        QFile mf(fi.absolutePath() + "/" + file);
        if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        QTextStream ts(&mf);
        Material* cur = nullptr;
        Id curId = kNone;
        while (!ts.atEnd()) {
            QString line = ts.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            QStringList tk = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const QString& k = tk[0];
            if (k == "newmtl" && tk.size() > 1) {
                Material m;
                m.name = line.mid(7).trimmed().toStdString();
                curId = doc.addMaterial(m);
                cur = &doc.state.materials[curId];
                matIds[line.mid(7).trimmed()] = curId;
            } else if (cur && k == "Kd" && tk.size() >= 4) {
                cur->color = Color(tk[1].toFloat(), tk[2].toFloat(), tk[3].toFloat());
            } else if (cur && k == "d" && tk.size() >= 2) {
                cur->opacity = tk[1].toFloat();
            } else if (cur && k == "Tr" && tk.size() >= 2) {
                cur->opacity = 1.0f - tk[1].toFloat();
            } else if (cur && k == "map_Kd" && tk.size() >= 2) {
                QString texFile = tk.last();
                QString full = QDir(fi.absolutePath()).filePath(texFile);
                Texture t;
                if (loadTextureFile(full, t)) {
                    cur->texture = t;
                    cur = &doc.state.materials[curId];
                }
            }
        }
        ++doc.materialsRevision;
    };

    std::vector<Id> vertId; // OBJ index -> mesh vertex (lazy)
    auto meshVertex = [&](int i) -> Id {
        if (i < 0 || i >= (int)pos.size()) return kNone;
        if ((int)vertId.size() < (int)pos.size()) vertId.resize(pos.size(), kNone);
        if (vertId[i] == kNone) vertId[i] = mesh.addVertex(pos[i]);
        return vertId[i];
    };
    auto parseIndex = [](const QString& s, int count) {
        int i = s.toInt();
        if (i < 0) return count + i;
        return i - 1;
    };

    QTextStream ts(&f);
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        QStringList tk = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        const QString& k = tk[0];
        if (k == "v" && tk.size() >= 4) {
            pos.push_back(fromFile(Vec3(tk[1].toDouble(), tk[2].toDouble(), tk[3].toDouble()), yUp, scale));
        } else if (k == "vt" && tk.size() >= 3) {
            uvs.emplace_back(tk[1].toDouble(), tk[2].toDouble());
        } else if (k == "mtllib" && tk.size() >= 2) {
            loadMtl(line.mid(7).trimmed());
        } else if (k == "usemtl" && tk.size() >= 2) {
            auto it = matIds.find(line.mid(7).trimmed());
            curMat = it != matIds.end() ? it->second : kNone;
        } else if (k == "f" && tk.size() >= 4) {
            std::vector<Id> loop;
            std::vector<int> uvIdx;
            for (int i = 1; i < tk.size(); ++i) {
                QStringList parts = tk[i].split('/');
                Id v = meshVertex(parseIndex(parts[0], (int)pos.size()));
                if (v == kNone) continue;
                if (!loop.empty() && loop.back() == v) continue;
                loop.push_back(v);
                uvIdx.push_back(parts.size() > 1 && !parts[1].isEmpty() ? parseIndex(parts[1], (int)uvs.size()) : -1);
            }
            while (loop.size() > 1 && loop.front() == loop.back()) { loop.pop_back(); uvIdx.pop_back(); }
            if (loop.size() < 3) continue;
            Id fid = mesh.addFaceRaw(loop, {});
            if (fid == kNone) continue;
            ++faceCount;
            Face& F = mesh.faces[fid];
            F.front.material = curMat;
            F.back.material = curMat;
            const Material* mat = doc.material(curMat);
            if (mat && mat->hasTexture()) {
                // Pick three well-spread vertices with valid uvs.
                size_t n = loop.size();
                size_t a = 0, b = n / 3, c = (2 * n) / 3;
                if (uvIdx[a] >= 0 && uvIdx[b] >= 0 && uvIdx[c] >= 0 && uvIdx[a] < (int)uvs.size() &&
                    uvIdx[b] < (int)uvs.size() && uvIdx[c] < (int)uvs.size()) {
                    Vec3 p[3] = {mesh.pos(loop[a]), mesh.pos(loop[b]), mesh.pos(loop[c])};
                    Vec2 t[3] = {uvs[uvIdx[a]], uvs[uvIdx[b]], uvs[uvIdx[c]]};
                    TexMap tm;
                    if (fitTexMap(F.normal, p, t, tm)) mesh.faces[fid].front.tex = tm;
                    // The back side needs a mirrored projection basis, but the same mapping works.
                    mesh.faces[fid].back.tex = mesh.faces[fid].front.tex;
                }
            }
        } else if (k == "l" && tk.size() >= 3) {
            for (int i = 1; i + 1 < tk.size(); ++i) {
                Id a = meshVertex(parseIndex(tk[i].split('/')[0], (int)pos.size()));
                Id b = meshVertex(parseIndex(tk[i + 1].split('/')[0], (int)pos.size()));
                if (a != kNone && b != kNone) mesh.addEdgeRaw(a, b);
            }
        }
    }
    // Triangulated meshes: hide the internal diagonals between coplanar triangles.
    for (Id e = 0; e < (Id)mesh.edges.size(); ++e) {
        if (!mesh.edges[e].alive) continue;
        const auto& fs = mesh.edgeFaces(e);
        if (fs.size() != 2) continue;
        const Face &A = mesh.faces[fs[0]], &B = mesh.faces[fs[1]];
        if (A.normal.dot(B.normal) > 0.9999 && A.front.material == B.front.material) {
            mesh.edges[e].soft = true;
            mesh.edges[e].smooth = true;
        } else if (A.normal.dot(B.normal) > 0.95) {
            mesh.edges[e].smooth = true;
            mesh.edges[e].soft = true;
        }
    }
    mesh.touch();
    return faceCount;
}

} // namespace gs
