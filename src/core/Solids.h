// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - solid tools: boolean operations on closed components.
//
// A solid is a mesh whose faces close up: every edge of a face bounds exactly
// two faces. Faces stay n-gons through the operations; the intersection
// curves split faces and the pieces of each solid are kept or dropped by
// whether they lie inside the other one.
#pragma once

#include "Document.h"

#include <string>

namespace gs {

enum class SolidOp {
    Union,     // both solids merged into one, inner faces removed
    Intersect, // only the volume the two share
    Subtract,  // second minus first; the first (the cutter) is deleted
    Trim,      // second minus first; the first is kept
    Split,     // first minus second, the shared part and second minus first
};

const char* solidOpName(SolidOp op);

// True if every edge of every face bounds exactly two faces and the faces can
// be oriented consistently. Edges without faces are ignored. `why` explains a
// failure.
bool isSolid(const Mesh& m, std::string* why = nullptr);
// Enclosed volume of a solid (model units cubed), 0 if it is not a solid.
double solidVolume(const Mesh& m);

enum class BoolOp { Union, Intersect, Difference };

// Boolean of two solids given in the same coordinates (Difference = a - b).
// On failure `out` is left empty and false is returned with a message.
bool booleanMeshes(const Mesh& a, const Mesh& b, BoolOp op, Mesh& out, std::string* error = nullptr);
// a - b, a and b, b - a in one pass.
bool splitMeshes(const Mesh& a, const Mesh& b, Mesh& aOnly, Mesh& both, Mesh& bOnly, std::string* error = nullptr);

bool instanceIsSolid(const ModelState& st, Id inst, std::string* why = nullptr);

// Applies a solid tool to two component instances (closing an open component
// first). Results replace the instances in place, keeping their transform:
// Union/Intersect/Subtract/Trim leave the result in `second`; Split leaves
// first-only in `first`, second-only in `second` and the shared part in a new
// instance. Definitions shared with other instances are copied, not changed.
// Resulting instances go to `result`. On failure nothing is changed.
bool solidOperation(ModelState& st, SolidOp op, Id first, Id second, Selection* result = nullptr,
                    std::string* error = nullptr);

} // namespace gs
