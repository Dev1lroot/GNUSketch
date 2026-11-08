// SPDX-License-Identifier: GPL-3.0-or-later
#include "ObmFormat.h"

#include "core/Components.h"
#include "io/ImageUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace gs::obm {

using ojson = nlohmann::ordered_json;

namespace {

constexpr double kPx = 1.0 / 16; // a pixel, in meters
constexpr double kDeg = kPi / 180;

// ---- faces: north, south, east, west, up, down -----------------------------------
enum { North, South, East, West, Up, Down };
const char* const kFaceName[6] = {"north", "south", "east", "west", "up", "down"};
const int kWriteOrder[6] = {North, East, South, West, Up, Down}; // (as Blockbench writes them)
const double kFaceDir[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
// Each face's corners, bottom-left, bottom-right, top-right, top-left seen from
// outside (1: the cube's `to` on that axis): as the game has them, blocks and
// entities alike (GNUBlocks registry.cpp CORNER, playermodel.cpp emitBox).
const int kCorner[6][4][3] = {
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}, // north
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, // south
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}, // east
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}, // west
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}, // up
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}, // down
};
int faceIndex(const std::string& n) {
    for (int f = 0; f < 6; ++f)
        if (n == kFaceName[f]) return f;
    return n == "bottom" ? Down : -1;
}

// How a face's picture lies on it. A rect [u0, v0, u1, v1] has the corners
// (u0, v1) (u1, v1) (u1, v0) (u0, v0); face corner c takes rect corner
// (c + turn + shift) % 4, the turn being its "rotation" / 90. An entity model's
// top and bottom are shifted by two more (playermodel.cpp: FLAT), and its
// bottom's rect is written with v0 and v1 the other way round.
struct Convention {
    bool entity = false;
    int shift(int f) const { return entity && (f == Up || f == Down) ? 2 : 0; }
    bool swapsV(int f) const { return entity && f == Down; }
};
void rectCorner(const double r[4], int k, double& u, double& v) {
    static const int pick[4][2] = {{0, 3}, {2, 3}, {2, 1}, {0, 1}};
    u = r[pick[k][0]], v = r[pick[k][1]];
}

// ---- OBM space (pixels, Y up, -Z north) and GNUSketch's (meters, Z up, +Y north) --
Vec3 toGs(const Vec3& o) { return {o.x * kPx, -o.z * kPx, o.y * kPx}; }
Mat4 obmToGs() {
    Mat4 m;
    m.at(0, 0) = kPx, m.at(1, 1) = 0, m.at(1, 2) = -kPx, m.at(2, 2) = 0, m.at(2, 1) = kPx;
    return m;
}
Mat4 gsToObm() {
    Mat4 m;
    m.at(0, 0) = 16, m.at(1, 1) = 0, m.at(1, 2) = 16, m.at(2, 2) = 0, m.at(2, 1) = -16;
    return m;
}

// 3x3 rotations, row-major.
using M3 = std::array<double, 9>;
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return r;
}
Vec3 mul(const M3& a, const Vec3& v) {
    return {a[0] * v.x + a[1] * v.y + a[2] * v.z, a[3] * v.x + a[4] * v.y + a[5] * v.z, a[6] * v.x + a[7] * v.y + a[8] * v.z};
}
Vec3 mulT(const M3& a, const Vec3& v) {
    return {a[0] * v.x + a[3] * v.y + a[6] * v.z, a[1] * v.x + a[4] * v.y + a[7] * v.z, a[2] * v.x + a[5] * v.y + a[8] * v.z};
}
// [x, y, z] degrees, applied x first, then y, then z (Rz Ry Rx), as the game does
M3 euler(const Vec3& d) {
    const double cx = std::cos(d.x * kDeg), sx = std::sin(d.x * kDeg), cy = std::cos(d.y * kDeg), sy = std::sin(d.y * kDeg),
                 cz = std::cos(d.z * kDeg), sz = std::sin(d.z * kDeg);
    const M3 rx{1, 0, 0, 0, cx, -sx, 0, sx, cx}, ry{cy, 0, sy, 0, 1, 0, -sy, 0, cy}, rz{cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    return mul(rz, mul(ry, rx));
}
double snap(double v, double step) {
    double r = std::round(v / step) * step;
    return std::abs(r) < step / 2 ? 0.0 : r;
}
Vec3 eulerOf(const M3& R) {
    const double sb = std::clamp(-R[6], -1.0, 1.0), b = std::asin(sb);
    double a, c;
    if (std::abs(std::cos(b)) > 1e-9) a = std::atan2(R[7], R[8]), c = std::atan2(R[3], R[0]);
    else a = std::atan2(-R[5], R[4]), c = 0;
    return {snap(a / kDeg, 1e-4), snap(b / kDeg, 1e-4), snap(c / kDeg, 1e-4)};
}
Mat4 toMat4(const M3& R, const Vec3& pivot) { // rotation about a pivot
    Mat4 m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.at(i, j) = R[i * 3 + j];
    const Vec3 t = pivot - mul(R, pivot);
    m.at(0, 3) = t.x, m.at(1, 3) = t.y, m.at(2, 3) = t.z;
    return m;
}
bool sameMat(const Mat4& a, const Mat4& b, double tol = 1e-9) {
    for (int i = 0; i < 16; ++i)
        if (std::abs(a.m[i] - b.m[i]) > tol * std::max(1.0, std::abs(a.m[i]))) return false;
    return true;
}

// ---- small JSON helpers --------------------------------------------------------
Vec3 vec3(const ojson& j, const char* key, Vec3 d = {}) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return d;
    auto n = [&](size_t i) { return j[key][i].is_number() ? j[key][i].get<double>() : 0.0; };
    return {n(0), n(1), n(2)};
}
ojson num(double v) { // a whole number as one, else to 1e-4
    v = snap(v, 1e-4);
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) return ojson(int64_t(std::llround(v)));
    return ojson(v);
}
ojson arr3(const Vec3& v) { return ojson::array({num(v.x), num(v.y), num(v.z)}); }
bool hasTag(const ojson& j, const std::string& t) {
    if (!j.contains("tags") || !j["tags"].is_array()) return false;
    for (const ojson& x : j["tags"])
        if (x.is_string() && x.get<std::string>() == t) return true;
    return false;
}
// Where a model's or a group's cubes and groups go back when it is written: null, or
// an empty list if the file had one (so it is written again).
ojson slot(const ojson& v) { return v.is_array() && v.empty() ? ojson::array() : ojson(); }
// Puts a list back in its place: where it was (left out if empty and it had none), else at the end if it has anything.
void putList(ojson& o, const char* key, const ojson& v) {
    if (o.contains(key)) {
        if (v.empty() && o[key].is_null()) o.erase(key);
        else o[key] = v;
    } else if (!v.empty()) {
        o[key] = v;
    }
}
ojson parseExtra(const std::string& text) {
    if (text.empty()) return ojson::object();
    ojson j = ojson::parse(text, nullptr, false);
    return j.is_object() ? j : ojson::object();
}

