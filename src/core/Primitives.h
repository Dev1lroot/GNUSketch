// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - primitive generator: spheres, cylinders, tori, the Platonic
// solids and the like, built as closed n-gon meshes (quads stay quads,
// pentagons stay pentagons). Curved surfaces get soft, smooth edges.
#pragma once

#include "Mesh.h"

#include <string>

namespace gs {

enum class PrimitiveKind {
    Box,
    Sphere,       // latitude/longitude bands: quads, triangles at the poles
    GeoSphere,    // subdivided icosahedron: triangles only
    Cylinder,
    Cone,         // a frustum when the top radius is above zero
    Torus,
    Tetrahedron,
    Octahedron,
    Dodecahedron,
    Icosahedron,
};
constexpr int kPrimitiveKindCount = 10;

const char* primitiveName(PrimitiveKind k);

struct PrimitiveParams {
    PrimitiveKind kind = PrimitiveKind::Sphere;
    // Sphere, geodesic sphere, Platonic solids: circumradius.
    // Cylinder, cone: base radius. Torus: distance from centre to the tube's middle.
    double radius = 0.5;
    double radius2 = 0.1;   // cone: top radius (0 = pointed); torus: tube radius
    double width = 1, depth = 1, height = 1; // box: all three; cylinder, cone: height
    int segments = 24;      // around the Z axis (sphere, cylinder, cone, torus)
    int rings = 12;         // sphere: latitude bands; torus: segments around the tube
    int frequency = 2;      // geodesic sphere: each icosahedron edge is cut into this many
    bool smooth = true;     // soften the edges inside curved surfaces
};

// Which parameters a kind uses (for showing only the relevant fields).
struct PrimitiveFields {
    bool radius = false, radius2 = false, width = false, depth = false, height = false;
    bool segments = false, rings = false, frequency = false, smooth = false;
};
PrimitiveFields primitiveFields(PrimitiveKind k);

// Builds the primitive into `out` (which should be empty): it stands on the
// ground plane (lowest point at z = 0), centred on the Z axis, with every face
// pointing outwards. Returns false (and explains in `err`) for bad parameters.
bool buildPrimitive(const PrimitiveParams& p, Mesh& out, std::string* err = nullptr);

} // namespace gs
