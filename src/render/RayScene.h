// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the scene for ray traced (cinematic) shadows: bounding volume
// hierarchies over the model's triangles, packed into float arrays that the
// shaders read from buffer textures (Renderer: GS_RT_GLSL).
//
// Two levels: every mesh (the loose geometry, each component definition) has
// its own BVH, built once per mesh revision; a small top level tree over the
// placed instances (with their transforms) is rebuilt every frame, so moving
// components and Game Mode entities cost nothing to update.
//
// Layout, all RGBA32F texels:
//   nodes      2 per node: (min.xyz, a) (max.xyz, count). count > 0: a leaf with
//              `count` items starting at a; count == 0: children at a and a + 1.
//   tris       3 per triangle: v0, v1 - v0, v2 - v0 (w unused).
//   instances  4 per instance: the 3 rows of its inverse transform (world to
//              mesh), then (root node, 0, 0, 0).
//   top        the instance tree, nodes as above; leaf items index `instances`.
#pragma once

#include "core/Math.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace gs {

class RayScene {
public:
    // Triangles of one mesh in its own coordinates (9 floats per triangle).
    // key identifies the mesh, revision its contents: unchanged ones are kept.
    void setMesh(const void* key, uint64_t revision, const std::vector<std::vector<float>>& triLists);
    // Forgets meshes not set since the last call (call once per frame after the setMesh calls).
    void endMeshes();
    // The placed meshes of this frame.
    void clearInstances() { inst_.clear(); }
    void addInstance(const void* key, const Mat4& xf);
    void buildTop();

    // Packed data; meshDataRevision() changes when nodes/tris did.
    const std::vector<float>& nodes() const { return nodes_; }
    const std::vector<float>& tris() const { return tris_; }
    const std::vector<float>& instances() const { return instData_; }
    const std::vector<float>& top() const { return top_; }
    uint64_t meshDataRevision() const { return dataRev_; }
    int triangleCount() const { return (int)(tris_.size() / 12); }
    bool empty() const { return instCount_ == 0; }
    // Scene size (for ray offsets).
    double radius() const { return radius_; }

private:
    struct Node {
        Vec3 mn, mx;
        int a = 0, count = 0;
    };
    struct Blas {
        uint64_t revision = 0;
        std::vector<Node> nodes;
        std::vector<std::array<Vec3, 3>> tris;
        bool used = true;
        int nodeOffset = 0, triOffset = 0;
    };
    // Median-split BVH over boxes (centroids); items are reordered through `order`.
    static void build(const std::vector<Vec3>& mn, const std::vector<Vec3>& mx, std::vector<Node>& nodes, std::vector<int>& order,
                      int leafSize);
    void pack();

    std::map<const void*, Blas> blas_;
    bool dirty_ = false;
    std::vector<float> nodes_, tris_, instData_, top_;
    uint64_t dataRev_ = 0;
    struct Inst {
        const void* key;
        Mat4 xf;
    };
    std::vector<Inst> inst_;
    double radius_ = 1;
    int instCount_ = 0;
};

} // namespace gs
