// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - boundary-representation mesh with SketchUp-like semantics:
//  * edges are first-class, faces are planar n-gons (outer loop + holes),
//  * drawing an edge splits other edges at intersections and automatically
//    creates faces for every closed planar loop (and splits/merges faces),
//  * nothing is triangulated in the model.
#pragma once

#include "Material.h"
#include "Math.h"

#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace gs {

using Id = int32_t;
constexpr Id kNone = -1;

// Process-wide revision counter: a revision number identifies one mesh state,
// so caches keyed by it stay valid across copies, undo and other meshes.
uint64_t nextMeshRevision();

// Planar texture mapping: uv = ((p - origin) . u, (p - origin) . v).
// u and v lie in the face plane and are pre-scaled (1 / tile size).
struct TexMap {
    bool valid = false;
    Vec3 origin, u, v;
};

struct FaceSide {
    Id material = kNone;
    TexMap tex; // explicit mapping (invalid = default world-aligned projection)
};

struct Vertex {
    Vec3 p;
    bool alive = true;
};

struct Edge {
    Id v[2] = {kNone, kNone};
    bool alive = true;
    bool soft = false;   // soft edges are not drawn and are "smoothed" by default
    bool smooth = false; // smooth shading across this edge
    bool hidden = false;
    Id curve = kNone;    // edges of the same arc/circle share a curve id
    Id other(Id vtx) const { return v[0] == vtx ? v[1] : v[0]; }
    bool has(Id vtx) const { return v[0] == vtx || v[1] == vtx; }
};

struct Face {
    std::vector<Id> outer;              // CCW around normal
    std::vector<std::vector<Id>> holes; // CW around normal
    Vec3 normal;                        // unit normal (front side)
    bool alive = true;
    bool hidden = false;
    FaceSide front, back;
};

// A face's geometry captured independently of vertex ids (used to carry
// materials over when faces are split or merged).
struct FaceSnapshot {
    std::vector<std::vector<Vec3>> loops; // outer first
    Vec3 normal;
    FaceSide front, back;
    bool hidden = false;
    double area = 0;
};

class Mesh {
public:
    std::vector<Vertex> vertices;
    std::vector<Edge> edges;
    std::vector<Face> faces;

    // Incremented on every change; renderers use it to invalidate caches.
    uint64_t revision = nextMeshRevision();
    // Camera position; new faces without other orientation cues face the camera.
    Vec3 viewHint = Vec3(10, -10, 10);
    // New faces on the ground plane face down (SketchUp convention) instead of the camera.
    bool groundFacesDown = true;

    // ---- queries -------------------------------------------------------------
    const Vec3& pos(Id v) const { return vertices[v].p; }
    Id findVertex(const Vec3& p, double tol = kTol) const;
    Id findEdge(Id a, Id b) const;
    const std::vector<Id>& vertexEdges(Id v) const;
    const std::vector<Id>& edgeFaces(Id e) const;
    std::vector<Id> faceEdges(Id f) const;
    std::vector<Id> faceVertices(Id f) const; // all loops
    Plane facePlane(Id f) const;
    double faceArea(Id f) const;
    double edgeLength(Id e) const;
    Vec3 faceInteriorPoint(Id f) const;
    bool pointInFace(Id f, const Vec3& p, bool includeBoundary = true) const;
    bool rayFace(Id f, const Ray& r, double& t) const;
    // +1 if face f traverses a->b, -1 if b->a, 0 if not at all.
    int faceEdgeDirection(Id f, Id a, Id b) const;
    bool isFreeFace(Id f) const; // no boundary edge shared with another face
    int aliveVertexCount() const;
    int aliveEdgeCount() const;
    int aliveFaceCount() const;
    void bounds(Vec3& mn, Vec3& mx, bool& any) const;
    FaceSnapshot snapshot(Id f) const;

    // ---- low level construction (no automatic intersection / face finding) ----
    Id addVertex(const Vec3& p);    // merges with an existing vertex within kTol
    Id addVertexRaw(const Vec3& p); // always creates
    Id addEdgeRaw(Id a, Id b, Id curve = kNone); // returns existing edge if present
    // Creates a face. Loops are re-oriented so that the outer loop is CCW around
    // the resulting normal; normalHint (if non-zero) selects which side is front.
    Id addFaceRaw(const std::vector<Id>& outer, const std::vector<std::vector<Id>>& holes,
                  const Vec3& normalHint = Vec3());
    Id newCurveId() { return nextCurve_++; }

