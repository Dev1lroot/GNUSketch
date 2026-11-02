// SPDX-License-Identifier: GPL-3.0-or-later
#include "Animation.h"

#include "Components.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace gs {

int findState(const ComponentDef& def, const std::string& name) {
    for (int i = 0; i < (int)def.states.size(); ++i)
        if (def.states[i].name == name) return i;
    return -1;
}

bool recordState(ModelState& st, const std::string& name) {
    if (st.editing == kNone || name.empty()) return false;
    const Instance& in = st.instances[st.editing];
    ComponentDef& def = st.defs[in.def];
    // While open the mesh holds world coordinates; see Components.h.
    const Mat4 toLocal = in.xf.inverted();
    AnimState s;
    s.name = name;
    for (Id v = 0; v < (Id)def.mesh.vertices.size(); ++v)
        if (def.mesh.vertices[v].alive) {
            s.verts.push_back(v);
            s.pos.push_back(toLocal.transformPoint(def.mesh.vertices[v].p));
        }
    for (Id c : childrenOf(st, in.def)) s.children.emplace_back(c, toLocal * st.instances[c].xf);
    if (s.verts.empty() && s.children.empty()) return false;
    const int i = findState(def, name);
    if (i >= 0) def.states[i] = std::move(s);
    else def.states.push_back(std::move(s));
    return true;
}

bool applyState(ModelState& st, Id d, const std::string& name) {
    if (d < 0 || d >= (Id)st.defs.size()) return false;
    ComponentDef& def = st.defs[d];
    const int i = findState(def, name);
    if (i < 0) return false;
    const bool open = st.editing != kNone && st.instances[st.editing].def == d;
    const Mat4 toMesh = open ? st.instances[st.editing].xf : Mat4();
    const AnimState& s = def.states[i];
    Mesh& m = def.mesh;
    for (size_t k = 0; k < s.verts.size(); ++k) {
        const Id v = s.verts[k];
        if (v >= 0 && v < (Id)m.vertices.size() && m.vertices[v].alive) m.vertices[v].p = toMesh.transformPoint(s.pos[k]);
    }
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive) m.recomputeNormal(f);
    for (const auto& [c, xf] : s.children)
        if (c >= 0 && c < (Id)st.instances.size() && st.instances[c].parent == d) st.instances[c].xf = toMesh * xf;
    m.markGeometryMoved();
    m.touch();
    return true;
}

