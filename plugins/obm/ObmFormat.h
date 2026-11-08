// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Open Block Model (.obm), GNUBlocks' model format (its docs/obm.md),
// read into a GNUSketch model and written back out of one.
//
// Reading: each group becomes a component (its rotation about its pivot the
// component's transform, groups inside groups components inside components),
// each cube six faces (only those the cube has) with its pictures laid on exactly
// as the game lays them: a block model's way, or an entity model's (box UV
// unwrapped, top and bottom turned as the game turns them). Pictures are read
// from the game's textures folder: the "textures" folder beside the "models"
// folder the model is in, else GNUSKETCH_OBM_TEXTURES, else the one set in the
// settings (obm/texturesDir).
//
// Writing: every box, and every lone rectangle (a flat cube), of the model and its
// components becomes a cube; its rotation is worked out from how it lies, its
// faces' UVs and rotations from how its pictures lie on it. A cube nothing was
// done to since it was read is written back exactly as it came, and so is all of
// the file GNUSketch has no use for (tags, props, animations, extensions, a
// group's settings): those are kept with the model (ModelState::extra and
// ComponentDef::extra) and saved with it.
//
// Units: a pixel is 1/16 m (a block is a metre). OBM's +Y up, +Z south is
// GNUSketch's +Z up, -Y south.
#pragma once

#include "core/Document.h"

#include <QString>
#include <QStringList>

namespace gs::obm {

// Where the pictures named by path ("block/oak_planks") are, for a model at modelPath ("" if not found).
QString texturesFolder(const QString& modelPath);

// `missing`, if given, gets the materials whose pictures weren't found (and nothing is said of them in messages).
bool importObm(Document& doc, const QString& path, QStringList* messages, std::vector<Id>* missing = nullptr);
bool exportObm(const Document& doc, const QString& path, QStringList* messages);

// ---- what the file had that GNUSketch has no place for, kept with the model ----
struct CubeInfo {
    std::string name;
    std::vector<std::string> tags;
};
struct GroupInfo {
    std::string name;
    Vec3 pivot, rotation;
    std::vector<std::string> tags;
    std::vector<CubeInfo> cubes;
};
struct ModelInfo {
    std::string id, kind, anchor, file;
    std::vector<std::string> tags;
    std::vector<std::string> textures; // their paths, by index
};
// false: it didn't come from an .obm
bool groupInfo(const Document& doc, Id def, GroupInfo& out);
bool modelInfo(const Document& doc, ModelInfo& out);
// (each in the document as it is: the caller makes the undo step)
void setGroupTags(Document& doc, Id def, const std::vector<std::string>& tags);
void setCubeTags(Document& doc, Id def, int cube, const std::vector<std::string>& tags);
void setModelTags(Document& doc, const std::vector<std::string>& tags);
// the picture of a material named by its path ("block/oak_planks@iron": block/oak_planks.png) in a textures folder
QString picturePath(const QString& texturesFolder, const std::string& materialName);

} // namespace gs::obm
