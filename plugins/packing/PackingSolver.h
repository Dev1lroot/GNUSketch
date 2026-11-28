// SPDX-License-Identifier: GPL-3.0-or-later
// Forma plugin: Packing Solver - the 2D nesting itself (no Qt, no document).
//
// Fits as many copies of one or more shapes as it can into a flat area (a
// polygon with holes, or several that share edges), none overlapping another,
// any obstacle or the area's border.
//
// How: one attempt is a Bottom-Left-Fill. The area is seen with "gravity" along
// some direction; each new copy goes to the lowest, then leftmost, place where it
// fits, found by sweeping a fine grid of positions (a cursor per shape and turn
// that never needs to go back: a place where a copy did not fit stays so as more
// are added) plus the exact places just above and right of the copy in the way,
// and is then pushed down and left until it touches. Every turn allowed competes
// for each place. Which way gravity points, which turns may be used, how the
// places are ranked and the grid's fineness make one attempt's settings; the
// solver tries the promising ones first (gravity along each main direction of the
// border, from either side), then keeps changing the settings of the best layout
// found so far one at a time, and some at random, for as many attempts or as
// long as it was given (a (1+λ) evolution strategy), the attempts of one round
// in parallel. The best layout wins: most of the first shape, then of the next...,
// then the most compact (smallest box around the copies).
//
// Overlap tests are exact for the polygons given: shapes are unions of convex
// parts, tested by separating axes, with `spacing` kept between copies and
// `margin` to the border (offsets with mitred corners). Touching is allowed.
#pragma once

#include "core/Math.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace forma::packing {

// A convex polygon, either way round; two points make a segment (a wall seen
// edge-on from above: obstacles only).
using Convex = std::vector<Vec2>;

struct Shape {
    std::vector<Convex> parts; // around its origin, the point it is placed by
};

struct ItemType {
    Shape shape;
    // Turns (radians, about the origin) it may be placed with; at least one.
    std::vector<double> angles{0.0};
    int maxCount = 0; // 0: as many as fit
};

struct Problem {
    // The area: polygons, each loops[0] the outline, loops[1..] holes. Edges that
    // two of them share (same end points) are inside the area, not its border.
    std::vector<std::vector<std::vector<Vec2>>> area;
    // Filled in this order: the first as much as possible, the next in what is left...
    std::vector<ItemType> items;
    std::vector<Convex> obstacles; // in area coordinates
    double spacing = 0; // between copies, and from obstacles
    double margin = 0;  // from the area's border
};

struct Placement {
    int item = 0;
    Vec2 pos;          // where the shape's origin goes
    double angle = 0;  // the turn, one of the item's angles
};

struct Layout {
    std::vector<Placement> placements;
    std::vector<int> counts;   // per item type
    double usedArea = 0;       // of the copies
    double spread = 0;         // area of the box around the copies, seen along the attempt's gravity
    // Better than o: more of an earlier item, then smaller spread.
    bool betterThan(const Layout& o) const;
};

struct SolveOptions {
    int attempts = 120;    // the most to try
    double seconds = 20;   // and for no longer than this (0: no limit)
    int resolution = 10;   // grid positions per smallest item size (finer finds tighter fits, slower)
    unsigned seed = 1;
    int threads = 0;       // 0: as many as the machine has
    const std::atomic<bool>* stop = nullptr; // set to stop early (the best found so far is returned)
};

// After each round, from the solving thread: attempts done, the most there will
// be, the best so far. Return false to stop.
using ProgressFn = std::function<bool(int done, int total, const Layout& best)>;

Layout solve(const Problem& p, const SolveOptions& opt, const ProgressFn& progress = {});

// ---- helpers (also for the plugin and the tests) ---------------------------------

double area(const Convex& c);
double area(const Shape& s);
Convex convexHull(std::vector<Vec2> pts);
// The shape as one part: its convex hull / its bounding rectangle.
Shape hullShape(const Shape& s);
Shape boxShape(const Shape& s);
// The turn (radians, in [0, pi/2)) of the shape's smallest bounding rectangle.
double principalAngle(const Shape& s);
// The main direction (radians, in [0, pi/2)) of the area's border: the one most
// of its length runs along (or at right angles to).
double mainDirection(const Problem& p);
// The border: the area's edges that are not shared by two of its polygons.
std::vector<std::pair<Vec2, Vec2>> areaBorder(const std::vector<std::vector<std::vector<Vec2>>>& area);
// The shape's parts turned by `angle` and moved to `pos`.
std::vector<Convex> placedParts(const Shape& s, Vec2 pos, double angle);
// Checks a layout: every copy inside the area (margin kept), none overlapping
// another or an obstacle (spacing kept). `why` says what is wrong.
bool layoutValid(const Problem& p, const Layout& l, std::string* why = nullptr);

} // namespace forma::packing
