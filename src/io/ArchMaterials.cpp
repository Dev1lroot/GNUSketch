// SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchMaterials.h"

#include "ImageUtil.h"

#include <QBuffer>
#include <QImage>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <random>

namespace gs {

namespace {

constexpr int N = 512;

float frac(float x) { return x - std::floor(x); }
float smooth(float t) { return t * t * (3 - 2 * t); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float box(float e0, float e1, float x) { return smooth(clamp01((x - e0) / (e1 - e0))); }

uint32_t hash2(int x, int y, int seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float rnd(int x, int y, int seed) { return float(hash2(x, y, seed) & 0xffffff) / float(0xffffff); }

// Value noise that repeats every `period` lattice cells: the maps tile seamlessly.
float noise(float u, float v, int period, int seed) {
    const float x = u * period, y = v * period;
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = smooth(x - xi), fy = smooth(y - yi);
    auto at = [&](int a, int b) { return rnd(((a % period) + period) % period, ((b % period) + period) % period, seed); };
    return mixf(mixf(at(xi, yi), at(xi + 1, yi), fx), mixf(at(xi, yi + 1), at(xi + 1, yi + 1), fx), fy);
}
float fbm(float u, float v, int period, int octaves, int seed) {
    float sum = 0, amp = 0.5f, total = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += noise(u, v, period, seed + i) * amp;
        total += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / total;
}

struct Rgb {
    float r, g, b;
};
Rgb lerp(const Rgb& a, const Rgb& b, float t) { return {mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)}; }
Rgb rgb8(int r, int g, int b) { return {r / 255.f, g / 255.f, b / 255.f}; }

struct Pixel {
    Rgb color{0.5f, 0.5f, 0.5f};
    float height = 0.5f; // 1 = highest
    float rough = 0.7f;
    float metal = 0;
};
using Shader = std::function<Pixel(float u, float v)>;

QByteArray encode(const QImage& img) {
    // JPEG keeps a saved model small: these end up embedded in its file.
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 92);
    return bytes;
}

Texture texture(const QImage& img, double w, double h) {
    Texture t;
    textureFromBytes(encode(img), t);
    t.worldWidth = w;
    t.worldHeight = h;
    return t;
}

struct Spec {
    const char* name;
    double tileW, tileH;
    float normalScale; // steepness of the normal map
    bool heightMap;    // real relief worth parallax
    bool metalMap;
    Shader shade;
    std::function<void(Material&)> tweak;
};

void build(const Spec& s, Material& m) {
    std::vector<Pixel> px(size_t(N) * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) px[size_t(y) * N + x] = s.shade((x + 0.5f) / N, (y + 0.5f) / N);
    auto at = [&](int x, int y) -> const Pixel& { return px[size_t((y + N) % N) * N + (x + N) % N]; };
    QImage base(N, N, QImage::Format_RGB32), normal(base), rough(base), occ(base), height(base), metal(base);
    double sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const Pixel& p = at(x, y);
            base.setPixel(x, y, qRgb(int(clamp01(p.color.r) * 255), int(clamp01(p.color.g) * 255), int(clamp01(p.color.b) * 255)));
            sr += p.color.r;
            sg += p.color.g;
            sb += p.color.b;
            // Images have their first row at the top, so "up" in the image is -y.
            const float dx = (at(x + 1, y).height - at(x - 1, y).height) * s.normalScale;
            const float dy = (at(x, y - 1).height - at(x, y + 1).height) * s.normalScale;
            const float len = std::sqrt(dx * dx + dy * dy + 1);
            normal.setPixel(x, y, qRgb(int((-dx / len * 0.5f + 0.5f) * 255), int((-dy / len * 0.5f + 0.5f) * 255), int((1 / len * 0.5f + 0.5f) * 255)));
            const int r8 = int(clamp01(p.rough) * 255);
            rough.setPixel(x, y, qRgb(r8, r8, r8));
            const int h8 = int(clamp01(p.height) * 255);
            height.setPixel(x, y, qRgb(h8, h8, h8));
            const int m8 = int(clamp01(p.metal) * 255);
            metal.setPixel(x, y, qRgb(m8, m8, m8));
            // Cavities: lower than the surroundings means less light gets in.
            float around = 0;
            for (int k = 0; k < 8; ++k) {
                static const int ox[8] = {-6, 6, 0, 0, -4, 4, -4, 4}, oy[8] = {0, 0, -6, 6, -4, 4, 4, -4};
                around += at(x + ox[k], y + oy[k]).height;
            }
            const int o8 = int(clamp01(1 - std::max(0.f, around / 8 - p.height) * 4.f) * 255);
            occ.setPixel(x, y, qRgb(o8, o8, o8));
        }
    m.name = s.name;
    m.pbr = true;
    const double n = double(N) * N;
    m.color = Color(float(sr / n), float(sg / n), float(sb / n));
    m.texture = texture(base, s.tileW, s.tileH);
    m.maps[MapNormal].tex = texture(normal, s.tileW, s.tileH);
    m.maps[MapRoughness].tex = texture(rough, s.tileW, s.tileH);
    m.maps[MapRoughness].strength = 1;
    m.maps[MapOcclusion].tex = texture(occ, s.tileW, s.tileH);
    if (s.heightMap) m.maps[MapHeight].tex = texture(height, s.tileW, s.tileH);
    if (s.heightMap) m.maps[MapHeight].strength = 0.3f;
    if (s.metalMap) {
        m.maps[MapMetallic].tex = texture(metal, s.tileW, s.tileH);
        m.maps[MapMetallic].strength = 1;
    }
    if (s.tweak) s.tweak(m);
}

// Courses of bricks in running bond.
Pixel brick(float u, float v, const Rgb& a, const Rgb& b, const Rgb& mortar, int seed) {
    constexpr int kRows = 8, kCols = 4;
    const float y = v * kRows;
    const int row = (int)std::floor(y);
    const float x = u * kCols + (row % 2 ? 0.5f : 0.f);
    const int col = (int)std::floor(x);
    const float fx = x - col, fy = y - row;
    const float wob = (fbm(u, v, 16, 2, seed) - 0.5f) * 0.03f;
    const float edge = std::min({fx * 2, (1 - fx) * 2, fy, 1 - fy}) + wob; // about equal joints both ways
    const float inBrick = box(0.05f, 0.1f, edge);
    const float id = rnd(((col % kCols) + kCols) % kCols, row, seed);
    const float grain = fbm(u, v, 32, 4, seed + 7);
    Rgb c = lerp(a, b, id * 0.8f + grain * 0.4f);
    c = lerp(c, rgb8(40, 30, 28), std::max(0.f, fbm(u, v, 64, 2, seed + 11) - 0.62f) * 2);
    Pixel p;
    p.color = lerp(lerp(mortar, rgb8(80, 78, 74), fbm(u, v, 64, 2, seed + 3) * 0.4f), c, inBrick);
    p.height = inBrick * (0.75f + grain * 0.2f + (id - 0.5f) * 0.08f) + (1 - inBrick) * (0.2f + grain * 0.05f);
    p.rough = mixf(0.95f, 0.72f + grain * 0.2f, inBrick);
    return p;
}

const std::vector<Spec>& specs() {
    static const std::vector<Spec> all = [] {
        std::vector<Spec> v;
        auto glass = [](const char* name, Color c, float opacity, float rough, float bump, int seed) {
            return Spec{name, 1.0, 1.0, bump, false, false,
                        [=](float u, float w) {
                            Pixel p;
                            const float n = fbm(u, w, 32, 3, seed);
                            p.color = {c.r, c.g, c.b};
                            p.height = n;
                            p.rough = rough * (0.8f + 0.4f * fbm(u, w, 64, 2, seed + 3));
                            return p;
                        },
                        [=](Material& m) {
                            m.opacity = opacity;
                            m.maps[MapOcclusion] = MaterialMap();
                        }};
        };
        v.push_back(glass("Clear Glass", Color(0.86f, 0.94f, 0.96f), 0.14f, 0.04f, 0.4f, 21));
        v.push_back(glass("Frosted Glass", Color(0.9f, 0.95f, 0.96f), 0.62f, 0.55f, 5.f, 22));
        v.push_back(glass("Tinted Glass", Color(0.32f, 0.42f, 0.42f), 0.5f, 0.05f, 0.4f, 23));
        v.push_back({"Red Brick", 0.9, 0.6, 10.f, true, false,
                     [](float u, float w) { return brick(u, w, rgb8(142, 62, 44), rgb8(176, 88, 62), rgb8(170, 164, 152), 31); }, {}});
        v.push_back({"Painted White Brick", 0.9, 0.6, 9.f, true, false,
                     [](float u, float w) {
                         Pixel p = brick(u, w, rgb8(226, 224, 216), rgb8(240, 238, 232), rgb8(206, 204, 196), 32);
                         p.rough *= 0.8f;
                         return p;
                     }, {}});
        v.push_back({"Concrete", 2.0, 2.0, 5.f, false, false,
                     [](float u, float w) {
                         Pixel p;
                         const float big = fbm(u, w, 4, 5, 41), fine = fbm(u, w, 64, 3, 42);
                         const float pore = box(0.72f, 0.78f, noise(u, w, 96, 43));
                         p.color = lerp(rgb8(142, 140, 134), rgb8(186, 184, 178), big * 0.7f + fine * 0.3f);
                         p.color = lerp(p.color, rgb8(92, 90, 86), pore * 0.7f);
                         // Formwork seams.
                         const float seam = box(0.0f, 0.008f, std::min({frac(u * 2), 1 - frac(u * 2), frac(w * 1), 1 - frac(w * 1)}));
                         p.color = lerp(rgb8(112, 110, 106), p.color, seam);
                         p.height = 0.6f + fine * 0.15f - pore * 0.3f - (1 - seam) * 0.3f;
                         p.rough = 0.78f + fine * 0.15f;
                         return p;
                     }, {}});
        v.push_back({"Polished Concrete", 2.5, 2.5, 0.8f, false, false,
                     [](float u, float w) {
                         Pixel p;
                         const float big = fbm(u, w, 3, 5, 51), speck = noise(u, w, 200, 52);
                         p.color = lerp(rgb8(120, 120, 118), rgb8(164, 163, 158), big);
                         p.color = lerp(p.color, rgb8(70, 68, 64), box(0.8f, 0.86f, speck));
                         p.height = 0.5f + big * 0.1f;
                         p.rough = 0.16f + big * 0.16f;
                         return p;
                     }, {}});
        v.push_back({"White Plaster", 1.5, 1.5, 7.f, false, false,
                     [](float u, float w) {
                         Pixel p;
                         const float n = fbm(u, w, 16, 5, 61);
                         p.color = lerp(rgb8(218, 214, 204), rgb8(238, 235, 228), n);
                         p.height = n;
                         p.rough = 0.85f + n * 0.1f;
                         return p;
                     }, {}});
        v.push_back({"Oak Floor", 1.2, 1.2, 4.f, false, false,
                     [](float u, float w) {
                         constexpr int kPlanks = 6;
                         const float x = u * kPlanks;
                         const int plank = (int)std::floor(x);
                         const float fx = x - plank;
                         const float shift = rnd(plank, 0, 71);
                         const float along = frac(w + shift);
                         const float joint = box(0.0f, 0.025f, std::min({fx, 1 - fx})) * box(0.0f, 0.004f, std::min(along, 1 - along));
                         const float warp = fbm(u, along, 4, 3, 72 + plank) * 2;
                         const float rings = 0.5f + 0.5f * std::sin((fx * 5 + warp * 3 + shift * 9) * 6.283f);
                         const float fibre = fbm(fx / kPlanks + plank * 0.37f, along * 0.06f, 64, 3, 73);
                         Pixel p;
                         Rgb c = lerp(rgb8(150, 106, 62), rgb8(196, 150, 96), rings * 0.55f + fibre * 0.45f);
                         c = lerp(c, rgb8(172, 124, 76), shift * 0.6f);
                         p.color = lerp(rgb8(50, 34, 22), c, joint);
                         p.height = joint * (0.7f + fibre * 0.15f);
                         p.rough = mixf(0.9f, 0.34f + fibre * 0.25f, joint);
                         return p;
                     }, {}});
        v.push_back({"White Marble", 1.6, 1.6, 0.4f, false, false,
                     [](float u, float w) {
                         Pixel p;
                         const float warp = fbm(u, w, 4, 5, 81);
                         const float vein = 1 - std::abs(std::sin((u * 2 + w * 3 + warp * 5) * 3.14159f));
                         const float thin = std::pow(vein, 14.f), broad = std::pow(vein, 3.f);
                         p.color = lerp(rgb8(238, 236, 232), rgb8(198, 198, 200), broad * 0.5f + fbm(u, w, 8, 4, 82) * 0.2f);
                         p.color = lerp(p.color, rgb8(102, 104, 112), thin * 0.8f);
                         p.height = 0.5f;
                         p.rough = 0.1f + thin * 0.1f;
                         return p;
                     }, {}});
        v.push_back({"Ceramic Tile", 0.6, 0.6, 6.f, false, false,
                     [](float u, float w) {
                         const float fx = frac(u * 2), fy = frac(w * 2);
                         const float e = std::min({fx, 1 - fx, fy, 1 - fy});
                         const float tile = box(0.012f, 0.03f, e);
                         const float id = rnd((int)std::floor(u * 2), (int)std::floor(w * 2), 91);
                         Pixel p;
                         p.color = lerp(rgb8(128, 124, 116), lerp(rgb8(226, 224, 218), rgb8(238, 236, 230), id), tile);
                         p.height = tile * 0.8f + box(0.03f, 0.07f, e) * 0.1f;
                         p.rough = mixf(0.9f, 0.12f + fbm(u, w, 16, 2, 92) * 0.08f, tile);
                         return p;
                     }, {}});
        v.push_back({"Stone Wall", 1.5, 1.5, 12.f, true, false,
                     [](float u, float w) {
                         // Rounded stones: distance to the nearest and second nearest cell point.
                         constexpr int kCells = 6;
                         float d1 = 9, d2 = 9;
                         int id = 0;
                         const float wu = u + (fbm(u, w, 8, 2, 101) - 0.5f) * 0.03f, wv = w + (fbm(u, w, 8, 2, 102) - 0.5f) * 0.03f;
                         const int cx = (int)std::floor(wu * kCells), cy = (int)std::floor(wv * kCells);
                         for (int y = cy - 1; y <= cy + 1; ++y)
                             for (int x = cx - 1; x <= cx + 1; ++x) {
                                 const int hx = ((x % kCells) + kCells) % kCells, hy = ((y % kCells) + kCells) % kCells;
                                 const float px = (x + 0.2f + rnd(hx, hy, 103) * 0.6f) / kCells, py = (y + 0.2f + rnd(hx, hy, 104) * 0.6f) / kCells;
                                 const float d = std::hypot((wu - px) * 1.3f, wv - py);
                                 if (d < d1) {
                                     d2 = d1;
                                     d1 = d;
                                     id = hash2(hx, hy, 105) & 255;
                                 } else if (d < d2) {
                                     d2 = d;
                                 }
                             }
                         const float e = (d2 - d1) * kCells;
                         const float stone = box(0.04f, 0.2f, e);
                         const float g = fbm(u, w, 32, 4, 106);
                         Pixel p;
                         const Rgb c = lerp(lerp(rgb8(124, 116, 104), rgb8(170, 160, 142), id / 255.f), rgb8(96, 90, 82), g * 0.5f);
                         p.color = lerp(rgb8(70, 66, 60), c, stone);
                         p.height = stone * (0.55f + std::min(e, 0.6f) * 0.4f + g * 0.15f);
                         p.rough = 0.8f + g * 0.2f;
                         return p;
                     }, {}});
        v.push_back({"Asphalt", 2.0, 2.0, 7.f, false, false,
                     [](float u, float w) {
                         Pixel p;
                         const float a = noise(u, w, 180, 111), big = fbm(u, w, 4, 3, 112);
                         p.color = lerp(rgb8(36, 36, 38), rgb8(70, 70, 72), big * 0.5f + a * 0.5f);
                         p.color = lerp(p.color, rgb8(128, 126, 122), box(0.88f, 0.94f, a));
                         p.height = a;
                         p.rough = 0.85f + a * 0.15f;
                         return p;
                     }, {}});
        v.push_back({"Slate Roof", 1.2, 1.2, 12.f, true, false,
                     [](float u, float w) {
                         constexpr int kRows = 8, kCols = 5;
                         const float y = w * kRows;
                         const int row = (int)std::floor(y);
                         const float x = u * kCols + (row % 2 ? 0.5f : 0.f);
                         const int col = (int)std::floor(x);
                         const float fx = x - col, fy = y - row;
                         const float id = rnd(((col % kCols) + kCols) % kCols, row, 121);
                         const float g = fbm(u, w, 32, 3, 122);
                         const float side = box(0.0f, 0.04f, std::min(fx, 1 - fx));
                         Pixel p;
                         p.color = lerp(rgb8(50, 54, 60), rgb8(86, 90, 96), id * 0.7f + g * 0.3f);
                         p.color = lerp(rgb8(24, 24, 26), p.color, side * box(0.0f, 0.05f, fy));
                         // Each slate rises towards its lower edge, over the next row.
                         p.height = (0.2f + fy * 0.7f + g * 0.1f) * side;
                         p.rough = 0.55f + g * 0.3f;
                         return p;
                     }, {}});
        v.push_back({"Corrugated Metal", 1.0, 1.0, 5.f, false, true,
                     [](float u, float w) {
                         Pixel p;
                         const float wave = 0.5f + 0.5f * std::sin(u * 12 * 6.283f);
                         const float rust = box(0.58f, 0.68f, fbm(u, w, 6, 5, 131));
                         p.color = lerp(rgb8(168, 172, 174), rgb8(112, 62, 38), rust);
                         p.height = wave;
                         p.rough = mixf(0.42f, 0.9f, rust);
                         p.metal = 1 - rust;
                         return p;
                     }, {}});
        v.push_back({"Brushed Steel", 1.0, 1.0, 0.6f, false, true,
                     [](float u, float w) {
                         Pixel p;
                         const float n = fbm(u * 0.03125f, w, 128, 3, 141);
                         p.color = lerp(rgb8(168, 170, 172), rgb8(200, 202, 204), n);
                         p.height = n;
                         p.rough = 0.3f + n * 0.2f;
                         p.metal = 1;
                         return p;
                     }, {}});
        return v;
    }();
    return all;
}

struct Slot {
    std::once_flag once;
    Material material;
};
std::vector<Slot>& allSlots() {
    static std::vector<Slot> s(specs().size());
    return s;
}

} // namespace

int architecturalMaterialCount() { return (int)specs().size(); }
std::string architecturalMaterialName(int index) { return specs()[index].name; }

const Material& architecturalMaterial(int index) {
    Slot& slot = allSlots()[index];
    std::call_once(slot.once, [&] { build(specs()[index], slot.material); });
    return slot.material;
}

void prepareArchitecturalMaterials() {
    static std::once_flag started;
    std::call_once(started, [] {
        for (int i = 0; i < architecturalMaterialCount(); ++i) QThreadPool::globalInstance()->start([i] { architecturalMaterial(i); });
    });
}

} // namespace gs