    // ---- SketchUp-like editing -------------------------------------------------
    // Draws a line: welds endpoints to existing vertices, splits crossing edges,
    // then finds/splits faces. Returns ids of the edges that make up the line.
    std::vector<Id> drawLine(const Vec3& a, const Vec3& b, Id curve = kNone);
    // Draws several segments with a single face-inference pass at the end.
    std::vector<Id> drawSegments(const std::vector<std::pair<Vec3, Vec3>>& segs, Id curve = kNone);
    std::vector<Id> drawPolyline(const std::vector<Vec3>& pts, bool closed, Id curve = kNone);

    void splitEdge(Id e, Id v);
    void eraseFace(Id f);
    // Erases an edge and the faces it bounds. With heal=true, two coplanar faces
    // sharing the edge are merged into one instead (SketchUp eraser behaviour).
    void eraseEdge(Id e, bool heal = true);
    void removeOrphanVertices();
    void reverseFace(Id f);

    // Push/pull a face along its normal by dist. keepOriginal leaves the starting
    // face in place (Ctrl modifier). Returns the moved face id (or kNone).
    Id pushPull(Id f, double dist, bool keepOriginal = false);

    // Applies a transform to a set of vertices, keeps texture mappings of fully
    // transformed faces attached, refreshes normals and welds coincident vertices.
    void transformVertices(const std::set<Id>& verts, const Mat4& m, bool weldAfter = true);
    // Duplicates the given faces/edges (and their vertices). Returns new ids.
    void duplicate(const std::set<Id>& faceIds, const std::set<Id>& edgeIds,
                   std::set<Id>& newFaces, std::set<Id>& newEdges, std::set<Id>& newVerts);

    // Merges vertices within tolerance of each other (only those in `candidates`,
    // or all vertices if empty), fixing up edges and faces.
    void weld(const std::set<Id>& candidates = {});
    // Merges two collinear edges meeting at v when nothing else uses v.
    bool mergeCollinearAt(Id v);
    // Runs face inference for the given new edges (planes derived from them).
    void inferFaces(const std::vector<Id>& newEdges);
    void recomputeNormal(Id f);

    // Removes dead entities and renumbers. Returns old->new vertex/edge/face maps.
    void compact(std::vector<Id>* vmap = nullptr, std::vector<Id>* emap = nullptr,
                 std::vector<Id>* fmap = nullptr);

    void touch() { revision = nextMeshRevision(); adjDirty_ = true; }
    void markGeometryMoved() { revision = nextMeshRevision(); gridDirty_ = true; }

    struct Region {
        std::vector<Id> outer;
        std::vector<std::vector<Id>> holes;
        double area = 0;
    };
    // Bounded regions of the planar arrangement formed by `planeEdges`.
    std::vector<Region> computeRegions(const Plane& P, const std::vector<Id>& planeEdges) const;

private:
    void insertSegment(const Vec3& a, const Vec3& b, Id curve, std::vector<Id>& created, std::vector<Id>& touched);
    void splitEdgesThrough(Id v);
    void rebuildPlane(const Plane& P, const std::set<Id>& newEdges, std::vector<FaceSnapshot> extraDirty);
    Vec3 chooseNormal(const std::vector<Id>& outerCCW, const Plane& P) const;
    Vec3 regionInteriorPoint(const Plane& P, const Region& r) const;
    static bool snapshotContains(const FaceSnapshot& s, const Vec3& p);
    void ensureAdjacency() const;
    bool adjacencyInSync() const {
        return !adjDirty_ && vEdges_.size() == vertices.size() && eFaces_.size() == edges.size();
    }
    void ensureGrid() const;
    void gridInsert(Id v) const;
    void edgeMapInsert(Id e);
    void edgeMapErase(Id e);
    static uint64_t edgeKey(Id a, Id b) {
        if (a > b) std::swap(a, b);
        return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
    }

    std::unordered_map<uint64_t, Id> edgeMap_;
    mutable std::vector<std::vector<Id>> vEdges_, eFaces_;
    mutable bool adjDirty_ = true;
    struct CellKey {
        int64_t x, y, z;
        bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct CellHash {
        size_t operator()(const CellKey& k) const {
            return size_t(k.x * 73856093LL) ^ size_t(k.y * 19349663LL) ^ size_t(k.z * 83492791LL);
        }
    };
    mutable std::unordered_map<CellKey, std::vector<Id>, CellHash> grid_;
    mutable bool gridDirty_ = true;
    Id nextCurve_ = 1;

    friend struct MeshSerializer;
public:
    void setNextCurve(Id c) { nextCurve_ = c; }
    Id nextCurve() const { return nextCurve_; }
};

// Texture coordinate for a point on a face side (default world-aligned
// projection unless the side has an explicit mapping).
Vec2 faceTexCoord(const Face& f, bool frontSide, const Vec3& p, const Material* m);
// Explicit mapping that stretches one copy of the texture over the face extents.
TexMap fitTextureToFace(const Mesh& mesh, Id f, bool frontSide);

} // namespace gs
