// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - texture image helpers (Qt image codecs).
#pragma once

#include "core/Material.h"

#include <QImage>
#include <QString>

namespace gs {

// Loads an image file into a Texture. PNG and JPEG bytes are kept verbatim;
// other formats are re-encoded to PNG so saved models stay portable.
bool loadTextureFile(const QString& path, Texture& out, QString* error = nullptr);
bool textureFromBytes(const QByteArray& bytes, Texture& out);
Texture textureFromImage(const QImage& img);
QImage decodeTexture(const Texture& t);
// Average colour of a texture (used as the material base colour).
Color averageColor(const QImage& img);
bool isImageFile(const QString& path);

} // namespace gs
