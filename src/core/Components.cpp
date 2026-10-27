// SPDX-License-Identifier: GPL-3.0-or-later
#include "Components.h"

#include <map>
#include <random>

namespace gs {

namespace {

void bakeTransform(Mesh& m, const Mat4& xf) {
    if (xf.isIdentity()) return;
    std::set<Id> verts;
    for (Id v = 0; v < (Id)m.vertices.size(); ++v)
        if (m.vertices[v].alive) verts.insert(v);
    m.transformVertices(verts, xf, false);
    m.touch();
}

void grow(Vec3& mn, Vec3& mx, bool& any, const Vec3& p) {
    if (!any) mn = mx = p;
    for (int i = 0; i < 3; ++i) {
        mn[i] = std::min(mn[i], p[i]);
        mx[i] = std::max(mx[i], p[i]);
    }
    any = true;
}

bool aliveInstance(const ModelState& st, Id i) {
    return i >= 0 && i < (Id)st.instances.size() && st.instances[i].alive && st.instances[i].def >= 0 &&
           st.instances[i].def < (Id)st.defs.size();
}

} // namespace

Mat4 instanceWorldXf(const ModelState& st, Id inst) {
    const Instance& I = st.instances[inst];
    if (st.editing == kNone) return I.xf;
    if (inst == st.editing) return Mat4();
    const Instance& E = st.instances[st.editing];
    if (E.def != I.def) return I.xf;
    // The definition mesh currently holds E.xf-transformed coordinates.
    return I.xf * E.xf.inverted();
}

bool instanceBoxCorners(const ModelState& st, Id inst, Vec3 corners[8]) {
    if (!aliveInstance(st, inst)) return false;
    const Instance& I = st.instances[inst];
    const Mesh& m = st.defs[I.def].mesh;
    // Box in the definition's own axes: undo a baked edit transform first.
    Mat4 toLocal = (st.editing != kNone && st.instances[st.editing].def == I.def) ? st.instances[st.editing].xf.inverted()
                                                                                : Mat4();
    Vec3 mn, mx;
    bool any = false;
    for (const Vertex& v : m.vertices)
        if (v.alive) grow(mn, mx, any, toLocal.transformPoint(v.p));
    if (!any) return false;
    Mat4 toWorld = instanceWorldXf(st, inst) * toLocal.inverted();
    for (int i = 0; i < 8; ++i)
        corners[i] = toWorld.transformPoint(Vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z));
    return true;
}

bool instanceBounds(const ModelState& st, Id inst, Vec3& mn, Vec3& mx) {
    Vec3 c[8];
    if (!instanceBoxCorners(st, inst, c)) return false;
    bool any = false;
    for (const Vec3& p : c) grow(mn, mx, any, p);
    return any;
}

void modelBounds(const ModelState& st, Vec3& mn, Vec3& mx, bool& any) {
    st.mesh.bounds(mn, mx, any);
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        if (!aliveInstance(st, i) || st.instances[i].hidden) continue;
        Vec3 a, b;
        if (!instanceBounds(st, i, a, b)) continue;
        grow(mn, mx, any, a);
        grow(mn, mx, any, b);
    }
}

int instanceCount(const ModelState& st, Id def) {
    int n = 0;
    for (const Instance& I : st.instances) n += I.alive && I.def == def;
    return n;
}

void appendMesh(Mesh& dst, const Mesh& src, const Mat4& xf, const std::set<Id>* faces, const std::set<Id>* edges,
                bool weld, Selection* created) {
    std::set<Id> fs, es;
    if (!faces && !edges) {
        for (Id f = 0; f < (Id)src.faces.size(); ++f)
            if (src.faces[f].alive) fs.insert(f);
        for (Id e = 0; e < (Id)src.edges.size(); ++e)
            if (src.edges[e].alive) es.insert(e);
    } else {
        if (faces)
            for (Id f : *faces)
                if (src.faces[f].alive) fs.insert(f);
        if (edges)
            for (Id e : *edges)
                if (src.edges[e].alive) es.insert(e);
        for (Id f : fs)
            for (Id e : src.faceEdges(f)) es.insert(e);
    }
    const bool identity = xf.isIdentity();
    std::map<Id, Id> vm;
    auto mapV = [&](Id v) {
        auto it = vm.find(v);
        if (it != vm.end()) return it->second;
        Vec3 p = xf.transformPoint(src.pos(v));
        Id nv = weld ? dst.addVertex(p) : dst.addVertexRaw(p);
        if (dst.vertices[nv].paint.empty()) dst.vertices[nv].paint = src.vertices[v].paint;
        vm[v] = nv;
        return nv;
    };
    std::map<Id, Id> curveMap;
    for (Id e : es) {
        const Edge& E = src.edges[e];
        Id a = mapV(E.v[0]), b = mapV(E.v[1]);
        if (a == b) continue;
        Id ne = dst.addEdgeRaw(a, b);
        if (ne == kNone) continue;
        Edge& N = dst.edges[ne];
        N.soft = E.soft;
        N.smooth = E.smooth;
        N.hidden = E.hidden;
        if (E.curve != kNone) {
            auto it = curveMap.find(E.curve);
            if (it == curveMap.end()) it = curveMap.emplace(E.curve, dst.newCurveId()).first;
            N.curve = it->second;
        }
        if (created) created->edges.insert(ne);
    }
    for (Id f : fs) {
        const Face& F = src.faces[f];
        std::vector<Id> outer;
        for (Id v : F.outer) outer.push_back(mapV(v));
        std::vector<std::vector<Id>> holes;
        for (const auto& h : F.holes) {
            holes.emplace_back();
            for (Id v : h) holes.back().push_back(mapV(v));
        }
        Vec3 n = identity ? F.normal : transformCovector(xf, F.normal).normalized();
        Id nf = dst.addFaceRaw(outer, holes, n);
        if (nf == kNone) continue;
        Face& N = dst.faces[nf];
        N.front = F.front;
        N.back = F.back;
        N.hidden = F.hidden;
        if (!identity)
            for (FaceSide* s : {&N.front, &N.back}) {
                if (s->axis.length2() > 0.5) s->axis = xf.transformDir(s->axis).normalized();
                if (!s->tex.valid) continue;
                s->tex.origin = xf.transformPoint(s->tex.origin);
                s->tex.u = transformCovector(xf, s->tex.u);
                s->tex.v = transformCovector(xf, s->tex.v);
            }
        if (created) created->faces.insert(nf);
    }
    dst.touch();
    dst.markGeometryMoved();
}