// ---- a cube's look, as a string: the same geometry and pictures give the same one --
std::string signature(const Document& doc, const Mesh& mesh, const std::vector<Id>& faces) {
    std::vector<std::string> parts;
    auto r = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.6f", snap(v, 1e-7));
        return std::string(b);
    };
    for (Id f : faces) {
        const Face& F = mesh.faces[f];
        std::vector<std::string> pts;
        for (Id v : F.outer) pts.push_back(r(mesh.pos(v).x) + "," + r(mesh.pos(v).y) + "," + r(mesh.pos(v).z));
        std::sort(pts.begin(), pts.end());
        std::string s;
        for (const std::string& p : pts) s += p + ";";
        for (const FaceSide* side : {&F.front, &F.back}) {
            const Material* m = doc.material(side->material);
            s += "|" + (m ? m->name : std::string("-"));
            if (side->tex.valid)
                for (const Vec3& v : {side->tex.origin, side->tex.u, side->tex.v}) s += ":" + r(v.x) + "," + r(v.y) + "," + r(v.z);
        }
        s += F.hidden ? "|h" : "";
        parts.push_back(s);
    }
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (const std::string& p : parts) out += p + "\n";
    return out;
}

// The planar map taking three corners of a rectangle (p0, p1 along one side, p3
// along the other) to their texture coordinates.
TexMap mapFrom(const Vec3& p0, const Vec3& p1, const Vec3& p3, Vec2 t0, Vec2 t1, Vec2 t3) {
    const Vec3 e1 = p1 - p0, e2 = p3 - p0;
    const double l1 = e1.dot(e1), l2 = e2.dot(e2);
    TexMap m;
    if (l1 < 1e-18 || l2 < 1e-18) return m;
    double dU1 = t1.x - t0.x, dU2 = t3.x - t0.x, dV1 = t1.y - t0.y, dV2 = t3.y - t0.y;
    double det = dU1 * dV2 - dU2 * dV1;
    if (std::abs(det) < 1e-12) { // (a picture squeezed to a line or a point: as near as a plane map can say)
        if (std::abs(dU1) + std::abs(dU2) < 1e-12) dU1 += 1e-6;
        if (std::abs(dV1) + std::abs(dV2) < 1e-12) dV2 += 1e-6;
        det = dU1 * dV2 - dU2 * dV1;
        if (std::abs(det) < 1e-12) dU1 += 1e-6, dV2 += 1e-6, det = dU1 * dV2 - dU2 * dV1;
    }
    m.u = e1 * (dU1 / l1) + e2 * (dU2 / l2);
    m.v = e1 * (dV1 / l1) + e2 * (dV2 / l2);
    // origin = p0 - (s e1 + t e2), with (p0 - origin) . u = U0 and . v = V0
    const double s = (t0.x * dV2 - dU2 * t0.y) / det, t = (dU1 * t0.y - t0.x * dV1) / det;
    m.origin = p0 - (e1 * s + e2 * t);
    m.valid = true;
    return m;
}

// ---- reading ---------------------------------------------------------------------
struct Reader {
    Document& doc;
    QStringList* msgs;
    Convention conv;
    double texW = 16, texH = 16;
    std::vector<Id> textures; // material of each texture index
    int missingPictures = 0;
    bool tinted = false;

    struct Turn { int axis = 1; double deg = 0, scale = 1; Vec3 origin; };
    static void turn(Vec3& p, const Turn& t) {
        double c[3] = {p.x, p.y, p.z};
        const double o[3] = {t.origin.x, t.origin.y, t.origin.z};
        const int u = (t.axis + 1) % 3, v = (t.axis + 2) % 3;
        const double cs = std::cos(t.deg * kDeg), sn = std::sin(t.deg * kDeg);
        const double a = c[u] - o[u], b = c[v] - o[v];
        c[u] = o[u] + (a * cs - b * sn) * t.scale, c[v] = o[v] + (a * sn + b * cs) * t.scale;
        p = {c[0], c[1], c[2]};
    }

    Id material(const ojson& tex, const QString& folder) {
        const std::string path = tex.value("path", "");
        if (path.empty()) return kNone;
        for (Id m = 0; m < (Id)doc.state.materials.size(); ++m) // (one of each picture in the model)
            if (doc.state.materials[m].name == path) return m;
        Material mat;
        mat.name = path;
        const std::string base = path.substr(0, path.find('@'));
        tinted |= base.size() != path.size();
        if (!folder.isEmpty() && loadTextureFile(folder + "/" + QString::fromStdString(base) + ".png", mat.texture)) {
            mat.color = averageColor(decodeTexture(mat.texture));
        } else {
            ++missingPictures;
            mat.color = Color::rgb8(200, 60, 200); // (a picture not found: magenta, as games show one)
        }
        Id id = doc.addMaterial(mat);
        doc.state.materials[id].name = path; // (its path is its name: how it is found again when written)
        return id;
    }

