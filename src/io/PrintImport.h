// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - reading 3D printing files: 3MF, AMF (plain or zipped), STL
// (binary and ASCII), PLY (ASCII and binary) and OFF.
//
// 3MF and AMF say their unit. STL, PLY and OFF have none: files written by
// GNUSketch name it in the header/comment, others get the unit the caller
// gives (millimeters is what slicers assume).
#pragma once

#include "core/Document.h"
#include "core/Units.h"

#include <QByteArray>
#include <QString>
#include <array>
#include <map>
#include <vector>

namespace gs {

struct PrintModel {
    std::vector<Vec3> verts; // meters, Z up
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triColor; // index into colors, or -1
    std::vector<Color> colors;
    std::vector<std::string> colorNames;
};

// "3mf", "amf", "stl", "ply", "off" (by extension), or empty.
QString printImportFormat(const QString& path);
// The unit the file itself names (3MF/AMF always, the others when GNUSketch
// wrote them). False if it does not say.
bool printFileUnit(const QString& path, double& metersPerUnit);
// Reads the file; unitless numbers are taken as `metersPerUnit`.
bool readPrintFile(const QString& path, double metersPerUnit, PrintModel& out, QString* error = nullptr);
// Adds the model to the document as a component named after the file:
// triangles welded, flat neighbours joined into n-gons, colours as materials.
// Returns the instance (kNone on failure).
Id importPrintModel(Document& doc, const PrintModel& model, const std::string& name, QString* error = nullptr);

// ZIP archive entries (stored or deflated), for 3MF and zipped AMF.
bool readZip(const QByteArray& zip, std::map<QString, QByteArray>& files, QString* error = nullptr);

} // namespace gs
