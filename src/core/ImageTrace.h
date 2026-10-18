// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Trace Image: turns an image's alpha cutout or its dark/light
// shapes into outline geometry on the face that shows the image.
#pragma once

#include "Mesh.h"

#include <string>
#include <vector>

namespace gs {

// Scalar image: values[y * width + x] in 0..1, row 0 at the top of the image.
struct ScalarImage {
    int width = 0, height = 0;
    std::vector<float> values;
    float at(int x, int y) const { return values[size_t(y) * width + x]; }
};

struct TraceOptions {
    double threshold = 0.5;   // values above this are "inside" the shape
    double simplify = 1.0;    // contour simplification tolerance (pixels)
    double minArea = 16.0;    // drop loops enclosing less than this (square pixels)
    bool removeBackground = true; // erase the parts of the face outside the shape
};

// Closed contours (pixel coordinates, y down, within [0,w] x [0,h]) of the
// region where the image exceeds the threshold. The image border counts as
// outside, so contours always close.
std::vector<std::vector<Vec2>> traceContours(const ScalarImage& img, const TraceOptions& opt);

// The planar texture mapping actually used to show a face side (explicit
// mapping, or the default world-aligned projection).
TexMap effectiveTexMap(const Face& f, bool frontSide, const Material* m);

// Traces `img` (as mapped onto face f by `map`, one image copy over uv 0..1)
// and draws the outlines on the face. With removeBackground the face parts
// outside the shape are erased. Returns false (mesh untouched) with a message
// if nothing could be traced.
bool traceImageOnFace(Mesh& m, Id f, const TexMap& map, const ScalarImage& img, const TraceOptions& opt,
                      std::string* error = nullptr);

} // namespace gs
