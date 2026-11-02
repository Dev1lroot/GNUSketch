// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - polygon-with-holes triangulation (used only for GPU display and
// triangle-based exporters; the model itself keeps true n-gon faces).
#pragma once

#include "Math.h"

#include <array>
#include <vector>

namespace gs {

// loops[0] is the outer boundary, loops[1..] are holes. Orientation of the input
// does not matter. Returned triangles index into the concatenation of all loops
// (loop 0 points first, then loop 1, ...) and are wound counter-clockwise.
std::vector<std::array<int, 3>> triangulatePolygon(const std::vector<std::vector<Vec2>>& loops);

// Collinear boundary vertices produce zero-area ears; flips them away against
// their neighbours so no slit opens when the vertices move later. `flat` holds
// the points of all loops, in the indices the triangles use.
void flipDegenerateTriangles(const std::vector<Vec2>& flat, std::vector<std::array<int, 3>>& tris);

} // namespace gs
