// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - dialogs for the standard engineering parts (Draw > Engineering,
// Draw > Mechanical): pick a standard size, see what it will be, place it.
#pragma once

#include "core/MechParts.h"

#include <string>
#include <vector>

class QWidget;

namespace forma {

enum class PartFamily {
    Screw,  // hex bolt, socket cap, countersunk, threaded rod
    Nut,    // hexagonal or square
    Washer,
    SpurGear,
    Rack,
    BevelGear, // for shafts at right angles; placed alone or as a meshing pair
};

// What the dialog chose: a fastener or a gear.
struct PartChoice {
    bool gear = false;
    FastenerParams fastener;
    GearParams gearParams;
    // Place the gear meshing with this placed gear (kNone: on the cursor);
    // meshParams are that gear's.
    Id meshWith = kNone;
    GearParams meshParams;
    // Bevel gears: place the gear and its mate together, meshing.
    bool pair = false;
};

// A gear already in the model that a new one can be fitted to (same module
// and pressure angle, so they mesh).
struct GearRef {
    std::string name;
    GearParams params;
    Id instance = kNone; // a selected placement of it: the new gear can go right next to it
};

// Shows the dialog for the family (remembering the last choices). `gears`
// are the model's gears offered to fit to. Returns false when cancelled.
bool runPartDialog(QWidget* parent, PartFamily family, PartChoice& out, const std::vector<GearRef>& gears = {});

// A gear's parameters kept with its component (ComponentDef::extra, JSON),
// so a later gear can be fitted to it. gearFromComponent also reads the names
// Forma gives gears ("Spur Gear m1 z20", "Gear Rack m2 × 10").
std::string withGearTag(const std::string& extra, const GearParams& p);
bool gearFromComponent(const std::string& extra, const std::string& name, GearParams& out);

} // namespace forma
