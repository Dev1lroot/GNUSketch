// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Slicer: cuts a solid into pieces that fit a box (a 3D printer's
// build volume), each piece a component of its own, optionally with
// connectors on the cuts so the glued parts line up and hold better.
//
// The grid starts at the model's lowest corner. With `equal`, every axis is
// divided into equal parts no larger than the given size (all pieces alike);
// otherwise into parts of exactly that size and a remainder.
//
// Connectors go on the inner cuts, from the lower piece (smaller x, y or z)
// into the higher one: a peg, key or rib standing out of one cut face and a
// matching socket or groove, wider and deeper by the clearance, sunk into the
// other. They are only placed where the solid has room for them and `margin`
// of wall all around; a cut too small for any gets none.
#pragma once

#include "Document.h"

#include <string>

namespace gs {

struct SlicerParams {
    Vec3 size = Vec3(0.2, 0.2, 0.2); // largest piece along x (W), y (L), z (H), meters
    bool equal = true;
    enum Connector { None, RoundPegs, SquareKeys, Ribs };
    Connector connector = None;
    double connectorSize = 0.008;   // peg diameter, key or rib width
    double connectorLength = 0.006; // how far it stands out of the cut (and the socket's depth)
    double clearance = 0.0002;      // the socket is this much larger all round (printing tolerance)
    double margin = 0.003;          // solid wall kept around a connector
    int perCut = 4;                 // most connectors on one cut
    double spread = 0;              // gap left between the pieces (moves them apart), meters
};

struct SlicerResult {
    int pieces = 0, connectors = 0;
    int nx = 1, ny = 1, nz = 1; // grid cells along each axis
    Vec3 cell;                  // the cell size used
    Selection created;          // the new component instances
};

// The pieces of the selection (selected faces of the mesh being edited, which
// must form closed solids, and selected components, whose solids are taken in
// world coordinates). The selection is replaced by one component per piece.
// On failure nothing is changed.
bool sliceSelection(ModelState& st, Selection& sel, const SlicerParams& p, SlicerResult* out = nullptr,
                    std::string* error = nullptr);

// The grid sliceSelection would use for these bounds: cells per axis and cell size.
void slicerGrid(const Vec3& mn, const Vec3& mx, const SlicerParams& p, int n[3], Vec3& cell);

// Point inside a closed mesh (ray parity, voted over three directions).
bool pointInSolid(const Mesh& m, const Vec3& p);

} // namespace gs
