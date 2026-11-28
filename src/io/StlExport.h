// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - binary STL export (millimeters, Z-up) for 3D printing.
#pragma once

#include "core/Document.h"

#include <QString>

namespace forma {

bool exportStl(const Document& doc, const QString& path, double scale = 1000.0, QString* error = nullptr);

} // namespace forma
