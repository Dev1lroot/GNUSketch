// SPDX-License-Identifier: GPL-3.0-or-later
#include "Material.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace gs {

void Material::tileSize(double& w, double& h) const {
    const Texture* t = hasTexture() ? &texture : nullptr;
    for (int i = 0; !t && i < MapSlotCount; ++i)
        if (maps[i].tex.valid()) t = &maps[i].tex;
    w = t && t->worldWidth > 1e-9 ? t->worldWidth : 1;
    h = t && t->worldHeight > 1e-9 ? t->worldHeight : 1;
}

void Material::setTileSize(double w, double h) {
    texture.worldWidth = w;
    texture.worldHeight = h;
    for (MaterialMap& m : maps) {
        m.tex.worldWidth = w;
        m.tex.worldHeight = h;
    }
}

namespace {
bool sameImage(const Texture& a, const Texture& b) {
    if (a.valid() != b.valid()) return false;
    if (!a.valid()) return true;
    return a.data == b.data || *a.data == *b.data;
}
} // namespace

bool sameMaterial(const Material& a, const Material& b) {
    if (a.name != b.name || !(a.color == b.color) || a.opacity != b.opacity || a.pbr != b.pbr || a.baseStrength != b.baseStrength ||
        !(a.emission == b.emission) || a.flipNormalY != b.flipNormalY || a.blend != b.blend || a.shaderOn != b.shaderOn ||
        a.shader != b.shader || a.shaderProps != b.shaderProps || !sameImage(a.texture, b.texture))
        return false;
    for (int i = 0; i < MapSlotCount; ++i)
        if (a.maps[i].strength != b.maps[i].strength || !sameImage(a.maps[i].tex, b.maps[i].tex)) return false;
    return true;
}

int guessMapSlot(const std::string& fileName) {
    // Words of the base name, lower case: "Oak_Floor-Roughness.2k.png" -> oak floor roughness 2k
    std::string name = fileName;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    std::vector<std::string> words(1);
    char prev = 0;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        if (!std::isalnum(u)) {
            if (!words.back().empty()) words.emplace_back();
        } else {
            if (std::isupper(u) && std::islower((unsigned char)prev) && !words.back().empty()) words.emplace_back(); // camelCase
            words.back() += (char)std::tolower(u);
        }
        prev = c;
    }
    // Later words decide: "normal_roughness_map" would be rare, "wood_normal" is not.
    static const std::map<std::string, int> kWords = {
        {"albedo", MapSlotCount}, {"basecolor", MapSlotCount}, {"base", MapSlotCount}, {"diffuse", MapSlotCount},
        {"diff", MapSlotCount}, {"color", MapSlotCount}, {"colour", MapSlotCount}, {"col", MapSlotCount},
        {"normal", MapNormal}, {"normals", MapNormal}, {"nor", MapNormal}, {"nrm", MapNormal}, {"norm", MapNormal},
        {"metallic", MapMetallic}, {"metalness", MapMetallic}, {"metal", MapMetallic}, {"met", MapMetallic},
        {"roughness", MapRoughness}, {"rough", MapRoughness}, {"rgh", MapRoughness},
        {"ao", MapOcclusion}, {"occlusion", MapOcclusion}, {"ambientocclusion", MapOcclusion}, {"occ", MapOcclusion},
        {"height", MapHeight}, {"displacement", MapHeight}, {"disp", MapHeight}, {"bump", MapHeight},
        {"emission", MapEmission}, {"emissive", MapEmission}, {"emit", MapEmission}, {"glow", MapEmission},
        {"opacity", MapOpacity}, {"alpha", MapOpacity}, {"mask", MapOpacity}};
    for (auto it = words.rbegin(); it != words.rend(); ++it) {
        auto f = kWords.find(*it);
        if (f != kWords.end()) return f->second;
    }
    return -1;
}

const char* mapSlotName(MapSlot s) {
    switch (s) {
    case MapNormal: return "Normal";
    case MapMetallic: return "Metallic";
    case MapRoughness: return "Roughness";
    case MapOcclusion: return "Ambient Occlusion";
    case MapHeight: return "Height";
    case MapEmission: return "Emission";
    case MapOpacity: return "Opacity";
    default: return "";
    }
}

const char* mapSlotKey(MapSlot s) {
    switch (s) {
    case MapNormal: return "normal";
    case MapMetallic: return "metallic";
    case MapRoughness: return "roughness";
    case MapOcclusion: return "occlusion";
    case MapHeight: return "height";
    case MapEmission: return "emission";
    case MapOpacity: return "opacity";
    default: return "";
    }
}

std::vector<ShaderProp> parseShaderProps(const std::string& source) {
    std::vector<ShaderProp> out;
    static const std::regex line(R"(^\s*uniform\s+(float|vec2|vec3|vec4)\s+([A-Za-z_]\w*)\s*;[ \t]*(?://(.*))?$)");
    static const std::regex range(R"(range\s*\(\s*([-+.\deE]+)\s*,\s*([-+.\deE]+)\s*\))");
    static const std::regex def(R"(default\s*\(([^)]*)\))");
    std::istringstream in(source);
    std::string text;
    while (std::getline(in, text)) {
        if (!text.empty() && text.back() == '\r') text.pop_back();
        std::smatch m;
        if (!std::regex_match(text, m, line)) continue;
        ShaderProp p;
        const std::string type = m[1].str();
        p.components = type == "float" ? 1 : type[3] - '0';
        p.name = m[2].str();
        const std::string note = m[3].str();
        p.isColor = p.components >= 3 && note.find("color") != std::string::npos;
        p.def = p.isColor ? std::array<float, 4>{1, 1, 1, 1} : std::array<float, 4>{0, 0, 0, 1};
        std::smatch a;
        try {
            if (std::regex_search(note, a, range)) {
                p.min = std::stof(a[1].str());
                p.max = std::stof(a[2].str());
            }
            if (std::regex_search(note, a, def)) {
                std::istringstream vals(a[1].str());
                std::string v;
                for (int i = 0; i < 4 && std::getline(vals, v, ','); ++i) p.def[i] = std::stof(v);
            }
        } catch (const std::exception&) {
            // A malformed note just leaves the defaults.
        }
        if (p.max <= p.min) p.max = p.min + 1;
        out.push_back(std::move(p));
    }
    return out;
}

const char* surfaceShaderTemplate() {
    return R"(// Custom surface shader (GLSL 3.30). Set the fields of `s`; lighting, shadows
// and the sun are done for you, like a Unity surface shader.
//
//   s.albedo, s.metallic, s.roughness, s.normal, s.emission, s.ao, s.alpha
//   read only: s.uv, s.worldPos, s.viewDir
//   maps: _BaseMap _NormalMap _MetallicMap _RoughnessMap _OcclusionMap
//         _HeightMap _EmissionMap _OpacityMap   and _Time (seconds)
//
// A uniform becomes a slider or colour in the editor:
uniform vec3 _Tint;    // color default(1, 1, 1)
uniform float _Speed;  // range(0, 10) default(1)

void surface(inout Surface s) {
    s.albedo *= _Tint;
    // Using _Time keeps the view repainting, so this pulse is off to begin with:
    // s.emission += s.albedo * (0.5 + 0.5 * sin(_Time * _Speed));
}
)";
}

} // namespace gs
