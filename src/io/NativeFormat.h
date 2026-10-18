// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch native model format (.gsketch): open, documented JSON.
//
// {
//   "format": "GNUSketch", "version": 1, "units": "meter", "up": "Z",
//   "materials": [ { "name", "color": [r,g,b,a], "opacity",
//                    "texture": { "mime", "width", "height", "data": base64 } } ],
//   "vertices": [ [x,y,z], ... ],
//   "edges":    [ { "v": [a,b], "soft", "smooth", "hidden", "curve" } ],
//   "faces":    [ { "outer": [v...], "holes": [[v...]], "normal": [x,y,z], "hidden",
//                   "front": { "material", "uv": { "origin", "u", "v" } }, "back": {...} } ],
//   "guides":   [ { "kind": "line"|"point", "p": [..], "dir": [..] } ],
//   "view":     { application specific (camera, style) }
// }
#pragma once

#include "core/Document.h"

#include <QJsonObject>
#include <QString>

namespace gs {

bool saveNative(const Document& doc, const QString& path, const QJsonObject& view, QString* error = nullptr);
bool loadNative(Document& doc, const QString& path, QJsonObject* view, QString* error = nullptr);

// Serialises a mesh + materials subset (used for clipboard).
QJsonObject meshToJson(const Mesh& mesh);
bool meshFromJson(const QJsonObject& o, Mesh& mesh);

} // namespace gs