std::string uniqueComponentName(const ModelState& st, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (Id d = 0; d < (Id)st.defs.size(); ++d)
            if (st.defs[d].name == n && instanceCount(st, d) > 0) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + "#" + std::to_string(i);
        if (!taken(n)) return n;
    }
}

std::string newComponentUid() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    uint64_t a = rng(), b = rng();
    for (int i = 0; i < 16; ++i) s += hex[(a >> (i * 4)) & 15];
    for (int i = 0; i < 16; ++i) s += hex[(b >> (i * 4)) & 15];
    return s;
}

Id makeComponent(ModelState& st, const Selection& sel, const std::string& name) {
    if (st.editing != kNone) return kNone; // no nested components
    Mesh& top = st.mesh;
    std::set<Id> faces, edges;
    for (Id f : sel.faces)
        if (f >= 0 && f < (Id)top.faces.size() && top.faces[f].alive) faces.insert(f);
    for (Id e : sel.edges)
        if (e >= 0 && e < (Id)top.edges.size() && top.edges[e].alive) edges.insert(e);
    for (Id f : faces)
        for (Id e : top.faceEdges(f)) edges.insert(e);
    if (edges.empty()) return kNone;

    // Component axes at the minimum corner of the geometry's bounding box.
    Vec3 mn, mx;
    bool any = false;
    for (Id e : edges)
        for (Id v : top.edges[e].v) grow(mn, mx, any, top.pos(v));

    ComponentDef def;
    def.name = uniqueComponentName(st, name.empty() ? std::string("Component") : name);
    def.uid = newComponentUid();
    appendMesh(def.mesh, top, Mat4::translation(-mn), &faces, &edges);
    st.defs.push_back(std::move(def));

    // Remove the geometry from the top level. Edges still bounding faces that
    // were not selected stay, so the surrounding geometry is left intact.
    for (Id f : faces) top.eraseFace(f);
    for (Id e : edges) {
        bool used = false;
        for (Id f : top.edgeFaces(e)) used = used || top.faces[f].alive;
        if (!used) top.eraseEdge(e, false);
    }
    top.removeOrphanVertices();
    top.touch();

    Instance inst;
    inst.def = (Id)st.defs.size() - 1;
    inst.xf = Mat4::translation(mn);
    st.instances.push_back(inst);
    return (Id)st.instances.size() - 1;
}

Id cloneInstance(ModelState& st, Id inst) {
    if (!aliveInstance(st, inst)) return kNone;
    Instance c = st.instances[inst];
    st.instances.push_back(c);
    return (Id)st.instances.size() - 1;
}

void explodeInstance(ModelState& st, Id inst, Selection* out) {
    if (!aliveInstance(st, inst)) return;
    if (st.editing != kNone) closeComponent(st);
    Instance& I = st.instances[inst];
    appendMesh(st.mesh, st.defs[I.def].mesh, I.xf, nullptr, nullptr, true, out);
    I.alive = false;
}

void makeUnique(ModelState& st, const std::set<Id>& insts) {
    std::map<Id, std::vector<Id>> byDef;
    for (Id i : insts)
        if (aliveInstance(st, i)) byDef[st.instances[i].def].push_back(i);
    for (auto& [d, list] : byDef) {
        if ((int)list.size() >= instanceCount(st, d)) continue;
        ComponentDef copy = st.defs[d];
        copy.name = uniqueComponentName(st, st.defs[d].name);
        copy.uid = newComponentUid();
        copy.mesh.touch();
        st.defs.push_back(std::move(copy));
        for (Id i : list) st.instances[i].def = (Id)st.defs.size() - 1;
    }
}

void openComponent(ModelState& st, Id inst) {
    if (st.editing == inst) return;
    if (st.editing != kNone) closeComponent(st);
    if (!aliveInstance(st, inst)) return;
    bakeTransform(st.defs[st.instances[inst].def].mesh, st.instances[inst].xf);
    st.editing = inst;
}

void closeComponent(ModelState& st) {
    if (st.editing == kNone) return;
    const Instance& I = st.instances[st.editing];
    bakeTransform(st.defs[I.def].mesh, I.xf.inverted());
    st.editing = kNone;
}

ModelState closedState(const ModelState& st) {
    ModelState c = st;
    closeComponent(c);
    return c;
}

Mesh flattenModel(const ModelState& st) {
    Mesh out = st.mesh;
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        if (!aliveInstance(st, i) || st.instances[i].hidden) continue;
        appendMesh(out, st.defs[st.instances[i].def].mesh, instanceWorldXf(st, i));
    }
    return out;
}

} // namespace gs
