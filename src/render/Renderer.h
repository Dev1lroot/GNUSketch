// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - OpenGL 3.3 core renderer for the model viewport.
#pragma once

#include "Camera.h"
#include "core/Document.h"

#include <QOpenGLFunctions_3_3_Core>
#include <QString>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class QOpenGLShaderProgram;

namespace gs {

enum class Dash { None = 0, Dashed = 1, Dotted = 2 };

// Transient drawing produced by the active tool (rubber bands, ghosts, ...).
struct Preview {
    struct Line {
        Vec3 a, b;
        Color color;
        float width = 1.0f;
        Dash dash = Dash::None;
        bool depthTest = false;
    };
    struct Poly {
        std::vector<Vec3> pts;
        Color color;
    };
    enum class MarkerKind { Endpoint, Midpoint, OnEdge, OnFace, Origin, Intersection, Guide, Center, Plain };
    struct Marker {
        Vec3 p;
        MarkerKind kind = MarkerKind::Plain;
    };
    struct Label {
        Vec3 p;
        std::string text;
    };
    std::vector<Line> lines;
    std::vector<Poly> polys;
    std::vector<Marker> markers;
    std::vector<Label> labels; // drawn next to the cursor-ish world point
    std::set<Id> highlightFaces, highlightEdges;
    // Faces/edges to hide while the tool shows a modified ghost (e.g. move).
    void clear() { *this = Preview(); }
    void line(const Vec3& a, const Vec3& b, const Color& c, float w = 1.0f, Dash d = Dash::None, bool depth = false) {
        lines.push_back({a, b, c, w, d, depth});
    }
};

class Renderer : protected QOpenGLFunctions_3_3_Core {
public:
    Renderer();
    ~Renderer();
    bool initialize(QString* log = nullptr);
    void cleanup();
    void render(const Document& doc, const Camera& cam, const Preview& preview, double dpr);

    static Color axisColor(int axis);

private:
    struct Batch {
        int first = 0, count = 0;
        Id matFront = kNone, matBack = kNone;
        bool texFront = false, texBack = false;
        bool transparent = false;
    };
    struct GpuTex {
        unsigned id = 0;
        uint64_t revision = 0;
    };

    void rebuildFaces(const Document& doc);
    void syncTextures(const Document& doc);
    void drawBackground(const Document& doc, const Camera& cam);
    void drawFaces(const Document& doc, const Camera& cam);
    void drawOverlayFaces(const std::vector<float>& tris, const Camera& cam, const Color& c, bool stipple);
    void drawLines(const std::vector<float>& data, const Camera& cam, float width, Dash dash, double dpr);
    void pushLine(std::vector<float>& d, const Vec3& a, const Vec3& b, const Color& c);
    unsigned textureFor(Id mat) const;

    std::unique_ptr<QOpenGLShaderProgram> faceProg_, lineProg_, skyProg_, flatProg_;
    unsigned faceVao_ = 0, faceVbo_ = 0;
    unsigned dynVao_ = 0, dynVbo_ = 0;    // lines (pos3 + color4)
    unsigned flatVao_ = 0, flatVbo_ = 0;  // overlay triangles (pos3)
    unsigned skyVao_ = 0;
    bool initialized_ = false;
    int vpW_ = 1, vpH_ = 1; // framebuffer size in device pixels

    uint64_t builtRevision_ = 0, builtMatRevision_ = 0;
    std::vector<Batch> batches_;
    std::vector<std::vector<float>> faceTris_; // per face: triangle positions (for overlays)
    std::map<Id, GpuTex> textures_;
};

} // namespace gs
