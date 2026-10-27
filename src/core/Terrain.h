// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - terrain sculpting (Mountain / Pit / Flatten brushes).
//
// Faces under the brush are converted into a triangle mesh joined by soft,
// smooth edges; triangles are bisected (longest edge first) whenever an edge
// gets longer than the brush's detail length, so the surface stays smooth
// while it is pulled. Vertices shared with untouched n-gon faces stay put, so
// those faces remain planar.
#pragma once

#include "Mesh.h"

#include <algorithm>
#include <vector>

namespace gs {

enum class TerrainMode { Raise, Lower, Flatten };

struct TerrainBrush {
    double radius = 1.0;   // meters
    double force = 0.10;   // meters per second of holding, at the brush centre
    double hardness = 0.5; // 0 = soft bell, 1 = flat top with a sharp rim
    // Target edge length of the generated triangles.
    double detail() const { return std::max(radius / 8.0, 1e-3); }
};

// Strength of the brush (0..1) at distance d from its centre.
double terrainFalloff(double d, double radius, double hardness);

// Splits face f into triangles joined by soft, smooth edges (materials,
// texture mappings and the front side carry over). f itself becomes the first
// triangle. Returns the triangles; a triangle face returns {f}.
std::vector<Id> triangulateFace(Mesh& m, Id f);

// Brush direction at a surface point: the area-weighted normal of the faces
// under the brush, on the side of `toViewer`.
Vec3 terrainNormal(const Mesh& m, const Vec3& center, Id face, const Vec3& toViewer, double radius);
// The vertical axis on the side of n (up for horizontal or upright surfaces).
inline Vec3 terrainVertical(const Vec3& n) { return n.z < -1e-9 ? Vec3(0, 0, -1) : Vec3(0, 0, 1); }

// One press-and-hold stroke. The direction (and the Flatten height) are taken
// where the stroke starts and kept while the brush is dragged around.
class TerrainStroke {
public:
    TerrainStroke(TerrainMode mode, const TerrainBrush& brush) : mode_(mode), brush_(brush) {}

    // Starts on surface point `hit` of `face`. False if there is nothing to sculpt.
    bool begin(Mesh& m, const Vec3& hit, Id face, const Vec3& toViewer);
    // Applies the brush centred at the surface point `center` for dt seconds.
    void apply(Mesh& m, const Vec3& center, double dt);
    // Splits edges stretched by the last application.
    void finish(Mesh& m, const Vec3& center) { refine(m, center); }

    const Vec3& direction() const { return dir_; }
    // Changes radius/force/hardness for the rest of the stroke.
    void setBrush(const TerrainBrush& b) { brush_ = b; }
    // Overrides the brush direction (e.g. locked to the vertical axis).
    void setDirection(const Vec3& d) { dir_ = d.normalized(); }
    const Vec3& origin() const { return origin_; }

private:
    void refine(Mesh& m, const Vec3& c, Id forceFace = kNone);
    void buildAdjacency(const Mesh& m);
    void shareTextureAxis(Mesh& m, Id start);
    void refineEdge(Mesh& m, Id e, int depth);
    void bisect(Mesh& m, Id e);
    Id longestEdge(const Mesh& m, Id f) const;
    bool isTri(const Mesh& m, Id f) const;

    TerrainMode mode_;
    TerrainBrush brush_;
    Vec3 dir_ = Vec3(0, 0, 1), origin_;
    std::vector<std::vector<Id>> edgeFaces_;
    int splitBudget_ = 0;
};

} // namespace gs
