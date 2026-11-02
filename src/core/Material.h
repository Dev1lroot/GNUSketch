// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - materials: colour, texture image (stored as encoded bytes), PBR maps
// and an optional custom surface shader.
#pragma once

#include <array>
#include <cstdint>
#include <map>
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

// PBR maps besides the base colour image (Material::texture).
enum MapSlot { MapNormal, MapMetallic, MapRoughness, MapOcclusion, MapHeight, MapEmission, MapOpacity, MapSlotCount };

// One PBR channel: an optional image and how strongly it acts. Without an image
// the strength of Metallic and Roughness is the plain value of the channel; with
// one it multiplies the image.
struct MaterialMap {
    Texture tex;
    float strength = 1.0f;
    bool operator==(const MaterialMap&) const = default;
};

inline std::array<MaterialMap, MapSlotCount> defaultMaterialMaps() {
    std::array<MaterialMap, MapSlotCount> m;
    m[MapMetallic].strength = 0.0f;
    m[MapRoughness].strength = 0.6f;
    m[MapHeight].strength = 0.5f;
    return m;
}

struct Material {
    std::string name;
    Color color = Color::rgb8(200, 200, 200);
    float opacity = 1.0f;
    Texture texture; // optional base colour image

    // Physically based shading (only used when advanced() is true; everything
    // else keeps the plain SketchUp-like shading).
    bool pbr = false;
    float baseStrength = 1.0f; // 0 = colour only, 1 = image only
    std::array<MaterialMap, MapSlotCount> maps = defaultMaterialMaps();
    Color emission = Color(0, 0, 0); // multiplied by the Emission map and strength
    bool flipNormalY = false;        // DirectX style normal map (green points down)
    bool blend = false;              // alpha blended even at full opacity (custom shaders)

    // Custom surface shader (GLSL, see surfaceShaderTemplate()).
    bool shaderOn = false;
    std::string shader;
    // Values of the shader's `uniform` properties, by name.
    std::map<std::string, std::array<float, 4>> shaderProps;

    bool hasTexture() const { return texture.valid(); }
    bool hasMap(MapSlot s) const { return maps[s].tex.valid(); }
    bool hasAnyMap() const {
        for (const MaterialMap& m : maps)
            if (m.tex.valid()) return true;
        return false;
    }
    bool customShader() const { return shaderOn && !shader.empty(); }
    // Needs the PBR renderer rather than the plain one.
    bool advanced() const { return pbr || hasAnyMap() || customShader(); }
    // Drawn with blending because of more than its opacity.
    bool blended() const { return advanced() && (blend || hasMap(MapOpacity)); }
    // Real-world size of one tile; the base image decides, then the first map.
    void tileSize(double& w, double& h) const;
    void setTileSize(double w, double h);
    bool operator==(const Material&) const = default;
};

// Same look, comparing images by content (for merging materials across files).
bool sameMaterial(const Material& a, const Material& b);

// Which map an image is, judging by its file name ("oak_Roughness_2k.png" ...).
// Returns MapSlotCount for the base colour and -1 when the name says nothing.
int guessMapSlot(const std::string& fileName);
const char* mapSlotName(MapSlot s);
const char* mapSlotKey(MapSlot s); // stable key in files

// One property of a custom shader, declared as an ordinary uniform:
//   uniform float _Speed;  // range(0, 10) default(2)
//   uniform vec3 _Tint;    // color default(1, 0.5, 0.2)
struct ShaderProp {
    std::string name;
    int components = 1; // 1..4
    bool isColor = false;
    float min = 0, max = 1;
    std::array<float, 4> def{0, 0, 0, 1};
};
std::vector<ShaderProp> parseShaderProps(const std::string& source);
// Starting point for a new custom shader.
const char* surfaceShaderTemplate();

// SketchUp-like default colours for faces without a material.
inline Color defaultFrontColor() { return Color::rgb8(255, 255, 255); }
inline Color defaultBackColor() { return Color::rgb8(164, 178, 187); }

} // namespace gs