    // The faces of one cube, into mesh; returns their ids.
    std::vector<Id> cube(Mesh& mesh, const ojson& c, const Vec3& groupPivot) {
        Vec3 from = vec3(c, "from"), to = vec3(c, "to");
        Vec3 lo{std::min(from.x, to.x), std::min(from.y, to.y), std::min(from.z, to.z)};
        Vec3 hi{std::max(from.x, to.x), std::max(from.y, to.y), std::max(from.z, to.z)};
        const Vec3 size = hi - lo; // (the box UV's measure: before inflation)
        const double inflate = c.value("inflate", 0.0);
        lo = lo - Vec3(inflate, inflate, inflate), hi = hi + Vec3(inflate, inflate, inflate);
        std::vector<Turn> turns;
        const ojson props = c.value("props", ojson::object());
        if (props.contains("turns") && props["turns"].is_array()) {
            for (const ojson& r : props["turns"]) {
                Turn t;
                const std::string ax = r.value("axis", "y");
                t.axis = ax == "x" ? 0 : ax == "z" ? 2 : 1;
                t.deg = r.value("angle", 0.0);
                t.origin = vec3(r, "origin", {8, 8, 8});
                if (r.value("rescale", false) && std::abs(std::cos(t.deg * kDeg)) > 1e-3) t.scale = 1 / std::abs(std::cos(t.deg * kDeg));
                turns.push_back(t);
            }
        } else {
            const Vec3 rot = vec3(c, "rotation"), piv = vec3(c, "pivot", conv.entity ? groupPivot : Vec3());
            const double d[3] = {rot.x, rot.y, rot.z};
            for (int a = 0; a < 3; ++a)
                if (d[a] != 0) {
                    Turn t;
                    t.axis = a, t.deg = d[a], t.origin = piv;
                    if (props.value("rescale", false) && std::abs(std::cos(d[a] * kDeg)) > 1e-3) t.scale = 1 / std::abs(std::cos(d[a] * kDeg));
                    turns.push_back(t);
                }
        }
        // the rect of each face: as written, or the box UV's (an entity's), or from where it lies (a block's)
        const ojson faces = c.value("faces", ojson::object());
        const bool box = c.contains("box_uv") && c["box_uv"].is_array() && c["box_uv"].size() >= 2;
        const bool useBox = box && (!c.value("mirror", false) || faces.empty());
        double rect[6][4];
        bool present[6] = {};
        int turnOf[6] = {}, texOf[6] = {};
        if (useBox) {
            const double u = c["box_uv"][0].get<double>(), v = c["box_uv"][1].get<double>(), w = size.x, h = size.y, d = size.z;
            const double b[6][4] = {{u + d, v + d, u + d + w, v + d + h}, {u + 2 * d + w, v + d, u + 2 * d + 2 * w, v + d + h},
                                    {u, v + d, u + d, v + d + h},         {u + d + w, v + d, u + 2 * d + w, v + d + h},
                                    {u + d, v, u + d + w, v + d},         {u + d + w, v, u + d + 2 * w, v + d}};
            for (int f = 0; f < 6; ++f) {
                std::copy(b[f], b[f] + 4, rect[f]);
                present[f] = faces.empty() || faces.contains(kFaceName[f]);
                if (faces.contains(kFaceName[f])) texOf[f] = faces[kFaceName[f]].value("texture", 0);
            }
        } else {
            for (auto it = faces.begin(); it != faces.end(); ++it) {
                const int f = faceIndex(it.key());
                if (f < 0 || !it.value().is_object()) continue;
                const ojson& fj = it.value();
                present[f] = true;
                texOf[f] = fj.contains("texture") && fj["texture"].is_number() ? fj["texture"].get<int>() : -1;
                turnOf[f] = ((fj.value("rotation", 0) / 90) % 4 + 4) % 4;
                if (fj.contains("uv") && fj["uv"].is_array() && fj["uv"].size() == 4) {
                    for (int k = 0; k < 4; ++k) rect[f][k] = fj["uv"][size_t(k)].get<double>();
                    if (conv.swapsV(f)) std::swap(rect[f][1], rect[f][3]);
                } else { // (a block's face without uv: where it lies in the 16 x 16 cell)
                    const double x0 = lo.x, y0 = lo.y, z0 = lo.z, x1 = hi.x, y1 = hi.y, z1 = hi.z;
                    const double a[6][4] = {{16 - x1, 16 - y1, 16 - x0, 16 - y0}, {x0, 16 - y1, x1, 16 - y0}, {16 - z1, 16 - y1, 16 - z0, 16 - y0},
                                            {z0, 16 - y1, z1, 16 - y0},          {x0, z0, x1, z1},          {x0, 16 - z1, x1, 16 - z0}};
                    for (int k = 0; k < 4; ++k) rect[f][k] = a[f][k] * (k % 2 ? texH : texW) / 16;
                }
            }
        }
        // its corners (each corner one vertex, the cube's own: cubes that touch stay apart)
        const double flatTol = 1e-9;
        const bool flat[3] = {hi.x - lo.x < flatTol, hi.y - lo.y < flatTol, hi.z - lo.z < flatTol};
        if (int(flat[0]) + int(flat[1]) + int(flat[2]) > 1) return {}; // (a line: nothing to draw)
        Id corner[8];
        for (int k = 0; k < 8; ++k) {
            const int bx = flat[0] ? 0 : (k & 1), by = flat[1] ? 0 : (k >> 1 & 1), bz = flat[2] ? 0 : (k >> 2 & 1);
            const int kk = bx | by << 1 | bz << 2;
            if (kk != k) {
                corner[k] = corner[kk];
                continue;
            }
            Vec3 p{bx ? hi.x : lo.x, by ? hi.y : lo.y, bz ? hi.z : lo.z};
            for (const Turn& t : turns) turn(p, t);
            corner[k] = mesh.addVertexRaw(toGs(p));
        }
        const bool hidden = hasTag(c, "hidden");
        std::vector<Id> made;
        std::map<std::set<Id>, Id> byVerts; // (a flat cube's two sides: one face, front and back)
        for (int f : kWriteOrder) {
            if (!present[f]) continue;
            Id vid[4];
            Vec3 pobm[4];
            for (int k = 0; k < 4; ++k) {
                const int* b = kCorner[f][k];
                vid[k] = corner[(b[0] ? 1 : 0) | (b[1] ? 2 : 0) | (b[2] ? 4 : 0)];
            }
            const std::set<Id> key(vid, vid + 4);
            if (key.size() < 4) continue; // (a side of a flat cube with no width)
            for (int k = 0; k < 4; ++k) pobm[k] = mesh.pos(vid[k]);
            // where its picture lies
            Vec2 t[4];
            for (int k = 0; k < 4; ++k) {
                double u, v;
                rectCorner(rect[f], (k + turnOf[f] + conv.shift(f)) % 4, u, v);
                t[k] = Vec2(u / texW, 1 - v / texH);
            }
            const TexMap tm = mapFrom(pobm[0], pobm[1], pobm[3], t[0], t[1], t[3]);
            const Id mat = texOf[f] >= 0 && texOf[f] < (int)textures.size() ? textures[size_t(texOf[f])] : kNone;
            auto it = byVerts.find(key);
            if (it != byVerts.end()) { // the other side of a flat one
                Face& F = mesh.faces[it->second];
                F.back.material = mat, F.back.tex = mat != kNone ? tm : TexMap{};
                continue;
            }
            for (int k = 0; k < 4; ++k) mesh.addEdgeRaw(vid[k], vid[(k + 1) % 4]);
            Vec3 outward = (pobm[1] - pobm[0]).cross(pobm[3] - pobm[0]);
            const Id face = mesh.addFaceRaw({vid[0], vid[1], vid[2], vid[3]}, {}, outward);
            if (face == kNone) continue;
            Face& F = mesh.faces[face];
            F.front.material = mat, F.front.tex = mat != kNone ? tm : TexMap{};
            F.hidden = hidden;
            byVerts[key] = face;
            made.push_back(face);
        }
        // (faces of one cube that don't touch, a pane's two sides: tied by a hidden edge, so they are still one cube)
        for (size_t i = 1; i < made.size(); ++i) {
            bool touches = false;
            for (size_t j = 0; j < i && !touches; ++j)
                for (Id a : mesh.faces[made[i]].outer)
                    for (Id b : mesh.faces[made[j]].outer) touches |= a == b;
            if (!touches) {
                const Id e = mesh.addEdgeRaw(mesh.faces[made[0]].outer[0], mesh.faces[made[i]].outer[0]);
                if (e != kNone) mesh.edges[e].hidden = true;
            }
        }
        return made;
    }
};

