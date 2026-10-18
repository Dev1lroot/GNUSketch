// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Follow Me: sweeps a profile face along a path of edges.
#pragma once

#include "Mesh.h"

#include <set>
#include <string>
#include <vector>

namespace gs {

// Edges connected to `start` through vertices that join exactly two edges
// (a line chain, arc or circle), in path order.
std::vector<Id> edgeChain(const Mesh& m, Id start);

// Sweeps face `profile` along the connected, unbranched chain `path`. The
// profile keeps its position relative to the path and is carried along it with
// mitred joints, so profiles drawn at the start of a line make pipes and beams
// and profiles next to a closed outline make mouldings and lathed shapes.
// Open paths get end caps (the profile face itself is the start cap); closed
// paths replace the profile face. Edges of the profile are ignored in the path.
// On failure the mesh is left untouched and false is returned with a message.
bool followMe(Mesh& m, Id profile, const std::set<Id>& path, std::string* error = nullptr);

} // namespace gs
