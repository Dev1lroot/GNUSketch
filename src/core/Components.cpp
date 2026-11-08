// SPDX-License-Identifier: GPL-3.0-or-later
#include "Components.h"

#include <algorithm>
#include <functional>
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

namespace {

// What is baked into a definition's coordinates just now.
Mat4 bakeOf(const ModelState& st, Id def) {
    if (st.editing != kNone && st.instances[st.editing].def == def) return st.instances[st.editing].xf;
    for (Id o : st.outerOpen)
        if (st.instances[o].def == def) return st.instances[o].xf;
    return Mat4();
}

std::map<Id, std::vector<Id>> childMap(const ModelState& st) {
    std::map<Id, std::vector<Id>> kids;
    for (Id i = 0; i < (Id)st.instances.size(); ++i)
        if (st.instances[i].parent != kNone && aliveInstance(st, i)) kids[st.instances[i].parent].push_back(i);
    return kids;
}

} // namespace

std::vector<Id> childrenOf(const ModelState& st, Id def) {
    std::vector<Id> out;
    for (Id i = 0; i < (Id)st.instances.size(); ++i)
        if (st.instances[i].parent == def && aliveInstance(st, i)) out.push_back(i);
    return out;
}

Mat4 instanceWorldXf(const ModelState& st, Id inst) {
    const Instance& I = st.instances[inst];
    // Mesh -> the coordinates I.xf maps into, which at the edited level are the world's.
    return I.xf * bakeOf(st, I.def).inverted();
}

std::vector<PlacedInstance> placedInstances(const ModelState& st) {
    std::vector<PlacedInstance> out;
    const auto kids = childMap(st);
    std::function<void(Id, const Mat4&, bool, Id, int)> walk = [&](Id i, const Mat4& outer, bool active, Id root, int depth) {
        const Instance& I = st.instances[i];
        if (!I.alive || I.hidden || I.def < 0 || I.def >= (Id)st.defs.size() || depth > 32) return;
        const Mat4 xf = outer * I.xf * bakeOf(st, I.def).inverted();
        out.push_back({i, I.def, xf, active, root});
        auto it = kids.find(I.def);
        if (it == kids.end()) return;
        for (Id c : it->second) {
            if (i == st.editing) walk(c, xf, true, c, depth + 1);
            else walk(c, xf, active, root, depth + 1);
        }
    };
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        if (st.instances[i].parent != kNone) continue;
        if (st.editing == kNone) walk(i, Mat4(), true, i, 0);
        else walk(i, Mat4(), false, kNone, 0);
    }
    return out;
}