// ---- writing: a box (or a lone rectangle) found in a mesh -------------------------
struct FoundBox {
    std::vector<Id> faces;
    Vec3 center;     // OBM
    Vec3 axis[3];    // OBM, unit
    double half[3];  // along them
    bool flat = false;
};

// The sets of faces joined by shared corners; those that make a box (or one rectangle).
std::vector<FoundBox> findBoxes(const Mesh& mesh, const Mat4& bake, int& notBoxes) {
    std::vector<Id> alive;
    for (Id f = 0; f < (Id)mesh.faces.size(); ++f)
        if (mesh.faces[f].alive) alive.push_back(f);
    std::vector<Id> parent(mesh.vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<Id(Id)> root = [&](Id v) { return parent[v] == v ? v : parent[v] = root(parent[v]); };
    for (Id f : alive) {
        const Face& F = mesh.faces[f];
        for (Id v : F.outer) parent[root(v)] = root(F.outer[0]);
        for (const auto& h : F.holes)
            for (Id v : h) parent[root(v)] = root(F.outer[0]);
    }
    for (const Edge& e : mesh.edges) // (a hidden edge ties the faces of one cube that don't touch)
        if (e.alive && e.hidden && e.v[0] >= 0 && e.v[1] >= 0) parent[root(e.v[0])] = root(e.v[1]);
    std::map<Id, std::vector<Id>> sets;
    for (Id f : alive) sets[root(mesh.faces[f].outer[0])].push_back(f);
    std::vector<FoundBox> out;
    const double tol = 1e-4; // pixels
    for (auto& [r, faces] : sets) {
        bool ok = faces.size() <= 6;
        for (Id f : faces) ok &= mesh.faces[f].holes.empty() && mesh.faces[f].outer.size() == 4;
        if (!ok) {
            ++notBoxes;
            continue;
        }
        auto P = [&](Id v) { return bake.transformPoint(mesh.pos(v)); };
        const Face& F0 = mesh.faces[faces[0]];
        const Vec3 p0 = P(F0.outer[0]), e1 = P(F0.outer[1]) - p0, e2 = P(F0.outer[3]) - p0;
        if (e1.length() < tol || e2.length() < tol || std::abs(e1.normalized().dot(e2.normalized())) > 1e-6) {
            ++notBoxes;
            continue;
        }
        FoundBox b;
        b.faces = faces;
        b.axis[0] = e1.normalized(), b.axis[1] = e2.normalized(), b.axis[2] = b.axis[0].cross(b.axis[1]).normalized();
        double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
        std::set<Id> verts;
        for (Id f : faces)
            for (Id v : mesh.faces[f].outer) verts.insert(v);
        for (Id v : verts) {
            const Vec3 d = P(v) - p0;
            for (int a = 0; a < 3; ++a) mn[a] = std::min(mn[a], d.dot(b.axis[a])), mx[a] = std::max(mx[a], d.dot(b.axis[a]));
        }
        for (Id v : verts) { // every corner at a corner of the box
            const Vec3 d = P(v) - p0;
            for (int a = 0; a < 3; ++a) {
                const double c = d.dot(b.axis[a]);
                ok &= std::abs(c - mn[a]) < tol || std::abs(c - mx[a]) < tol;
            }
        }
        b.flat = mx[2] - mn[2] < tol;
        if (b.flat && faces.size() != 1) ok = false;
        for (Id f : faces) { // every face a whole side of it
            const Face& F = mesh.faces[f];
            int sideAxis = -1;
            for (int a = 0; a < 3 && sideAxis < 0; ++a) {
                bool all = true;
                const double first = (P(F.outer[0]) - p0).dot(b.axis[a]);
                for (Id v : F.outer) all &= std::abs((P(v) - p0).dot(b.axis[a]) - first) < tol;
                if (all && (std::abs(first - mn[a]) < tol || std::abs(first - mx[a]) < tol)) sideAxis = a;
            }
            ok &= sideAxis >= 0;
        }
        if (!ok) {
            ++notBoxes;
            continue;
        }
        Vec3 c = p0;
        for (int a = 0; a < 3; ++a) c = c + b.axis[a] * ((mn[a] + mx[a]) / 2), b.half[a] = (mx[a] - mn[a]) / 2;
        b.center = c;
        out.push_back(b);
    }
    return out;
}

struct Writer {
    const Document& doc;
    QStringList* msgs;
    Convention conv;
    double texW = 16, texH = 16;
    ojson textures = ojson::array();
    std::map<std::string, int> textureOf; // material name -> index
    int noPicture = 0, notBoxes = 0;
    std::set<std::string> newPaths;
    QString folder;
    const Mat4 base = gsToObm();

    int textureIndex(const Material* m) {
        if (!m) {
            ++noPicture;
            return -1;
        }
        auto it = textureOf.find(m->name); // (one of the model's own: kept, its picture found or not)
        if (it != textureOf.end()) return it->second;
        if (!m->hasTexture()) {
            ++noPicture;
            return -1;
        }
        std::string path = m->name; // (a picture of our own: its name is its path)
        for (char& ch : path) ch = ch == ' ' ? '_' : char(std::tolower(static_cast<unsigned char>(ch)));
        const std::string base = path.substr(0, path.find('@'));
        if (folder.isEmpty() || !QFile::exists(folder + "/" + QString::fromStdString(base) + ".png")) newPaths.insert(path);
        textures.push_back({{"name", path.substr(path.rfind('/') + 1)}, {"path", path}});
        const int idx = int(textures.size()) - 1;
        textureOf[m->name] = idx;
        return idx;
    }

    // One box as a cube: `rec` the cube it was read as, if it is still that one in all but its place.
    ojson cube(const Mesh& mesh, const FoundBox& b, const Mat4& bake, const ojson* rec, int n) {
        // its axes: the box's, matched to x, y, z as closely as they go (the least turn)
        const Vec3 world[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        int assign[3] = {-1, -1, -1};
        bool used[3] = {};
        double sign[3] = {1, 1, 1}, how[3] = {};
        for (int round = 0; round < 3; ++round) {
            double best = -1;
            int bi = 0, bj = 0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if (assign[i] < 0 && !used[j] && std::abs(world[i].dot(b.axis[j])) > best) best = std::abs(world[i].dot(b.axis[j])), bi = i, bj = j;
            assign[bi] = bj, used[bj] = true, how[bi] = best, sign[bi] = world[bi].dot(b.axis[bj]) < 0 ? -1 : 1;
        }
        M3 R;
        for (int i = 0; i < 3; ++i) {
            const Vec3 col = b.axis[assign[i]] * sign[i];
            R[0 * 3 + i] = col.x, R[1 * 3 + i] = col.y, R[2 * 3 + i] = col.z;
        }
        const double det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
        if (det < 0) { // (a box is the same box with one axis the other way: the least sure one)
            int k = int(std::min_element(how, how + 3) - how);
            for (int r = 0; r < 3; ++r) R[r * 3 + k] = -R[r * 3 + k];
        }
        Vec3 rot = eulerOf(R);
        R = euler(rot); // (as the file will say it: snapped)
        const bool turned = rot.x != 0 || rot.y != 0 || rot.z != 0;
        Vec3 pivot = b.center;
        if (rec && rec->contains("pivot") && turned && (vec3(*rec, "rotation") - rot).length() < 1e-3) pivot = vec3(*rec, "pivot");
        const Vec3 lc = pivot + mulT(R, b.center - pivot);
        double half[3];
        for (int i = 0; i < 3; ++i) half[i] = b.half[assign[i]];
        const Vec3 lo = lc - Vec3(half[0], half[1], half[2]), hi = lc + Vec3(half[0], half[1], half[2]);

        ojson c = ojson::object();
        c["name"] = rec ? rec->value("name", "cube_" + std::to_string(n)) : "cube_" + std::to_string(n);
        c["from"] = arr3(lo), c["to"] = arr3(hi);
        if (turned || (rec && rec->contains("pivot"))) c["pivot"] = arr3(pivot);
        if (turned) c["rotation"] = arr3(rot);
        // its faces: which side each is, and how its picture lies on it
        const Mat4 unbake = bake.inverted();
        ojson faces = ojson::object();
        std::map<int, ojson> byFace;
        const ojson recFaces = rec ? rec->value("faces", ojson::object()) : ojson::object();
        for (Id fid : b.faces) {
            const Face& F = mesh.faces[fid];
            auto P = [&](Id v) { return bake.transformPoint(mesh.pos(v)); };
            const Vec3 n = (P(F.outer[1]) - P(F.outer[0])).cross(P(F.outer[3]) - P(F.outer[0])).normalized();
            for (int side = 0; side < (b.flat ? 2 : 1); ++side) {
                const FaceSide& S = side == 0 ? F.front : F.back;
                if (side == 1 && S.material == kNone) continue;
                const Vec3 ln = mulT(R, side == 0 ? n : -n);
                int f = 0;
                double best = -2;
                for (int k = 0; k < 6; ++k) {
                    const double d = ln.dot(Vec3(kFaceDir[k][0], kFaceDir[k][1], kFaceDir[k][2]));
                    if (d > best) best = d, f = k;
                }
                const Material* mat = doc.material(S.material);
                const int tex = textureIndex(mat);
                ojson fj = ojson::object();
                if (tex >= 0) {
                    // the picture's place at each corner of the face, in the texture's pixels
                    double fu[4], fv[4];
                    for (int k = 0; k < 4; ++k) {
                        const int* cb = kCorner[f][k];
                        const Vec3 local{cb[0] ? hi.x : lo.x, cb[1] ? hi.y : lo.y, cb[2] ? hi.z : lo.z};
                        const Vec3 p = unbake.transformPoint(pivot + mul(R, local - pivot));
                        const Vec2 t = faceTexCoord(F, side == 0, p, mat);
                        fu[k] = t.x * texW, fv[k] = (1 - t.y) * texH;
                    }
                    // the rect and turn that put them there (a picture skewed on its face: the nearest)
                    double rect[4] = {fu[0], fv[2], fu[2], fv[0]};
                    int turn = 0;
                    double bestErr = 1e300;
                    for (int t = 0; t < 4; ++t) {
                        const int s = (t + conv.shift(f)) % 4;
                        // rect corner k sits at face corner (k - s) mod 4
                        auto at = [&](int k, double& u, double& v) { const int c = ((k - s) % 4 + 4) % 4; u = fu[c], v = fv[c]; };
                        double u0, v1, u1, v1b, u1b, v0, u0b, v0b;
                        at(0, u0, v1), at(1, u1, v1b), at(2, u1b, v0), at(3, u0b, v0b);
                        const double err = std::abs(v1 - v1b) + std::abs(u1 - u1b) + std::abs(v0 - v0b) + std::abs(u0 - u0b);
                        if (err < bestErr - 1e-9) bestErr = err, turn = t, rect[0] = u0, rect[1] = v0, rect[2] = u1, rect[3] = v1;
                    }
                    if (conv.swapsV(f)) std::swap(rect[1], rect[3]);
                    fj["uv"] = ojson::array({num(rect[0]), num(rect[1]), num(rect[2]), num(rect[3])});
                    if (turn) fj["rotation"] = turn * 90;
                    fj["texture"] = tex;
                }
                // what else it had (its cull, tint, props): kept
                if (recFaces.contains(kFaceName[f]) && recFaces[kFaceName[f]].is_object())
                    for (auto it = recFaces[kFaceName[f]].begin(); it != recFaces[kFaceName[f]].end(); ++it)
                        if (it.key() != "uv" && it.key() != "rotation" && it.key() != "texture") fj[it.key()] = it.value();
                byFace[f] = fj;
            }
        }
        for (int f : kWriteOrder)
            if (byFace.count(f)) faces[kFaceName[f]] = byFace[f];
        c["faces"] = faces;
        bool hidden = false;
        for (Id fid : b.faces) hidden |= mesh.faces[fid].hidden;
        if (rec) // (its tags, props and anything else; what is worked out here is not taken from it)
            for (auto it = rec->begin(); it != rec->end(); ++it)
                if (!c.contains(it.key()) && it.key() != "box_uv" && it.key() != "mirror" && it.key() != "inflate" && it.key() != "rotation" &&
                    it.key() != "pivot")
                    c[it.key()] = it.value();
        ojson tags = c.value("tags", ojson::array());
        if (hidden != hasTag(c, "hidden")) {
            if (hidden) tags.push_back("hidden");
            else tags.erase(std::remove(tags.begin(), tags.end(), ojson("hidden")), tags.end());
            c["tags"] = tags;
        }
        return c;
    }

    // The cubes of a mesh, as they go into the file: unchanged ones as they were read.
    ojson cubes(const Mesh& mesh, const Mat4& bake, const ojson& records) {
        ojson out = ojson::array();
        const bool asRead = sameMat(bake, base);
        std::vector<bool> taken(records.size(), false);
        const std::vector<FoundBox> boxes = findBoxes(mesh, bake, notBoxes);
        int n = 0;
        for (const FoundBox& b : boxes) {
            const std::string sig = signature(doc, mesh, b.faces);
            const ojson* rec = nullptr;
            if (asRead)
                for (size_t i = 0; i < records.size() && !rec; ++i)
                    if (!taken[i] && records[i].value("sig", "") == sig) rec = &records[i]["json"], taken[i] = true;
            if (rec) {
                out.push_back(*rec);
                continue;
            }
            // (changed: the cube it was, if one was read near where it is, for its name and tags)
            double bestD = 4;
            int bestI = -1;
            for (size_t i = 0; i < records.size(); ++i) {
                if (taken[i]) continue;
                const double d = (vec3(records[i], "center") - b.center).length();
                if (d < bestD) bestD = d, bestI = int(i);
            }
            if (bestI >= 0) taken[size_t(bestI)] = true;
            out.push_back(cube(mesh, b, bake, bestI >= 0 ? &records[size_t(bestI)]["json"] : nullptr, n++));
        }
        return out;
    }
};

// Written as GNUBlocks' tools write it (Python's json.dump): its layout, and each number as Python prints it, the
// shortest that reads back the same (nlohmann's own printing doesn't always find the shortest: 10.516540000000001).
std::string pyFloat(double v) {
    if (!std::isfinite(v)) return v != v ? "NaN" : v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific); // (shortest digits: d.ddde±xx)
    std::string sci(buf, res.ptr);
    const size_t e = sci.find('e');
    const int exp = std::stoi(sci.substr(e + 1));
    std::string digits;
    bool neg = false;
    for (char c : sci.substr(0, e))
        if (c == '-') neg = true;
        else if (c != '.') digits += c;
    std::string out = neg ? "-" : "";
    if (exp >= -5 && exp < 16) { // (fixed, as repr: 0.0001, 1e-05 switches at the fifth place)
        if (exp >= 0) {
            if (int(digits.size()) <= exp + 1) out += digits + std::string(size_t(exp + 1 - int(digits.size())), '0') + ".0";
            else out += digits.substr(0, size_t(exp + 1)) + "." + digits.substr(size_t(exp + 1));
        } else {
            out += "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
        if (exp == -5) { // (repr: 1e-05, not 0.00001)
            out = neg ? "-" : "";
            out += digits.substr(0, 1) + (digits.size() > 1 ? "." + digits.substr(1) : "") + "e-05";
        }
        return out;
    }
    out += digits.substr(0, 1) + (digits.size() > 1 ? "." + digits.substr(1) : "");
    char eb[16];
    std::snprintf(eb, sizeof eb, "e%c%02d", exp < 0 ? '-' : '+', std::abs(exp));
    return out + eb;
}
void dumpPy(std::string& out, const ojson& j, int indent, int depth) {
    const std::string pad(size_t(indent * (depth + 1)), ' '), end(size_t(indent * depth), ' ');
    switch (j.type()) {
    case ojson::value_t::object: {
        if (j.empty()) { out += "{}"; return; }
        out += "{\n";
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!first) out += ",\n";
            first = false;
            out += pad + ojson(it.key()).dump(-1, ' ', true) + ": ";
            dumpPy(out, it.value(), indent, depth + 1);
        }
        out += "\n" + end + "}";
        return;
    }
    case ojson::value_t::array: {
        if (j.empty()) { out += "[]"; return; }
        out += "[\n";
        for (size_t i = 0; i < j.size(); ++i) {
            if (i) out += ",\n";
            out += pad;
            dumpPy(out, j[i], indent, depth + 1);
        }
        out += "\n" + end + "]";
        return;
    }
    case ojson::value_t::number_float: out += pyFloat(j.get<double>()); return;
    default: out += j.dump(-1, ' ', true); return;
    }
}

