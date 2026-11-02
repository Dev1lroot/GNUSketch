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

// Runs an effect on the selection: the selected faces of the active mesh, then
// the whole geometry of each selected component in world coordinates. A component
// whose definition is shared with instances outside the selection gets its own
// copy first. Selected faces are replaced by the result.
using EffectFn = std::function<bool(Mesh&, const std::set<Id>&, EffectResult*, std::string*)>;
bool applyEffect(ModelState& st, Selection& sel, const EffectFn& fn, bool* limited = nullptr, std::string* error = nullptr);
// Faces an effect on this selection works on.
int effectFaceCount(const ModelState& st, const Selection& sel);

} // namespace gs
