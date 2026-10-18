// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - glTF 2.0 export (single self-contained .gltf with embedded
// buffers and images). glTF is triangle based and Y-up; good for converting to
// FBX with Blender/assimp.
#pragma once

#include "core/Document.h"

#include <QString>

namespace gs {

bool exportGltf(const Document& doc, const QString& path, QString* error = nullptr);

// Shared helper: triangulated render mesh of a face (positions + uvs per corner).
struct FaceTriangles {
    std::vector<Vec3> pos;
    std::vector<Vec2> uv;
    std::vector<std::array<int, 3>> tris;
};
FaceTriangles triangulateFace(const Mesh& m, Id f, bool frontSide, const Material* mat);

} // namespace gs
