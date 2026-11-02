// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - ready-made architectural materials with generated PBR maps.
#pragma once

#include "core/Material.h"

#include <string>
#include <vector>

namespace gs {

int architecturalMaterialCount();
std::string architecturalMaterialName(int index);
// The material, with its maps generated on first use (a fraction of a second).
// Safe from any thread; the same object comes back every time.
const Material& architecturalMaterial(int index);
// Starts generating them all in the background.
void prepareArchitecturalMaterials();

} // namespace gs
