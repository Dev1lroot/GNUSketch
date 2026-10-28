// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - inference engine: snaps the cursor to endpoints, midpoints,
// edges, faces, axis directions and "from point" alignments. Components
// (other than the moved ones) offer the same snaps as loose geometry.
#pragma once

#include "core/Document.h"
#include "render/Camera.h"
#include "render/Renderer.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace gs {

struct InferenceResult {
    enum Type {
        None, Free, Ground, Endpoint, Midpoint, OnEdge, OnFace, Origin, AxisX, AxisY, AxisZ,
        Locked, FromPoint, OnGuide, GuidePoint, Intersection, OnPlane,
        AlongEdge,     // from the reference point along an edge there (or its extension)
        Parallel,      // parallel to the last hovered edge
        Perpendicular, // perpendicular to the last hovered edge
        FaceNormal     // perpendicular to the last hovered face
    };
    Type type = None;
    Vec3 p;
    Id vertex = kNone, edge = kNone, face = kNone; // loose geometry only
    // The edge snapped to (also set for edges of components).
    bool hasEdge = false;
    Vec3 edgeA, edgeB;
    bool hasLine = false; // dotted inference line to draw
    Vec3 lineA, lineB;
    Color lineColor;
    Vec3 dir;             // direction inferred (axis / edge) for shift-locking
    bool hasDir = false;
    std::string label() const;
    bool isSnap() const { return type == Endpoint || type == Midpoint || type == Origin || type == GuidePoint || type == Intersection; }
};

struct PickResult {
    Id face = kNone, edge = kNone, vertex = kNone;
    Id instance = kNone; // component instance (top level only)
    Vec3 p;
    double t = 1e300;
};

class InferenceEngine {
public:
    struct Options {
        std::optional<Vec3> ref;     // reference point (start of the line, base point...)
        std::optional<Vec3> lockDir; // locked direction through ref
        int lockAxis = -1;           // 0/1/2 when lockDir is a world axis
        std::optional<Plane> plane;  // constrain result to this plane
        const std::set<Id>* excludeVerts = nullptr; // geometry being moved
        bool axes = true;            // axis inference from ref
        double snapPx = 10.0;
    };

    InferenceEngine(const Document& doc, const Camera& cam) : doc_(doc), cam_(cam) {}

    InferenceResult infer(double px, double py, const Options& opt);
    PickResult pick(double px, double py, double edgeTolPx = 6.0, const std::set<Id>* excludeVerts = nullptr) const;
    Id pickFace(double px, double py, double* tOut = nullptr, const std::set<Id>* excludeVerts = nullptr) const;
    Id pickEdge(double px, double py, double tolPx, const std::set<Id>* excludeVerts = nullptr) const;
    int pickGuide(double px, double py, double tolPx) const;
    // Closest component instance under the cursor (none while one is open);
    // normalOut receives the world normal of the face hit (zero for an edge).
    Id pickInstance(double px, double py, double* tOut = nullptr, Vec3* normalOut = nullptr) const;
    // Instances being moved by a tool: not picked, snapped to or occluding.
    std::set<Id> ignoredInstances;
    void addPreview(const InferenceResult& r, Preview& pv) const;
    void resetMemory() {
        remembered_.reset();
        rememberedEdge_.reset();
        rememberedNormal_.reset();
    }
    double pixelDistance(const Vec3& p, double px, double py) const;

private:
    struct Segment {
        Vec3 a, b;
    };
    // Closest visible edge near the cursor, loose or of a component.
    struct EdgeHit {
        Segment seg;
        Id edge = kNone; // loose edge id (kNone for a component edge)
        Vec3 p;          // point on the edge under the cursor
    };
    bool pickAnyEdge(double px, double py, double tolPx, const std::set<Id>* excludeVerts, double tFace, EdgeHit& hit) const;
    // World-space hard edges of the instances that offer snaps (cached).
    const std::vector<Segment>& instanceEdges(Id inst) const;
    bool snapsTo(Id inst) const;
    bool excluded(Id v, const Options& o) const { return o.excludeVerts && o.excludeVerts->count(v); }
    // pickEdge with the nearest face hit distance already known (for occlusion).
    Id pickEdgeT(double px, double py, double tolPx, const std::set<Id>* excludeVerts, double tFace) const;
    Vec3 freePoint(const Ray& r, const Options& o) const;

    const Document& doc_;
    const Camera& cam_;
    std::optional<Vec3> remembered_; // last hovered endpoint for "from point" inference
    std::optional<Segment> rememberedEdge_; // last hovered edge for "parallel" inference
    std::optional<Vec3> rememberedNormal_;  // last hovered face for "perpendicular to face"
    struct CachedEdges {
        uint64_t revision = 0;
        Mat4 xf;
        std::vector<Segment> edges;
    };
    mutable std::map<Id, CachedEdges> instEdges_;
};

} // namespace gs