std::string readFile(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return {};
    }
    const QByteArray b = f.readAll();
    return std::string(b.constData(), size_t(b.size()));
}

} // namespace

QString texturesFolder(const QString& modelPath) {
    QDir d = QFileInfo(modelPath).absoluteDir();
    for (int depth = 0; depth < 12; ++depth) { // (…/assets/gnublocks/models/block/x.obm: …/assets/gnublocks/textures)
        if (d.dirName() == "models" && QDir(d.absoluteFilePath("../textures")).exists()) return QDir::cleanPath(d.absoluteFilePath("../textures"));
        if (!d.cdUp()) break;
    }
    const QString env = qEnvironmentVariable("GNUSKETCH_OBM_TEXTURES");
    if (!env.isEmpty() && QDir(env).exists()) return env;
    const QString set = QSettings().value("obm/texturesDir").toString();
    return !set.isEmpty() && QDir(set).exists() ? set : QString();
}

bool importObm(Document& doc, const QString& path, QStringList* msgs, std::vector<Id>* missing) {
    QString err;
    const std::string text = readFile(path, &err);
    if (text.empty()) {
        if (msgs) msgs->append(err.isEmpty() ? QStringLiteral("The file is empty") : err);
        return false;
    }
    ojson m = ojson::parse(text, nullptr, false);
    if (!m.is_object() || m.value("format", "") != "obm") {
        if (msgs) msgs->append(QStringLiteral("Not an Open Block Model (no \"format\": \"obm\")"));
        return false;
    }
    Reader rd{doc, msgs, {}, 16, 16, {}, 0, false};
    rd.conv.entity = m.value("kind", "") == "entity" || m.value("kind", "") == "item";   // (an item's box model: drawn as an entity's is)
    if (m.contains("texture_size") && m["texture_size"].is_array() && m["texture_size"].size() >= 2)
        rd.texW = std::max(1.0, m["texture_size"][0].get<double>()), rd.texH = std::max(1.0, m["texture_size"][1].get<double>());
    const QString folder = texturesFolder(path);
    for (const ojson& t : m.value("textures", ojson::array())) {
        const size_t before = doc.state.materials.size();
        const Id id = rd.material(t, folder);
        rd.textures.push_back(id);
        if (missing && id != kNone && doc.state.materials.size() > before && !doc.state.materials[id].hasTexture()) missing->push_back(id);
    }

    ModelState& st = doc.state;
    const Id into = st.activeDef(); // (inside what is open, if a component is)
    // a group: a component; what it has of its own kept with it
    std::function<void(const ojson&, Id, const Vec3&)> group = [&](const ojson& g, Id parentDef, const Vec3& outerPivot) {
        ComponentDef def;
        const std::string name = g.value("name", "group");
        def.name = uniqueComponentName(st, name);
        def.uid = newComponentUid();
        const Vec3 pivot = vec3(g, "pivot", outerPivot), rot = vec3(g, "rotation");
        st.defs.push_back(def);
        const Id d = (Id)st.defs.size() - 1;
        Instance in;
        in.def = d, in.parent = parentDef;
        in.xf = obmToGs() * toMat4(euler(rot), pivot) * gsToObm();
        in.hidden = hasTag(g, "hidden");
        st.instances.push_back(in);
        ojson records = ojson::array();
        for (const ojson& c : g.value("cubes", ojson::array())) {
            if (!c.is_object()) continue;
            const std::vector<Id> faces = rd.cube(st.defs[d].mesh, c, pivot);
            if (faces.empty()) continue;
            Vec3 ctr = (vec3(c, "from") + vec3(c, "to")) * 0.5;
            records.push_back({{"sig", signature(doc, st.defs[d].mesh, faces)}, {"center", arr3(ctr)}, {"json", c}});
        }
        ojson kept = ojson::object(); // (the group as it was, its cubes and groups to be filled in when written)
        for (auto it = g.begin(); it != g.end(); ++it) kept[it.key()] = it.key() == "cubes" || it.key() == "groups" ? slot(it.value()) : it.value();
        ojson extra = ojson::object();
        extra["obm"] = {{"group", kept}, {"name", st.defs[d].name}, {"xf", std::vector<double>(in.xf.m, in.xf.m + 16)}, {"cubes", records}};
        st.defs[d].extra = extra.dump();
        st.defs[d].mesh.touch();
        for (const ojson& child : g.value("groups", ojson::array()))
            if (child.is_object()) group(child, d, pivot);
    };
    Mesh& top = doc.mesh();
    ojson topRecords = ojson::array();
    for (const ojson& c : m.value("cubes", ojson::array())) {
        if (!c.is_object()) continue;
        const std::vector<Id> faces = rd.cube(top, c, {});
        if (faces.empty()) continue;
        topRecords.push_back({{"sig", signature(doc, top, faces)}, {"center", arr3((vec3(c, "from") + vec3(c, "to")) * 0.5)}, {"json", c}});
    }
    top.touch();
    for (const ojson& g : m.value("groups", ojson::array()))
        if (g.is_object()) group(g, into, {});

    // the model as it was, its groups and cubes to be filled in when written; and how the file was laid out
    ojson kept = ojson::object();
    for (auto it = m.begin(); it != m.end(); ++it) kept[it.key()] = it.key() == "cubes" || it.key() == "groups" ? slot(it.value()) : it.value();
    size_t indent = 0;
    if (const size_t nl = text.find('\n'); nl != std::string::npos)
        while (nl + 1 + indent < text.size() && text[nl + 1 + indent] == ' ') ++indent;
    ojson extra = parseExtra(st.extra);
    extra["obm"] = {{"model", kept}, {"cubes", topRecords}, {"indent", indent}, {"newline", !text.empty() && text.back() == '\n'},
                    {"file", QFileInfo(path).absoluteFilePath().toStdString()}};
    st.extra = extra.dump();
    ++doc.materialsRevision;

    if (msgs && !missing) {
        if (folder.isEmpty() && !rd.textures.empty())
            msgs->append(QStringLiteral("The game's textures folder wasn't found beside the model's \"models\" folder: its pictures are "
                                        "shown magenta. Set GNUSKETCH_OBM_TEXTURES to the textures folder (or obm/texturesDir in the settings)."));
        else if (rd.missingPictures)
            msgs->append(QStringLiteral("%1 of its pictures weren't in %2: shown magenta.").arg(rd.missingPictures).arg(folder));
    }
    if (msgs) {
        if (rd.tinted) msgs->append(QStringLiteral("Pictures drawn in a material's colours (\"…@iron\") are shown grey, as they are in the file."));
    }
    return true;
}

