// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Valve Source engine exports: Hammer maps (.vmf) and studio
// models (.smd, with a .qc to compile them).
//
// The behaviour follows Valve's SketchUp Source Tools (MIT, 2008-2010: vmf_export,
// smd_export, "Hammer Time"), written anew for GNUSketch's model; no SketchUp
// code or formats are involved:
//  * every closed, convex piece of geometry (in a component or loose) becomes a
//    brush, its faces brush sides with their materials; a component with several
//    becomes a Hammer group;
//  * components named after an entity class (info_player_start, light,
//    npc_zombie, weapon_ar2 ... also "ENT.npc_zombie" / "ENT_light") become
//    point entities where they stand, turned as they are;
//  * GNUSketch's lights become light, light_spot and light_environment, the sun
//    a light_environment;
//  * material names map to Source materials: "tools-toolsnodraw" or
//    "tools/toolsnodraw" is tools/toolsnodraw; unnamed faces get dev/graygrid;
//  * Source units are inches (1 unit = 2.54 cm), Z up;
//  * warnings like the original's: no info_player_start, no light, beyond the
//    16384 unit limit, pieces that are not convex or not closed.
#pragma once

#include "core/Document.h"

#include <QString>
#include <QStringList>

namespace gs::valve {

constexpr double kUnitsPerMeter = 1.0 / 0.0254;

struct VmfOptions {
    std::string skyname = "sky_day01_01";
    std::string defaultMaterial = "dev/graygrid";
    bool sealBox = false; // six tools/toolsskybox brushes around everything (a leak-free test map)
    int lightmapScale = 16;
};

struct SmdOptions {
    bool writeQc = true;
    std::string surfaceProp = "default";
    bool staticProp = true;
};

// "tools-toolsnodraw" -> "tools/toolsnodraw"; empty -> the default.
std::string sourceMaterial(const std::string& name, const std::string& fallback);
// The VMF text of a model (messages: warnings).
QByteArray vmfText(const Document& doc, const VmfOptions& o, QStringList* messages);
bool writeVmf(const Document& doc, const QString& path, const VmfOptions& o, QStringList* messages);
// The reference SMD of the whole model.
QByteArray smdText(const Document& doc, QStringList* messages);
// A .qc beside it that compiles the model with studiomdl.
QByteArray qcText(const QString& smdFileName, const SmdOptions& o);
bool writeSmd(const Document& doc, const QString& path, const SmdOptions& o, QStringList* messages);

} // namespace gs::valve
