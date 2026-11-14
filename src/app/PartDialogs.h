// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - dialogs for the standard engineering parts (Draw > Engineering,
// Draw > Mechanical): pick a standard size, see what it will be, place it.
#pragma once

#include "core/MechParts.h"

class QWidget;

namespace gs {

enum class PartFamily {
    Screw,  // hex bolt, socket cap, countersunk, threaded rod
    Nut,    // hexagonal or square
    Washer,
    SpurGear,
    Rack,
};

// What the dialog chose: a fastener or a gear.
struct PartChoice {
    bool gear = false;
    FastenerParams fastener;
    GearParams gearParams;
};

// Shows the dialog for the family (remembering the last choices). Returns
// false when cancelled.
bool runPartDialog(QWidget* parent, PartFamily family, PartChoice& out);

} // namespace gs
