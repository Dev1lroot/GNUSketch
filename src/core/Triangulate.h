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

} // namespace gs
