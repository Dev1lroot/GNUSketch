// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - materials (colour + optional texture image stored as encoded bytes).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gs {

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    Color() = default;
    Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
    static Color rgb8(int r, int g, int b, int a = 255) { return {r / 255.f, g / 255.f, b / 255.f, a / 255.f}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

struct Texture {
    // Original encoded image file bytes (PNG/JPEG/...). Kept as-is so exporters can
    // write the file unchanged and the native format can embed it losslessly.
    std::shared_ptr<const std::vector<uint8_t>> data;
    std::string mime = "image/png"; // image/png, image/jpeg, ...
    int pixelWidth = 0, pixelHeight = 0;
    double worldWidth = 1.0, worldHeight = 1.0; // real-world size of one tile (meters)
    uint64_t revision = 0;                     // changes whenever the image changes
    bool valid() const { return data && !data->empty(); }
    bool operator==(const Texture& b) const {
        return data == b.data && worldWidth == b.worldWidth && worldHeight == b.worldHeight;
    }
    std::string fileExtension() const {
        if (mime == "image/jpeg") return "jpg";
        if (mime == "image/webp") return "webp";
        if (mime == "image/bmp") return "bmp";
        return "png";
    }
};

struct Material {
    std::string name;
    Color color = Color::rgb8(200, 200, 200);
    float opacity = 1.0f;
    Texture texture; // optional
    bool hasTexture() const { return texture.valid(); }
    bool operator==(const Material&) const = default;
};

// SketchUp-like default colours for faces without a material.
inline Color defaultFrontColor() { return Color::rgb8(255, 255, 255); }
inline Color defaultBackColor() { return Color::rgb8(164, 178, 187); }

} // namespace gs
