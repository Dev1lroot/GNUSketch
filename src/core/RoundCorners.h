// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Round Corners: replaces sharp edges/corners with circular arcs.
#pragma once

#include "Mesh.h"

#include <set>
#include <string>

namespace gs {

// Rounds the given edges with `radius` (model units) using `segments` facets.
//  * An edge between two non-coplanar faces becomes a rounded strip (3D
//    fillet). Its ends are blended where three faces meet: cut by the end face
//    (one rounded edge), mitred (two rounded edges) or closed with a spherical
//    patch (three rounded edges).
//  * Two selected edges meeting at a vertex used by nothing else (corners of a
//    rectangle or polyline) are joined by a tangent arc (2D fillet).
// On failure the mesh is left untouched and false is returned with a message.
bool roundCorners(Mesh& m, const std::set<Id>& edges, double radius, int segments, std::string* error = nullptr);

} // namespace gs