void definitionBounds(const ModelState& st, Id def, Vec3& mn, Vec3& mx, bool& any) {
    const Mat4 toLocal = bakeOf(st, def).inverted();
    for (const Vertex& v : st.defs[def].mesh.vertices)
        if (v.alive) grow(mn, mx, any, toLocal.transformPoint(v.p));
    for (Id c : childrenOf(st, def)) {
        if (st.instances[c].hidden) continue;
        Vec3 a, b;
        bool sub = false;
        definitionBounds(st, st.instances[c].def, a, b, sub);
        if (!sub) continue;
        const Mat4 xf = toLocal * st.instances[c].xf;
        for (int k = 0; k < 8; ++k) grow(mn, mx, any, xf.transformPoint(Vec3((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y, (k & 4) ? b.z : a.z)));
    }
}

bool definitionContains(const ModelState& st, Id def, Id to) {
    if (def == to) return true;
    for (Id c : childrenOf(st, def))
        if (definitionContains(st, st.instances[c].def, to)) return true;
    return false;
}

bool canPlaceInActive(const ModelState& st, Id def) {
    if (st.editing == kNone) return true;
    if (definitionContains(st, def, st.instances[st.editing].def)) return false;
    for (Id o : st.outerOpen)
        if (definitionContains(st, def, st.instances[o].def)) return false;
    return true;
}

bool instanceBoxCorners(const ModelState& st, Id inst, Vec3 corners[8]) {
    if (!aliveInstance(st, inst)) return false;
    const Instance& I = st.instances[inst];
    // Box in the definition's own axes.
    Vec3 mn, mx;
    bool any = false;
    definitionBounds(st, I.def, mn, mx, any);
    if (!any) return false;
    for (int i = 0; i < 8; ++i)
        corners[i] = I.xf.transformPoint(Vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z));
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
    for (const PlacedInstance& p : placedInstances(st))
        for (const Vertex& v : st.defs[p.def].mesh.vertices)
            if (v.alive) grow(mn, mx, any, p.xf.transformPoint(v.p));
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
    Mesh& top = st.activeMesh();
    std::set<Id> faces, edges, insts;
    for (Id f : sel.faces)
        if (f >= 0 && f < (Id)top.faces.size() && top.faces[f].alive) faces.insert(f);
    for (Id e : sel.edges)
        if (e >= 0 && e < (Id)top.edges.size() && top.edges[e].alive) edges.insert(e);
    for (Id i : sel.instances)
        if (aliveInstance(st, i) && st.instances[i].parent == st.activeDef()) insts.insert(i);
    for (Id f : faces)
        for (Id e : top.faceEdges(f)) edges.insert(e);
    if (edges.empty() && insts.empty()) return kNone;

    // Component axes at the minimum corner of the bounding box.
    Vec3 mn, mx;
    bool any = false;
    for (Id e : edges)
        for (Id v : top.edges[e].v) grow(mn, mx, any, top.pos(v));
    for (Id i : insts) {
        Vec3 a, b;
        if (instanceBounds(st, i, a, b)) {
            grow(mn, mx, any, a);
            grow(mn, mx, any, b);
        }
    }

    ComponentDef def;
    def.name = uniqueComponentName(st, name.empty() ? std::string("Component") : name);
    def.uid = newComponentUid();
    if (!edges.empty()) appendMesh(def.mesh, top, Mat4::translation(-mn), &faces, &edges);
    const Id parentBefore = st.activeDef();
    st.defs.push_back(std::move(def));
    const Id newDef = (Id)st.defs.size() - 1;

    // Remove the geometry from where it was. Edges still bounding faces that
    // were not selected stay, so the surrounding geometry is left intact.
    Mesh& from = st.activeMesh();
    for (Id f : faces) from.eraseFace(f);
    for (Id e : edges) {
        bool used = false;
        for (Id f : from.edgeFaces(e)) used = used || from.faces[f].alive;
        if (!used) from.eraseEdge(e, false);
    }
    from.removeOrphanVertices();
    from.touch();
    // The components go inside the new one.
    const Mat4 toLocal = Mat4::translation(-mn);
    for (Id i : insts) {
        st.instances[i].parent = newDef;
        st.instances[i].xf = toLocal * st.instances[i].xf;
    }

    Instance inst;
    inst.def = newDef;
    inst.parent = parentBefore;
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
    if (st.instances[inst].parent != st.activeDef()) closeAllComponents(st);
    const Instance I = st.instances[inst];
    appendMesh(st.activeMesh(), st.defs[I.def].mesh, I.xf * bakeOf(st, I.def).inverted(), nullptr, nullptr, true, out);
    // What is inside comes out, as copies: other placements keep theirs.
    for (Id c : childrenOf(st, I.def)) {
        Instance copy = st.instances[c];
        copy.parent = st.activeDef();
        copy.xf = I.xf * bakeOf(st, I.def).inverted() * copy.xf;
        st.instances.push_back(copy);
        if (out) out->instances.insert((Id)st.instances.size() - 1);
    }
    st.instances[inst].alive = false;
}

namespace {
// The children of `from` copied into definition `to`; returns old -> new instance.
std::map<Id, Id> copyChildren(ModelState& st, Id from, Id to) {
    std::map<Id, Id> m;
    for (Id c : childrenOf(st, from)) {
        Instance copy = st.instances[c];
        copy.parent = to;
        st.instances.push_back(copy);
        m[c] = (Id)st.instances.size() - 1;
    }
    return m;
}
void remapStateChildren(ComponentDef& d, const std::map<Id, Id>& m) {
    for (AnimState& s : d.states)
        for (auto& [id, xf] : s.children) {
            auto it = m.find(id);
            id = it == m.end() ? kNone : it->second;
        }
}
// Copies a definition and what is inside it into dst.
Id copyDefInto(ModelState& dst, const ModelState& src, Id d, std::map<Id, Id>& defMap) {
    auto done = defMap.find(d);
    if (done != defMap.end()) return done->second;
    dst.defs.push_back(src.defs[d]);
    const Id nd = (Id)dst.defs.size() - 1;
    defMap[d] = nd;
    std::map<Id, Id> instMap;
    for (Id c : childrenOf(src, d)) {
        const Id childDef = copyDefInto(dst, src, src.instances[c].def, defMap);
        Instance copy = src.instances[c];
        copy.def = childDef;
        copy.parent = nd;
        dst.instances.push_back(copy);
        instMap[c] = (Id)dst.instances.size() - 1;
    }
    remapStateChildren(dst.defs[nd], instMap);
    dst.defs[nd].mesh.touch();
    return nd;
}
} // namespace

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
        const Id nd = (Id)st.defs.size() - 1;
        remapStateChildren(st.defs[nd], copyChildren(st, d, nd));
        for (Id i : list) st.instances[i].def = nd;
    }
}

Id copyInstanceInto(ModelState& dst, const ModelState& src, Id inst, std::map<Id, Id>& defMap) {
    if (!aliveInstance(src, inst)) return kNone;
    // The instance first: a file's first instance is what a prefab places.
    dst.instances.push_back(src.instances[inst]);
    const Id at = (Id)dst.instances.size() - 1;
    const Id nd = copyDefInto(dst, src, src.instances[inst].def, defMap);
    dst.instances[at].def = nd;
    dst.instances[at].parent = kNone;
    return at;
}

