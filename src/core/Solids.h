// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - solid tools: boolean operations on closed components.
//
// A solid is a mesh whose faces close up: every edge of a face bounds exactly
// two faces. Faces stay n-gons through the operations; the intersection
// curves split faces and the pieces of each solid are kept or dropped by
// whether they lie inside the other one.
#pragma once

#include "Document.h"

#include <string>
#include <vector>

namespace forma {

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
// isSolid(m), worked out once per change of the mesh (its revision) and
// remembered: cheap to ask every frame.
bool isSolidCached(const Mesh& m);
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

// ---- Knife ------------------------------------------------------------------
// Cuts any geometry (closed solids, open surfaces, loose sheets) along a flat
// knife: the faces of `knife` lying in one plane, given in the same
// coordinates as `target`. The cut follows the knife's outline exactly: only
// what lies within the knife's faces is cut. Each separated part becomes one
// piece; a closed solid gets the cut openings closed with flat faces, so both
// parts stay solids. Fails (pieces empty) if the knife is not flat or does not
// cut the target into at least two parts.
bool knifeIsFlat(const Mesh& knife, Plane* plane = nullptr, std::string* why = nullptr);
bool knifeMesh(const Mesh& target, const Mesh& knife, std::vector<Mesh>& pieces, std::string* error = nullptr);

// Cuts component `target` with a knife: the flat component `knifeInstance`
// or, if that is kNone, face `knifeFace` of the top-level geometry together
// with the faces joined to it in its plane. The first part stays in `target`,
// the others become new components next to it; the knife is kept. Resulting
// instances go to `result`. On failure nothing is changed.
bool knifeOperation(ModelState& st, Id knifeInstance, Id knifeFace, Id target, Selection* result = nullptr,
                    std::string* error = nullptr);

} // namespace forma