namespace {

// Eigenvector of the largest eigenvalue of a symmetric 4x4 matrix (Jacobi).
void largestEigenvector(double A[4][4], double out[4]) {
    double V[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) off += A[p][q] * A[p][q];
        if (off < 1e-30) break;
        for (int p = 0; p < 4; ++p)
            for (int q = p + 1; q < 4; ++q) {
                if (std::fabs(A[p][q]) < 1e-300) continue;
                const double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                const double t = (th >= 0 ? 1 : -1) / (std::fabs(th) + std::sqrt(th * th + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double a = A[k][p], b = A[k][q];
                    A[k][p] = c * a - s * b;
                    A[k][q] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const double a = A[p][k], b = A[q][k];
                    A[p][k] = c * a - s * b;
                    A[q][k] = s * a + c * b;
                }
                for (int k = 0; k < 4; ++k) {
                    const double a = V[k][p], b = V[k][q];
                    V[k][p] = c * a - s * b;
                    V[k][q] = s * a + c * b;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (A[i][i] > A[best][best]) best = i;
    for (int k = 0; k < 4; ++k) out[k] = V[k][best];
}

Mat4 fromRotationTranslation(const double R[3][3], const Vec3& t) {
    Mat4 m;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m.at(r, c) = R[r][c];
        m.at(r, 3) = t[r];
    }
    return m;
}

void rotationAboutAxis(const Vec3& u, double angle, double R[3][3]) {
    const double c = std::cos(angle), s = std::sin(angle), k = 1 - c;
    R[0][0] = c + u.x * u.x * k;
    R[0][1] = u.x * u.y * k - u.z * s;
    R[0][2] = u.x * u.z * k + u.y * s;
    R[1][0] = u.y * u.x * k + u.z * s;
    R[1][1] = c + u.y * u.y * k;
    R[1][2] = u.y * u.z * k - u.x * s;
    R[2][0] = u.z * u.x * k - u.y * s;
    R[2][1] = u.z * u.y * k + u.x * s;
    R[2][2] = c + u.z * u.z * k;
}

} // namespace

Mat4 fitRigid(const std::vector<Vec3>& a, const std::vector<Vec3>& b, double* residual) {
    Mat4 out;
    if (residual) *residual = 0;
    if (a.empty() || a.size() != b.size()) return out;
    Vec3 ca, cb;
    for (size_t i = 0; i < a.size(); ++i) {
        ca += a[i];
        cb += b[i];
    }
    ca = ca / (double)a.size();
    cb = cb / (double)a.size();
    // Horn's closed form: the rotation is the top eigenvector of a 4x4 matrix.
    double S[3][3] = {};
    for (size_t i = 0; i < a.size(); ++i) {
        const Vec3 p = a[i] - ca, q = b[i] - cb;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) S[r][c] += p[r] * q[c];
    }
    double N[4][4] = {{S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
                      {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
                      {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
                      {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
    double q[4];
    largestEigenvector(N, q);
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double R[3][3] = {{w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z}};
    Vec3 rc;
    for (int r = 0; r < 3; ++r) rc[r] = R[r][0] * ca.x + R[r][1] * ca.y + R[r][2] * ca.z;
    out = fromRotationTranslation(R, cb - rc);
    if (residual) {
        double worst = 0;
        for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, (out.transformPoint(a[i]) - b[i]).length());
        *residual = worst;
    }
    return out;
}

Mat4 interpolateRigid(const Mat4& from, const Mat4& to, double s) {
    if (s <= 0) return from;
    if (s >= 1) return to;
    const Mat4 T = to * from.inverted();
    const Vec3 t(T.at(0, 3), T.at(1, 3), T.at(2, 3));
    // The rotation's quaternion gives a stable axis and angle at any angle.
    const double tr = T.at(0, 0) + T.at(1, 1) + T.at(2, 2);
    double qw, qx, qy, qz;
    if (tr > 0) {
        const double r = std::sqrt(tr + 1) * 2;
        qw = r / 4; qx = (T.at(2, 1) - T.at(1, 2)) / r; qy = (T.at(0, 2) - T.at(2, 0)) / r; qz = (T.at(1, 0) - T.at(0, 1)) / r;
    } else if (T.at(0, 0) > T.at(1, 1) && T.at(0, 0) > T.at(2, 2)) {
        const double r = std::sqrt(1 + T.at(0, 0) - T.at(1, 1) - T.at(2, 2)) * 2;
        qw = (T.at(2, 1) - T.at(1, 2)) / r; qx = r / 4; qy = (T.at(0, 1) + T.at(1, 0)) / r; qz = (T.at(0, 2) + T.at(2, 0)) / r;
    } else if (T.at(1, 1) > T.at(2, 2)) {
        const double r = std::sqrt(1 + T.at(1, 1) - T.at(0, 0) - T.at(2, 2)) * 2;
        qw = (T.at(0, 2) - T.at(2, 0)) / r; qx = (T.at(0, 1) + T.at(1, 0)) / r; qy = r / 4; qz = (T.at(1, 2) + T.at(2, 1)) / r;
    } else {
        const double r = std::sqrt(1 + T.at(2, 2) - T.at(0, 0) - T.at(1, 1)) * 2;
        qw = (T.at(1, 0) - T.at(0, 1)) / r; qx = (T.at(0, 2) + T.at(2, 0)) / r; qy = (T.at(1, 2) + T.at(2, 1)) / r; qz = r / 4;
    }
    if (qw < 0) { qw = -qw; qx = -qx; qy = -qy; qz = -qz; } // the short way round
    const double vlen = std::sqrt(qx * qx + qy * qy + qz * qz);
    Mat4 M;
    if (vlen < 1e-9) {
        M = Mat4::translation(t * s);
    } else {
        const Vec3 u(qx / vlen, qy / vlen, qz / vlen);
        const double angle = 2 * std::atan2(vlen, qw);
        const Vec3 along = u * t.dot(u), across = t - along;
        // The axis stays put: (I - R) c = across.
        const Vec3 c = (across + u.cross(across) * (1 / std::tan(angle / 2))) * 0.5;
        double R[3][3];
        rotationAboutAxis(u, angle * s, R);
        Vec3 rc;
        for (int r = 0; r < 3; ++r) rc[r] = R[r][0] * c.x + R[r][1] * c.y + R[r][2] * c.z;
        M = fromRotationTranslation(R, c - rc + along * s);
    }
    return M * from;
}

std::vector<std::vector<Id>> meshIslands(const Mesh& m) {
    std::vector<int> parent(m.vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    std::vector<char> used(m.vertices.size(), 0);
    for (const Edge& e : m.edges) {
        if (!e.alive) continue;
        parent[find(e.v[0])] = find(e.v[1]);
        used[e.v[0]] = used[e.v[1]] = 1;
    }
    std::vector<std::vector<Id>> groups(m.vertices.size());
    for (Id v = 0; v < (Id)m.vertices.size(); ++v)
        if (used[v] && m.vertices[v].alive) groups[find(v)].push_back(v);
    std::erase_if(groups, [](const std::vector<Id>& g) { return g.empty(); });
    return groups;
}

namespace {

// Geometry of a definition and the plain components inside it, merged, in the
// coordinates xf maps to; entities inside are listed instead.
void collectPlain(ModelState& st, Id def, const Mat4& xf, Mesh& out, std::vector<std::pair<Id, Mat4>>& entities, int depth = 0) {
    if (depth > 32) return;
    appendMesh(out, st.defs[def].mesh, xf);
    for (Id c : childrenOf(st, def)) {
        const Instance& I = st.instances[c];
        if (I.hidden) continue;
        const Mat4 cxf = xf * I.xf;
        if (I.entity.isEntity()) entities.emplace_back(c, cxf);
        else collectPlain(st, I.def, cxf, out, entities, depth + 1);
    }
}

Id addPrivateInstance(ModelState& st, Mesh&& mesh, const std::string& name, const Mat4& xf, bool dynamic) {
    ComponentDef own;
    own.name = name;
    own.mesh = std::move(mesh);
    own.mesh.touch();
    st.defs.push_back(std::move(own));
    Instance inst;
    inst.def = (Id)st.defs.size() - 1;
    inst.xf = xf;
    inst.dynamic = dynamic;
    st.instances.push_back(inst);
    return (Id)st.instances.size() - 1;
}

void expandEntity(ModelState& st, std::vector<AnimEntity>& out, Id source, const Mat4& base, int owner, int ownerPart, const Mat4& mount) {
    const Instance src = st.instances[source];
    const ComponentDef def = st.defs[src.def]; // a copy: st.defs grows below
    const int self = (int)out.size();
    out.emplace_back();
    {
        AnimEntity& e = out[self];
        e.source = source;
        e.props = src.entity;
        e.owner = owner;
        e.ownerPart = ownerPart;
        e.mount = mount;
        for (const AnimState& s : def.states) e.states.push_back(s.name);
    }
    for (const std::vector<Id>& island : meshIslands(def.mesh)) {
        AnimPart part;
        part.base = base;
        // A copy with only this piece alive: vertex ids stay valid.
        Mesh mesh = def.mesh;
        std::vector<char> in(mesh.vertices.size(), 0);
        for (Id v : island) in[v] = 1;
        for (size_t v = 0; v < mesh.vertices.size(); ++v) mesh.vertices[v].alive = mesh.vertices[v].alive && in[v];
        for (Edge& ed : mesh.edges) ed.alive = ed.alive && in[ed.v[0]];
        for (Face& f : mesh.faces) f.alive = f.alive && !f.outer.empty() && in[f.outer[0]];
        part.rest.resize(mesh.vertices.size());
        for (Id v = 0; v < (Id)mesh.vertices.size(); ++v) part.rest[v] = def.mesh.vertices[v].p;
        std::vector<Vec3> restPts;
        for (Id v : island) restPts.push_back(part.rest[v]);
        double size = 0;
        for (const Vec3& p : restPts) size = std::max(size, (p - restPts[0]).length());
        for (const AnimState& s : def.states) {
            std::vector<Vec3> full = part.rest;
            for (size_t k = 0; k < s.verts.size(); ++k)
                if (s.verts[k] >= 0 && s.verts[k] < (Id)full.size()) full[s.verts[k]] = s.pos[k];
            std::vector<Vec3> statePts;
            for (Id v : island) statePts.push_back(full[v]);
            double residual = 0;
            const Mat4 fit = fitRigid(restPts, statePts, &residual);
            const bool rigid = residual <= std::max(1e-6, size * 1e-5);
            part.fit.push_back(fit);
            part.rigid.push_back(rigid);
            part.deforms = part.deforms || !rigid;
            part.shape.push_back(std::move(full));
        }
        part.instance = addPrivateInstance(st, std::move(mesh), def.name, base, true);
        part.def = st.instances[part.instance].def;
        out[self].parts.push_back(std::move(part));
    }
    // Each component inside is a part too, moving as the states say.
    for (Id c : childrenOf(st, src.def)) {
        const Instance child = st.instances[c];
        if (child.hidden) continue;
        Mesh merged;
        std::vector<std::pair<Id, Mat4>> inner;
        if (child.entity.isEntity()) inner.emplace_back(c, child.xf);
        else collectPlain(st, child.def, child.xf, merged, inner);
        AnimPart part;
        part.base = base;
        const Mat4 restInv = child.xf.inverted();
        for (const AnimState& s : def.states) {
            Mat4 fit;
            for (const auto& [id, xf] : s.children)
                if (id == c) fit = xf * restInv;
            part.fit.push_back(fit);
            part.rigid.push_back(1);
        }
        part.instance = addPrivateInstance(st, std::move(merged), def.name, base, true);
        part.def = st.instances[part.instance].def;
        const int partIndex = (int)out[self].parts.size();
        out[self].parts.push_back(std::move(part));
        for (const auto& [id, xf] : inner) expandEntity(st, out, id, base * xf, self, partIndex, xf);
    }
}

} // namespace

std::vector<AnimEntity> prepareEntities(ModelState& st) {
    closeAllComponents(st);
    std::vector<AnimEntity> out;
    const Id originalCount = (Id)st.instances.size();
    for (Id i = 0; i < originalCount; ++i) {
        const Instance src = st.instances[i];
        if (!src.alive || src.hidden || src.parent != kNone || src.def < 0 || src.def >= (Id)st.defs.size()) continue;
        if (src.entity.isEntity()) {
            expandEntity(st, out, i, src.xf, -1, -1, Mat4());
            st.instances[i].alive = false;
            continue;
        }
        if (childrenOf(st, src.def).empty()) continue; // plain geometry stays as it is
        // Plain with things inside: its plain contents become one static piece.
        Mesh merged;
        std::vector<std::pair<Id, Mat4>> inner;
        collectPlain(st, src.def, src.xf, merged, inner);
        addPrivateInstance(st, std::move(merged), st.defs[src.def].name, Mat4(), false);
        for (const auto& [id, xf] : inner) expandEntity(st, out, id, xf, -1, -1, Mat4());
        st.instances[i].alive = false;
    }
    return out;
}

void poseMounted(ModelState& st, std::vector<AnimEntity>& all, int i, const std::function<void(int)>& moved) {
    for (int j = 0; j < (int)all.size(); ++j) {
        AnimEntity& e = all[j];
        if (e.owner != i) continue;
        const Mat4 base = st.instances[all[i].parts[e.ownerPart].instance].xf * e.mount;
        for (AnimPart& p : e.parts) p.base = base;
        poseEntity(st, e, e.poseA, e.poseB, e.poseT);
        moved(j);
        poseMounted(st, all, j, moved);
    }
}

bool poseEntity(ModelState& st, const AnimEntity& e, int a, int b, double t) {
    bool changedShape = false;
    const int n = (int)e.states.size();
    if (a < 0 || b < 0 || a >= n || b >= n) {
        // No states to go by: it just stays on its base.
        for (const AnimPart& p : e.parts) st.instances[p.instance].xf = p.base;
        return false;
    }
    for (const AnimPart& p : e.parts) {
        if (p.rigid[a] && p.rigid[b]) {
            if (p.deforms) {
                // It may have been deformed by an earlier pose.
                Mesh& m = st.defs[p.def].mesh;
                for (Id v = 0; v < (Id)m.vertices.size(); ++v) m.vertices[v].p = p.rest[v];
                for (Id f = 0; f < (Id)m.faces.size(); ++f)
                    if (m.faces[f].alive) m.recomputeNormal(f);
                m.touch();
                changedShape = true;
            }
            st.instances[p.instance].xf = p.base * interpolateRigid(p.fit[a], p.fit[b], t);
        } else {
            Mesh& m = st.defs[p.def].mesh;
            for (Id v = 0; v < (Id)m.vertices.size(); ++v) m.vertices[v].p = lerp(p.shape[a][v], p.shape[b][v], t);
            for (Id f = 0; f < (Id)m.faces.size(); ++f)
                if (m.faces[f].alive) m.recomputeNormal(f);
            m.touch();
            st.instances[p.instance].xf = p.base;
            changedShape = true;
        }
    }
    return changedShape;
}

} // namespace gs
