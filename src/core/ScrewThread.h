// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Screw Thread: a helical thread carved into, or raised out of,
// a round surface.
//
// Works on the side of a cylinder: a rod or a beam (thread on the outside, a
// bolt) or a round hole through a part (thread on the inside, a nut). The
// selected faces must lie on one cylinder and go all the way round it; faces
// at its ends that stand square to the axis (a rod's caps, the faces a hole
// goes through) are reshaped to meet the thread, so a closed solid stays
// closed. Selected caps are allowed and kept as caps.
//
// "Carve in" cuts grooves into the material (the surface stays where the
// thread's crest is), "raise out" stands ridges out of it (the surface stays
// where the thread's root is). Both go by the side the faces face, so the same
// setting cuts into a rod or into the wall around a nut's hole.
#pragma once

#include "Mesh.h"

#include <set>
#include <string>

namespace gs {

struct ScrewThreadParams {
    enum Profile { V, Metric, Square, Trapezoid, Buttress, Rounded };
    Profile profile = Metric;
    double pitch = 0.01;   // distance from one turn of the thread to the next, meters
    double depth = 0.006;  // from root to crest, meters
    int starts = 1;        // threads wound side by side (lead = starts x pitch)
    bool leftHand = false;
    bool inward = true;    // carve grooves in (else raise ridges out)
    int segments = 48;     // pieces around the circumference
    int roundSteps = 8;    // pieces of one turn of a Rounded profile
};

struct ScrewThreadResult {
    std::set<Id> faces;    // the threaded surface
    double radius = 0;     // radius of the selected cylinder
    double length = 0;     // its length along the axis
    double turns = 0;      // turns of each thread
};

// Replaces the selected cylinder surface with a threaded one. On failure the
// mesh is unchanged.
bool screwThread(Mesh& m, const std::set<Id>& faces, const ScrewThreadParams& p, ScrewThreadResult* out = nullptr,
                 std::string* error = nullptr);

} // namespace gs