bool exportObm(const Document& doc, const QString& path, QStringList* msgs) {
    const ModelState st = closedState(doc.state);
    const ojson extra = parseExtra(st.extra);
    const ojson info = extra.contains("obm") ? extra["obm"] : ojson::object();
    ojson model = info.value("model", ojson::object());
    if (model.empty()) // (a model made here)
        model = {{"format", "obm"}, {"version", 1}, {"kind", "other"}, {"anchor", "corner"}, {"texture_size", ojson::array({16, 16})},
                 {"textures", ojson::array()}, {"groups", ojson()}, {"cubes", ojson()}};
    Writer w{doc, msgs, {}, 16, 16, ojson::array(), {}, 0, 0, {}, {}};
    w.conv.entity = model.value("kind", "") == "entity" || model.value("kind", "") == "item";
    w.folder = texturesFolder(path);
    if (model.contains("texture_size") && model["texture_size"].is_array() && model["texture_size"].size() >= 2)
        w.texW = std::max(1.0, model["texture_size"][0].get<double>()), w.texH = std::max(1.0, model["texture_size"][1].get<double>());
    w.textures = model.value("textures", ojson::array());
    for (size_t i = 0; i < w.textures.size(); ++i)
        if (const std::string p = w.textures[i].value("path", ""); !p.empty()) w.textureOf.emplace(p, int(i));
    if (!info.contains("model")) // (a model made here: its UVs measured in its first picture's pixels, worked out before any cube)
        for (const Material& m : st.materials)
            if (m.hasTexture() && m.texture.pixelWidth > 0 && m.texture.pixelHeight > 0) {
                w.texW = m.texture.pixelWidth, w.texH = m.texture.pixelHeight;
                model["texture_size"] = ojson::array({m.texture.pixelWidth, m.texture.pixelHeight});
                break;
            }

    // the groups: each instance of a component, those inside it within it
    std::map<Id, std::vector<Id>> kids;
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        const Instance& in = st.instances[i];
        if (in.alive && in.def >= 0 && in.def < (Id)st.defs.size()) kids[in.parent].push_back(i);
    }
    std::map<Id, int> placed; // (each component's instances so far: its copies made here get a number)
    std::function<ojson(Id, const Mat4&, int)> groupOf = [&](Id inst, const Mat4& parentBake, int depth) -> ojson {
        const Instance& in = st.instances[inst];
        const ComponentDef& def = st.defs[in.def];
        const ojson dx = parseExtra(def.extra);
        const ojson gi = dx.contains("obm") ? dx["obm"] : ojson::object();
        ojson g = gi.value("group", ojson::object());
        if (g.empty()) g = {{"name", def.name}, {"pivot", ojson::array({0, 0, 0})}, {"cubes", ojson()}, {"groups", ojson()}};
        // its name: as read, unless it was renamed here (and never two of one name)
        std::string name = def.name == gi.value("name", std::string()) ? g.value("name", def.name) : def.name;
        if (const int n = ++placed[in.def]; n > 1) name += "_" + std::to_string(n);
        g["name"] = name;
        // its turn in the file's terms: a rotation about its pivot, and a shift (put into all it holds)
        const Mat4 T = parentBake * in.xf * parentBake.inverted();
        Vec3 pivot = vec3(g, "pivot");
        const Mat4 readXf = [&] {
            Mat4 x;
            if (gi.contains("xf") && gi["xf"].is_array() && gi["xf"].size() == 16)
                for (int k = 0; k < 16; ++k) x.m[k] = gi["xf"][size_t(k)].get<double>();
            return x;
        }();
        Mat4 bake;
        const bool asRead = gi.contains("xf") && sameMat(parentBake, w.base) && sameMat(in.xf, readXf);
        if (!asRead) {
            M3 R;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) R[r * 3 + c] = T.at(r, c);
            const M3 RtR = mul({R[0], R[3], R[6], R[1], R[4], R[7], R[2], R[5], R[8]}, R);
            bool rigid = true;
            for (int k = 0; k < 9; ++k) rigid &= std::abs(RtR[k] - (k % 4 == 0 ? 1 : 0)) < 1e-6;
            rigid &= R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]) > 0;
            if (rigid) {
                const Vec3 rot = eulerOf(R);
                const Vec3 d = T.transformPoint(pivot) - pivot;
                pivot = pivot + d;
                bake = Mat4::translation(d) * parentBake;
                g["pivot"] = arr3(pivot);
                if (rot.x != 0 || rot.y != 0 || rot.z != 0) g["rotation"] = arr3(rot);
                else g.erase("rotation");
            } else { // (stretched or mirrored: into its cubes, the group not turned)
                bake = T * parentBake;
                g["pivot"] = arr3(T.transformPoint(pivot));
                g.erase("rotation");
                if (msgs) msgs->append(QStringLiteral("%1 is stretched or mirrored: written into its cubes").arg(QString::fromStdString(name)));
            }
        } else {
            bake = parentBake;
        }
        if (in.hidden != hasTag(g, "hidden")) {
            ojson tags = g.value("tags", ojson::array());
            if (in.hidden) tags.push_back("hidden");
            else tags.erase(std::remove(tags.begin(), tags.end(), ojson("hidden")), tags.end());
            g["tags"] = tags;
        }
        const ojson cubes = w.cubes(def.mesh, bake, gi.value("cubes", ojson::array()));
        ojson groups = ojson::array();
        if (depth < 32)
            for (Id k : kids[in.def]) groups.push_back(groupOf(k, bake, depth + 1));
        putList(g, "cubes", cubes), putList(g, "groups", groups);
        return g;
    };
    ojson groups = ojson::array();
    for (Id k : kids[kNone]) groups.push_back(groupOf(k, w.base, 0));
    const ojson cubes = w.cubes(st.mesh, w.base, info.value("cubes", ojson::array()));
    if (!w.textures.empty() || model.contains("textures")) model["textures"] = w.textures;
    putList(model, "groups", groups), putList(model, "cubes", cubes);
    const int indent = info.value("indent", 1);
    std::string out;
    dumpPy(out, model, indent, 0);
    if (info.value("newline", false)) out += "\n";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(out.data(), qint64(out.size())) != qint64(out.size())) {
        if (msgs) msgs->append(f.errorString());
        return false;
    }
    if (msgs) {
        if (w.notBoxes) msgs->append(QStringLiteral("%1 shapes aren't boxes (an Open Block Model is made of boxes): left out").arg(w.notBoxes));
        if (w.noPicture) msgs->append(QStringLiteral("%1 faces have no picture (a colour can't be written): drawn with the game's missing texture").arg(w.noPicture));
        for (const std::string& p : w.newPaths)
            msgs->append(QStringLiteral("New picture \"%1\": put it in the game's textures folder as %1.png").arg(QString::fromStdString(p)));
    }
    return true;
}

