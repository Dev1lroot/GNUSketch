// SPDX-License-Identifier: GPL-3.0-or-later
// Forma plugin: Packing Solver - between the model and the 2D solver.
//
// The selected faces (in one plane) are the area; the selected components are
// what goes onto it, each seen from above the area as its footprint. Copies
// stand on the area's side the originals are on (the upper side when that says
// nothing), their lowest point on it, turned only about the area's normal. What
// else stands on the area up to the height of the tallest item (walls,
// furniture, other components) is kept clear of; what is higher (a table top)
// is not, so items go under it.
#pragma once

#include "PackingSolver.h"
#include "core/Document.h"

#include <string>
#include <vector>

namespace forma::packing {

struct SceneOptions {
    enum Footprint { Exact, Hull, Box };
    Footprint footprint = Exact;
    double turnStep = 90;    // degrees between the turns allowed; 0: not turned
    bool alignToArea = true; // turn the items' sides along the area's edges first
    bool avoidExisting = true;
    double spacing = 0, margin = 0; // meters
    int maxCount = 0;        // of each item (0: as many as fit)
};

struct SceneItem {
    Id instance = kNone; // the original (the first selected of its definition)
    Id def = kNone;
    std::string name;
    Vec2 centre;         // its footprint's middle, in the plane's coordinates
    double lift = 0;     // how far its lowest point is from the plane (along `normal`)
    double height = 0;
    double width = 0, depth = 0; // footprint size
    bool simplified = false; // too detailed: its convex hull was used
};

struct Scene {
    std::string error;   // empty: usable
    std::vector<std::string> notes;
    Vec3 origin, axisX, axisY, normal; // the area's plane: axisX x axisY = normal (towards the items)
    double areaSize = 0; // square meters
    int faces = 0;
    std::vector<SceneItem> items;  // in the order of problem.items
    std::set<Id> originals;        // every selected instance
    Problem problem;
};

// What is selected (faces and components at the level being edited), ready to solve.
Scene buildScene(const Document& doc, const SceneOptions& opt);

// The layout as new instances (copies of the originals' definitions and their
// other settings), replacing the originals unless `keepOriginals`. Selects the
// new ones. Opens no undo step. Returns their ids.
std::vector<Id> applyLayout(Document& doc, const Scene& scene, const Layout& layout, bool keepOriginals);

// The transform that takes item i from where its original is to `pl`.
Mat4 placementMotion(const Scene& scene, const Placement& pl);

} // namespace forma::packing
