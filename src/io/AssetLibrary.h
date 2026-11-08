// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - files shared between models, kept in the application's data
// directory: prefabs (share/prefabs/*.gsketch), materials (share/materials/*.gsmat)
// and texture images (share/textures).
#pragma once

#include "core/Document.h"

#include <QImage>
#include <QString>
#include <QStringList>

namespace gs {

// The writable directory for a kind of asset ("prefabs", "materials", "textures"),
// created on demand: ~/.local/share/GNUSketch/<kind>. The environment variable
// GNUSKETCH_DATA_DIR replaces ~/.local/share/GNUSketch.
QString assetDir(const QString& kind);
// That directory first, then the read-only system ones (/usr/share/GNUSketch/<kind>).
QStringList assetDirs(const QString& kind);

struct PrefabInfo {
    QString path;
    QString name;
    QImage thumbnail; // may be null
};

// Writes the definition as a stand-alone model file; a free file name is
// picked, nothing gets overwritten. Returns the path, or empty with *error set.
// The entity settings (class, script...) go with it.
QString savePrefab(const ModelState& state, Id def, const QString& name, const QImage& thumbnail, QString* error = nullptr, const EntityProps& entity = {});
bool loadPrefab(const QString& path, ModelState& out, QString* name = nullptr, QImage* thumbnail = nullptr, QString* error = nullptr);
// The prefab files of every directory, sorted by name (no file contents read).
QStringList prefabFiles();

// Saves into the materials directory (replacing one of the same name).
QString saveLibraryMaterial(const Material& m, QString* error = nullptr);
QStringList libraryMaterialFiles();
bool loadLibraryMaterial(const QString& path, Material& out);
// Ready-made physically based materials.
std::vector<Material> builtinPbrMaterials();
// Image files in the textures directories and the given extra folders.
QStringList textureFiles(const QStringList& extraFolders);
// Creates a material from image files, each going to the map its name suggests
// (see guessMapSlot; the first unnamed image becomes the base colour).
bool materialFromImages(const QStringList& paths, Material& out, QString* error = nullptr);

} // namespace gs