namespace {
std::vector<std::string> tagsOf(const ojson& j) {
    std::vector<std::string> out;
    if (j.contains("tags") && j["tags"].is_array())
        for (const ojson& t : j["tags"])
            if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}
void putTags(ojson& j, const std::vector<std::string>& tags) {
    if (tags.empty() && !j.contains("tags")) return;
    j["tags"] = tags;
}
} // namespace

bool groupInfo(const Document& doc, Id def, GroupInfo& out) {
    if (def < 0 || def >= (Id)doc.state.defs.size()) return false;
    const ojson x = parseExtra(doc.state.defs[def].extra);
    if (!x.contains("obm")) return false;
    const ojson& g = x["obm"].value("group", ojson::object());
    out.name = g.value("name", doc.state.defs[def].name);
    out.pivot = vec3(g, "pivot"), out.rotation = vec3(g, "rotation");
    out.tags = tagsOf(g);
    out.cubes.clear();
    for (const ojson& r : x["obm"].value("cubes", ojson::array())) {
        const ojson c = r.value("json", ojson::object());
        out.cubes.push_back({c.value("name", std::string("cube")), tagsOf(c)});
    }
    return true;
}

bool modelInfo(const Document& doc, ModelInfo& out) {
    const ojson x = parseExtra(doc.state.extra);
    if (!x.contains("obm")) return false;
    const ojson& m = x["obm"].value("model", ojson::object());
    out.id = m.value("id", ""), out.kind = m.value("kind", ""), out.anchor = m.value("anchor", "corner");
    out.file = x["obm"].value("file", "");
    out.tags = tagsOf(m);
    out.textures.clear();
    for (const ojson& t : m.value("textures", ojson::array())) out.textures.push_back(t.value("path", ""));
    return true;
}

