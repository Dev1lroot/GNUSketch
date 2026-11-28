// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - Gear Notches: teeth (or notches) along outlines and lines.
//
// Works on whatever is selected in a flat outline: a face (its outline, and
// its holes if asked), a closed curve such as a circle's edges, a rectangle's
// sides, a single line. The selected edges are chained into paths; each path
// gets a row of teeth of the chosen profile, evenly spaced. Straight sides
// meeting at a corner each get their own whole number of teeth so the corners
// stay clean; on curves the teeth stand square to the curve (radial on a
// circle). Faces along the path keep their materials and orientation; a face
// outline with teeth outward grows, with notches inward it is cut into.
//
// Paths on the outline of a solid (an edge between faces at an angle) are not
// changed: notch the flat shape first, then push/pull it.
#pragma once

#include "Mesh.h"

#include <set>
#include <string>
#include <vector>

namespace forma {

struct GearNotchParams {
    enum Profile { Square, Trapezoid, Triangle, Sawtooth, Rounded, Gear };
    Profile profile = Trapezoid;
    bool byCount = true;    // teeth per path (else one tooth every `pitch`)
    int count = 24;         // teeth along each path (closed or open)
    double pitch = 0.1;     // distance from tooth to tooth, meters
    double depth = 0.05;    // height of a tooth (depth of a notch), meters
    double toothWidth = 0.5; // share of each tooth's space taken by the tooth at its base (0..1)
    double tipWidth = 0.5;  // tip width as a share of the base width (Trapezoid, Gear)
    bool inward = false;    // notches cut into the shape instead of teeth standing out
    double phase = 0;       // shifts the teeth along a closed curve, in teeth (0..1)
    double cornerAngle = 30; // a turn sharper than this (degrees) is a corner
    bool holes = false;     // selected faces: their holes get teeth too
    bool flip = false;      // lines bounding no face: teeth on the other side
    int curveSegments = 6;  // pieces of a rounded or curved flank
    // Standard teeth (meters, 0 = off): the module sets the tooth size, so the
    // result meshes with any gear or rack of the same module and pressure
    // angle. The path is the pitch line (where meshing gears touch); teeth are
    // ISO 53 rack teeth (spacing π·module, module above the line, 1.25 below),
    // and spacing, depth, profile and widths above are not used. A circle
    // with the teeth outward becomes an exact involute spur gear; with
    // fitCircles its radius changes to the nearest whole number of teeth
    // (pitch diameter = module × teeth), otherwise the spacing stretches.
    double module = 0;
    double pressureAngle = 20; // degrees
    bool fitCircles = true;
    // Share of the depth that lies under the path, the rest above it (0: the
    // path is the root of the teeth). Set from the module when that is on.
    double baseline = 0;
};

struct GearNotchResult {
    int paths = 0, teeth = 0;
    int circles = 0;           // circles made into involute gears (module on)
    double pitchDiameter = 0;  // of the last one, meters
    std::set<Id> faces, edges; // the faces rebuilt and the edges of the new outlines
};

// One path's new outline, computed without changing a mesh. pts are the path's
// points in order (closed: last joins first). `side` at each segment is the
// unit direction teeth grow towards (in the path's plane, square to the
// segment). Returns the points with their position s along the path; teeth
// counted in *teeth. Closed paths return points with 0 <= s < length.
struct NotchPoint {
    Vec3 p;
    double s = 0;
};
std::vector<NotchPoint> notchPath(const std::vector<Vec3>& pts, bool closed, const std::vector<Vec3>& side,
                                  const GearNotchParams& p, int* teeth = nullptr);

// Adds teeth along the selected faces' outlines and the selected edges.
// On failure the mesh is unchanged.
bool gearNotch(Mesh& m, const std::set<Id>& faces, const std::set<Id>& edges, const GearNotchParams& p,
               GearNotchResult* out = nullptr, std::string* error = nullptr);

} // namespace forma
