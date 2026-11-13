// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - 3D printing exports that keep the model's length unit:
//   3MF  (unit declared in the file, colours as base materials)
//   AMF  (unit declared in the file, colours per volume)
//   STL  binary / ASCII (unitless format: written in the chosen unit, noted in the header)
//   PLY, OFF (unitless: written in the chosen unit, noted in a comment)
// Coordinates are Z-up like the model. Vertices are shared between triangles
// (welded), which slicers need to see a closed (manifold) solid.
#pragma once

#include "core/Document.h"
#include "core/Units.h"

#include <QString>
#include <array>
#include <vector>

namespace gs {

enum class PrintFormat { ThreeMF, Amf, StlBinary, StlAscii, Ply, Off };

struct PrintMesh {
    std::vector<Vec3> verts;               // in the export unit
    std::vector<std::array<int, 3>> tris;  // counter-clockwise seen from outside
    std::vector<int> triMaterial;          // index into colors, or -1
    std::vector<Color> colors;
    std::vector<std::string> colorNames;
};

// Visible faces of the whole model (components flattened), triangulated and
// welded, scaled from meters into `unit`.
PrintMesh buildPrintMesh(const Document& doc, LengthUnit unit);

const char* printFormatName(PrintFormat f);
const char* printFormatExtension(PrintFormat f);
// Unit name as written into 3MF / AMF ("millimeter", "inch", ...).
const char* printUnitName(PrintFormat f, LengthUnit u);
// The unit actually written: AMF has no centimeters, so they become millimeters.
LengthUnit printUnitFor(PrintFormat f, LengthUnit wanted);

bool exportPrint(const Document& doc, const QString& path, PrintFormat fmt, LengthUnit unit, QString* error = nullptr);

// Minimal ZIP writer (deflate), used for 3MF containers.
QByteArray makeZip(const std::vector<std::pair<QString, QByteArray>>& files);

} // namespace gs
