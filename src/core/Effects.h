// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - effects: whole-surface operations on the selected faces.
//
// Every effect takes the faces out of the mesh as a shared-vertex triangle
// surface, works on that and writes polygons back. Vertices shared with faces
// outside the selection stay put, and the edges shared with those faces only
// gain vertices on their straight line, so the rest of the model is unchanged
// and stays connected.
//
// Faces produced by Noise, Blur and Subdivide are triangles and therefore
// planar. The cells of Remesh and the polygons of Merge Flat Faces are n-gons:
// planar on flat surfaces, slightly warped where a cell lies across a curved one.
#pragma once

#include "Document.h"

#include <functional>
#include <set>
#include <string>

namespace gs {

// A result of an effect. On failure the mesh is unchanged.
struct EffectResult {
    std::set<Id> faces;    // the new faces
    bool limited = false;  // the detail was reduced to stay within the polygon limit
    std::set<Id> edges;    // the new loose edges (effects that keep those)
};

// Largest number of triangles an effect subdivides the selection into.
constexpr int kEffectTriangleLimit = 200000;

struct NoiseParams {
    bool x = true, y = true, z = true; // axes the vertices may move along
    double amount = 0.1;   // largest distance along each axis, meters
    double scale = 0.5;    // size of the bumps, meters
    int octaves = 3;       // layers of ever finer bumps
    double detail = 0;     // longest edge kept, meters (0 = from the bump size)
    int seed = 1;
    bool smooth = true;    // soft, smooth-shaded edges
};
bool effectNoise(Mesh& m, const std::set<Id>& faces, const NoiseParams& p, EffectResult* out = nullptr,
                 std::string* error = nullptr);

struct BlurParams {
    double radius = 0.1;   // how far the softening reaches from an edge, meters
    double strength = 1.0; // 0..1
    double detail = 0;     // longest edge kept, meters (0 = from the radius)
    bool keepVolume = true; // counteracts the shrinking of plain smoothing
};
bool effectBlur(Mesh& m, const std::set<Id>& faces, const BlurParams& p, EffectResult* out = nullptr,
                std::string* error = nullptr);

struct DecimateParams {
    enum Mode { Ratio, Density };
    Mode mode = Ratio;
    double ratio = 0.5;    // triangles kept (Ratio)
    double density = 10;   // triangles per square meter (Density)
    bool mergeFlat = true; // joins the coplanar triangles left over into n-gons
};
bool effectDecimate(Mesh& m, const std::set<Id>& faces, const DecimateParams& p, EffectResult* out = nullptr,
                    std::string* error = nullptr);

// Splits edges until none is longer than 1 / edgesPerMeter.
bool effectSubdivide(Mesh& m, const std::set<Id>& faces, double edgesPerMeter, EffectResult* out = nullptr,
                     std::string* error = nullptr);
bool effectTriangulate(Mesh& m, const std::set<Id>& faces, EffectResult* out = nullptr, std::string* error = nullptr);
// Joins connected coplanar faces with the same materials into n-gons.
bool effectMergeFlat(Mesh& m, const std::set<Id>& faces, EffectResult* out = nullptr, std::string* error = nullptr);

// Joins triangles back into polygons (an imported STL, say): neighbours lying in
// one plane within `angle` degrees become one n-gon, and the points in between
// on straight outlines go. With `quads`, pairs of the triangles left over that
// bend less than `quadAngle` degrees become quads (curved surfaces).
struct DetriangulateParams {
    double angle = 0.1;
    bool quads = false;
    double quadAngle = 30;
};
bool effectDetriangulate(Mesh& m, const std::set<Id>& faces, const DetriangulateParams& p, EffectResult* out = nullptr,
                         std::string* error = nullptr);

// Makes a closed solid hollow: an inner wall `thickness` inside the surface,
// facing the cavity. Any shape and topology: the inner wall is the level set
// of the distance to the surface (on a grid of `detail` cells along the longest
// side, finer if the wall needs it), so where the solid is thinner than two
// walls it stays solid. The outer surface is not changed.
struct HollowParams {
    double thickness = 0.002;
    int detail = 96;
};
bool effectHollow(Mesh& m, const std::set<Id>& faces, const HollowParams& p, EffectResult* out = nullptr, std::string* error = nullptr);

// Leaves only the outside of the geometry: overlapping closed parts are merged
// (union), and faces that cannot be seen from outside go: inner walls, the
// walls of sealed hollows (they become solid), loose sheets. Faces seen from
// outside with their back are turned around. Gaps narrower than a grid cell
// (`detail` cells along the longest side) count as closed.
struct SolidifyParams {
    int detail = 128;
    bool mergeParts = true;
};
bool effectSolidify(Mesh& m, const std::set<Id>& faces, const SolidifyParams& p, EffectResult* out = nullptr,
                    std::string* error = nullptr);

struct RemeshParams {
    enum Shape { Triangles, Quads, Hexagons };
    Shape shape = Quads;
    double cellsPerMeter = 2;
    double angle = 0;      // rotation of the pattern, degrees
    bool smooth = true;    // smooth shading across shallow edges (edges stay visible)
};
// Rebuilds the surface as a regular pattern of cells. The surface is divided
// into regions facing about the same way, each with its own pattern, cut along
// the creases and the selection's outline.
bool effectRemesh(Mesh& m, const std::set<Id>& faces, const RemeshParams& p, EffectResult* out = nullptr,
                  std::string* error = nullptr);

struct VoxelateParams {
    double size = 0.1;     // edge of a cube, meters
    bool fill = true;      // a closed shape becomes solid, not a shell of cubes
    bool mergeFlat = false; // flat sides become single n-gons, without the grid lines
};
// Rebuilds the faces as cubes on a world-aligned grid: the outer surface of all
// cubes touched by the faces, as one closed mesh. A shape that fits the grid keeps
// its size exactly. Materials come from the nearest face. If the grid would be too
// large, the cubes get bigger (out->limited). The faces do not stay connected to
// neighbours outside the selection, as the new surface does not follow them.
bool effectVoxelate(Mesh& m, const std::set<Id>& faces, const VoxelateParams& p, EffectResult* out = nullptr,
                    std::string* error = nullptr);

// Bends space along a straight stretch, like a strip of soft metal around a
// cylinder. Along `dir`, measured from `origin` and across `axis`: what lies
// before 0 stays, 0..length curves into an arc of `angle` radians around `axis`
// (keeping its length along the middle), and what lies past `length` follows
// rigidly. Position along `axis` is unchanged, so stairs bent around the vertical
// keep their height. Positive angles turn dir towards axis x dir.
struct BendParams {
    Vec3 origin, dir, axis; // dir and axis: unit, perpendicular
    double length = 1;
    double angle = 0;
    int segments = 0;       // straight pieces of the arc (0 = one per 7.5 degrees)
    int segmentCount() const;
    // Radius of the arc along the middle line (signed; infinite-ish for no angle).
    double radius() const;
    Vec3 apply(const Vec3& p) const;
    // False if p is inside the bend beyond its centre, where it would turn inside out.
    bool fits(const Vec3& p) const;
};
// Bend from a to b: the stretch is the part of a-b across `axis`. False if a-b
// runs along the axis (nothing to bend along).
bool makeBend(const Vec3& a, const Vec3& b, const Vec3& axis, double angle, int segments, BendParams& out);
// Bends the faces, and the given edges that bound no face (null: every such edge).
bool effectBend(Mesh& m, const std::set<Id>& faces, const std::set<Id>* edges, const BendParams& p,
                EffectResult* out = nullptr, std::string* error = nullptr);
// Bends the selection: faces and loose edges, and whole components (see applyEffect).
bool applyBend(ModelState& st, Selection& sel, const BendParams& p, std::string* error = nullptr);

// Runs an effect on the selection: the selected faces of the active mesh, then
// the whole geometry of each selected component in world coordinates. A component
// whose definition is shared with instances outside the selection gets its own
// copy first. Selected faces are replaced by the result.
using EffectFn = std::function<bool(Mesh&, const std::set<Id>&, EffectResult*, std::string*)>;
bool applyEffect(ModelState& st, Selection& sel, const EffectFn& fn, bool* limited = nullptr, std::string* error = nullptr);
// Faces an effect on this selection works on.
int effectFaceCount(const ModelState& st, const Selection& sel);

} // namespace gs