void setGroupTags(Document& doc, Id def, const std::vector<std::string>& tags) {
    if (def < 0 || def >= (Id)doc.state.defs.size()) return;
    ojson x = parseExtra(doc.state.defs[def].extra);
    if (!x.contains("obm")) return;
    if (!x["obm"].contains("group")) x["obm"]["group"] = ojson::object();
    putTags(x["obm"]["group"], tags);
    doc.state.defs[def].extra = x.dump();
}

void setCubeTags(Document& doc, Id def, int cube, const std::vector<std::string>& tags) {
    if (def < 0 || def >= (Id)doc.state.defs.size()) return;
    ojson x = parseExtra(doc.state.defs[def].extra);
    if (!x.contains("obm") || !x["obm"].contains("cubes") || cube < 0 || cube >= (int)x["obm"]["cubes"].size()) return;
    // (the record's cube: written as it is while the cube is unchanged, its tags taken from it if it was changed)
    putTags(x["obm"]["cubes"][size_t(cube)]["json"], tags);
    doc.state.defs[def].extra = x.dump();
}

void setModelTags(Document& doc, const std::vector<std::string>& tags) {
    ojson x = parseExtra(doc.state.extra);
    if (!x.contains("obm")) return;
    if (!x["obm"].contains("model")) x["obm"]["model"] = ojson::object();
    putTags(x["obm"]["model"], tags);
    doc.state.extra = x.dump();
}

QString picturePath(const QString& folder, const std::string& name) {
    return folder + "/" + QString::fromStdString(name.substr(0, name.find('@'))) + ".png";
}

} // namespace gs::obm
