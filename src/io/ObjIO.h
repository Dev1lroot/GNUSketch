// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Wavefront OBJ/MTL import & export. OBJ keeps n-gon faces, so the
// exported model stays non-triangulated (faces with holes are the exception:
// OBJ cannot express holes, so those faces are triangulated). OBJ is Y-up by
// convention; the Z-up model is converted on the way in and out.
#pragma once

#include "core/Document.h"

#include <QString>

namespace gs {

struct ObjExportOptions {
    bool yUp = true;           // convert Z-up -> Y-up
    bool exportEdges = true;   // loose edges as "l" elements
    bool exportTextures = true;
    double scale = 1.0;        // model meters -> file units
};

bool exportObj(const Document& doc, const QString& path, const ObjExportOptions& opt = {}, QString* error = nullptr);
// Imports into the document (adds geometry and materials). Returns the number of faces imported.
int importObj(Document& doc, const QString& path, bool yUp = true, double scale = 1.0, QString* error = nullptr);

} // namespace gs
