// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - inference engine: snaps the cursor to endpoints, midpoints,
// edges, faces, axis directions and "from point" alignments.
#pragma once

#include "core/Document.h"
#include "render/Camera.h"
#include "render/Renderer.h"

#include <optional>
#include <set>
#include <string>

namespace gs {

struct InferenceResult {
    enum Type {
        None, Free, Ground, Endpoint, Midpoint, OnEdge, OnFace, Origin, AxisX, AxisY, AxisZ,
        Locked, FromPoint, OnGuide, GuidePoint, Intersection, OnPlane
    };
    Type type = None;
    Vec3 p;
    Id vertex = kNone, edge = kNone, face = kNone;
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
    void addPreview(const InferenceResult& r, Preview& pv) const;
    void resetMemory() { remembered_.reset(); }
    double pixelDistance(const Vec3& p, double px, double py) const;

private:
    bool excluded(Id v, const Options& o) const { return o.excludeVerts && o.excludeVerts->count(v); }
    // pickEdge with the nearest face hit distance already known (for occlusion).
    Id pickEdgeT(double px, double py, double tolPx, const std::set<Id>* excludeVerts, double tFace) const;
    Vec3 freePoint(const Ray& r, const Options& o) const;

    const Document& doc_;
    const Camera& cam_;
    std::optional<Vec3> remembered_; // last hovered endpoint for "from point" inference
};

} // namespace gs
