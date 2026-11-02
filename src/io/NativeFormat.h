// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch native model format (.gsketch): open, documented JSON.
//
// {
//   "format": "GNUSketch", "version": 1, "units": "meter", "up": "Z",
//   "materials": [ { "name", "color": [r,g,b,a], "opacity",
//                    "texture": { "mime", "width", "height", "data": base64 },
//                    optional PBR: "pbr", "baseStrength", "emission": [r,g,b,a], "flipNormalY", "blend",
//                    "maps": { "normal"|"metallic"|"roughness"|"occlusion"|"height"|"emission"|"opacity":
//                              { "strength", "texture": {...} } },
//                    optional custom shader: "shaderOn", "shader": GLSL text,
//                    "shaderProps": { name: [up to 4 numbers] } } ],
//   "vertices": [ [x,y,z], ... ],
//   "edges":    [ { "v": [a,b], "soft", "smooth", "hidden", "curve" } ],
//   "faces":    [ { "outer": [v...], "holes": [[v...]], "normal": [x,y,z], "hidden",
//                   "front": { "material", "uv": { "origin", "u", "v" } }, "back": {...} } ],
//   "guides":   [ { "kind": "line"|"point", "p": [..], "dir": [..] } ],
//   "components": [ { "name", "uid", "vertices", "edges", "faces", "nextCurve" } ],
//   "instances":  [ { "component": index, "transform": [16 numbers, column-major
//                     4x4 component -> model], "hidden" } ],
//   "objects":  [ { "kind": "point"|"spot"|"directional"|"camera", "name", "pos", "target",
//                   "enabled", "color", "intensity", "range", "coneAngle", "softness",
//                   "fov", "perspective" } ],
//   "view":     { application specific (camera, style; a prefab keeps its "thumbnail" here) }
// }
#pragma once

#include "core/Document.h"

#include <QJsonObject>
#include <QString>

namespace gs {

bool saveNative(const Document& doc, const QString& path, const QJsonObject& view, QString* error = nullptr);
bool loadNative(Document& doc, const QString& path, QJsonObject* view, QString* error = nullptr);

// The whole model (mesh, materials, guides) as .gsketch JSON bytes and back;
// also used for the system clipboard.
// The display style in the file's "view" object.
QJsonObject styleToJson(const RenderStyle& s);
void styleFromJson(const QJsonObject& o, RenderStyle& s);

QByteArray modelToJson(const ModelState& state, const QJsonObject& view = {});
bool modelFromJson(const QByteArray& json, ModelState& state, QJsonObject* view = nullptr, QString* error = nullptr);

// One material as JSON (also the content of a library .gsmat file).
QJsonObject materialToJson(const Material& m);
Material materialFromJson(const QJsonObject& o);

// Serialises a mesh + materials subset (used for clipboard).
QJsonObject meshToJson(const Mesh& mesh);
bool meshFromJson(const QJsonObject& o, Mesh& mesh);

} // namespace gs
