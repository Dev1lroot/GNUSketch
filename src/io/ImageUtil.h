// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - texture image helpers (Qt image codecs).
#pragma once

#include "core/ImageTrace.h"
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

// What Trace Image outlines.
enum class TraceSource { Alpha, Dark, Light };
// Per-pixel values for tracing (alpha, darkness or lightness in 0..1), with the
// image scaled down so its longer side is at most maxSide pixels.
ScalarImage scalarImage(const QImage& img, TraceSource src, int maxSide = 2048);
bool hasTransparency(const QImage& img);

} // namespace gs