ModelState extractDefinition(const ModelState& stIn, Id def) {
    ModelState out;
    const ModelState st = closedState(stIn);
    if (def < 0 || def >= (Id)st.defs.size()) return out;
    Instance in;
    in.def = 0;
    out.instances.push_back(in);
    std::map<Id, Id> defMap;
    copyDefInto(out, st, def, defMap);
    // Only the materials used, renumbered.
    std::map<Id, Id> matMap;
    auto mapMat = [&](Id& mat) {
        if (mat < 0 || mat >= (Id)st.materials.size()) {
            mat = kNone;
            return;
        }
        auto it = matMap.find(mat);
        if (it == matMap.end()) {
            it = matMap.emplace(mat, (Id)out.materials.size()).first;
            out.materials.push_back(st.materials[mat]);
        }
        mat = it->second;
    };
    for (ComponentDef& d : out.defs) {
        for (Face& f : d.mesh.faces) {
            mapMat(f.front.material);
            mapMat(f.back.material);
        }
        for (Vertex& v : d.mesh.vertices) {
            for (PaintWeight& w : v.paint) mapMat(w.material);
            std::erase_if(v.paint, [](const PaintWeight& w) { return w.material == kNone; });
        }
    }
    return out;
}

Id importDefinition(Document& doc, const ModelState& src, Id def) {
    if (def < 0 || def >= (Id)src.defs.size()) return kNone;
    const ComponentDef& sd = src.defs[def];
    if (!sd.uid.empty())
        for (Id d = 0; d < (Id)doc.state.defs.size(); ++d)
            if (doc.state.defs[d].uid == sd.uid) return d;
    std::map<Id, Id> matMap;
    auto mapMat = [&](Id id) -> Id {
        if (id < 0 || id >= (Id)src.materials.size()) return kNone;
        auto it = matMap.find(id);
        if (it != matMap.end()) return it->second;
        Id found = kNone;
        for (Id j = 0; j < (Id)doc.state.materials.size() && found == kNone; ++j)
            if (sameMaterial(doc.state.materials[j], src.materials[id])) found = j;
        if (found == kNone) found = doc.addMaterial(src.materials[id]);
        matMap[id] = found;
        return found;
    };
    ComponentDef nd = sd;
    for (Face& f : nd.mesh.faces) {
        f.front.material = mapMat(f.front.material);
        f.back.material = mapMat(f.back.material);
    }
    for (Vertex& v : nd.mesh.vertices) {
        for (PaintWeight& w : v.paint) w.material = mapMat(w.material);
        std::erase_if(v.paint, [](const PaintWeight& w) { return w.material == kNone; });
    }
    nd.name = uniqueComponentName(doc.state, sd.name);
    if (nd.uid.empty()) nd.uid = newComponentUid();
    nd.mesh.touch();
    doc.state.defs.push_back(std::move(nd));
    const Id made = (Id)doc.state.defs.size() - 1;
    // What is inside it, recursively.
    std::map<Id, Id> instMap;
    for (Id c : childrenOf(src, def)) {
        const Id childDef = importDefinition(doc, src, src.instances[c].def);
        if (childDef == kNone) continue;
        Instance copy = src.instances[c];
        copy.def = childDef;
        copy.parent = made;
        doc.state.instances.push_back(copy);
        instMap[c] = (Id)doc.state.instances.size() - 1;
    }
    remapStateChildren(doc.state.defs[made], instMap);
    return made;
}

void openComponent(ModelState& st, Id inst) {
    if (st.editing == inst) return;
    if (!aliveInstance(st, inst)) return;
    // Only what is at the level being edited opens directly.
    while (st.editing != kNone && st.instances[inst].parent != st.activeDef()) closeComponent(st);
    if (st.instances[inst].parent != st.activeDef()) return;
    const Mat4 xf = st.instances[inst].xf;
    const Id def = st.instances[inst].def;
    bakeTransform(st.defs[def].mesh, xf);
    for (Id c : childrenOf(st, def)) st.instances[c].xf = xf * st.instances[c].xf;
    if (st.editing != kNone) st.outerOpen.push_back(st.editing);
    st.editing = inst;
}

void closeComponent(ModelState& st) {
    if (st.editing == kNone) return;
    const Instance I = st.instances[st.editing];
    const Mat4 inv = I.xf.inverted();
    bakeTransform(st.defs[I.def].mesh, inv);
    for (Id c : childrenOf(st, I.def)) st.instances[c].xf = inv * st.instances[c].xf;
    if (st.outerOpen.empty()) {
        st.editing = kNone;
    } else {
        st.editing = st.outerOpen.back();
        st.outerOpen.pop_back();
    }
}

void closeAllComponents(ModelState& st) {
    while (st.editing != kNone) closeComponent(st);
}

ModelState closedState(const ModelState& st) {
    ModelState c = st;
    closeAllComponents(c);
    return c;
}

Mesh flattenModel(const ModelState& st) {
    Mesh out = st.mesh;
    for (const PlacedInstance& p : placedInstances(st)) appendMesh(out, st.defs[p.def].mesh, p.xf);
    return out;
}

} // namespace gs
