// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - OpenGL 3.3 core renderer for the model viewport.
#pragma once

#include "Camera.h"
#include "core/Document.h"

#include <QElapsedTimer>
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
    enum class MarkerKind { Endpoint, Midpoint, OnEdge, OnFace, Origin, Intersection, Guide, Center, Plain, Grip, GripHot };
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
    std::set<Id> highlightInstances;
    bool hideFaceSelection = false; // no selection pattern over the faces (it would hide their texture)
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

    // Forgets per-document GPU data, for rendering a different document next.
    void invalidate();
    // Compiles a custom surface shader; false and the compiler's message (line
    // numbers count from the top of the shader code) when it does not build.
    bool compileSurfaceShader(const std::string& source, QString* log = nullptr);

    // The last frame drew a custom shader that uses _Time: keep repainting.
    bool animated() const { return animated_; }

    // "Real" sun shadows (Game Mode): a sharp map around the camera with soft,
    // contact-hardening edges on top of the whole-model one. radius: meters
    // around the camera it covers.
    void setSoftShadows(bool on, double radius = 40.0) { softShadows_ = on; nearShadowRadius_ = radius; }
    bool softShadows() const { return softShadows_; }

    static Color axisColor(int axis);
    static constexpr int kMaxLights = 16;

private:
    struct Batch {
        int first = 0, count = 0;
        Id matFront = kNone, matBack = kNone;
        bool texFront = false, texBack = false;
        bool transparent = false;
        bool paint = false; // Texture Brush layer, blended over the faces below
        bool advFront = false, advBack = false; // that side uses the PBR renderer
    };
    struct GpuTex {
        unsigned id = 0;
        uint64_t revision = 0;
    };

    // GPU copy of one mesh's triangulated faces (the top level or a component
    // definition, drawn once per instance).
    struct MeshGpu {
        unsigned vao = 0, vbo = 0;
        uint64_t revision = 0, matRevision = 0;
        std::vector<Batch> batches;
        std::vector<std::vector<float>> faceTris; // per face: triangle positions (for overlays)
    };
    // Edge lines collected for one frame.
    struct EdgeLists {
        std::vector<float> normal, profile, selected, hidden, faded, fadedProfile;
    };

    void buildMesh(MeshGpu& g, const Mesh& m, const Document& doc);
    void freeMesh(MeshGpu& g);
    MeshGpu& meshGpu(const Document& doc, Id def); // kNone = top level
    void syncTextures(const Document& doc);
    void drawBackground(const Document& doc, const Camera& cam);
    // fade > 0 draws the mesh washed out and translucent (the rest of the model
    // while a component is open for editing).
    void drawMeshFaces(const MeshGpu& g, const Document& doc, const Camera& cam, const Mat4& model, float fade);
    void collectEdges(const Mesh& m, const Mat4& model, const Document& doc, const Camera& cam, const Preview& preview,
                      bool active, bool faded, EdgeLists& out);
    void drawOverlayFaces(const std::vector<float>& tris, const Camera& cam, const Color& c, bool stipple);
    void drawLines(const std::vector<float>& data, const Camera& cam, float width, Dash dash, double dpr);
    void pushLine(std::vector<float>& d, const Vec3& a, const Vec3& b, const Color& c);
    // slot -1 is the base colour image, otherwise a MapSlot.
    unsigned textureFor(Id mat, int slot = -1) const;
    QOpenGLShaderProgram* surfaceProgram(const Material& m);
    // Draws one side of a batch with the PBR renderer.
    void drawSurfaceSide(const Batch& b, const Material& mat, Id matId, const Camera& cam, const RenderStyle& st, const Mat4& model,
                         float fade, float alpha);
    void updateLights(const ModelState& ms);
    void setLightUniforms(QOpenGLShaderProgram* p);
    void drawObjects(const Document& doc, const Camera& cam, double dpr);
    // Sun shadow map from the given meshes (with their world transforms).
    void renderShadowMap(const std::vector<std::pair<const MeshGpu*, Mat4>>& casters, const Vec3& mn, const Vec3& mx);
    void setShadowUniforms(QOpenGLShaderProgram* p);
    void drawShadowCasters(unsigned fbo, int size, const Mat4& vp, const std::vector<std::pair<const MeshGpu*, Mat4>>& casters);
    bool ensureNearShadowMap();
    void renderNearShadowMap(const std::vector<std::pair<const MeshGpu*, Mat4>>& casters, const Camera& cam, const Vec3& mn, const Vec3& mx);
    void updateSun(const RenderStyle& st);
    // Screen-space ambient occlusion multiplied over the faces drawn so far.
    void drawAmbientOcclusion(const std::vector<std::pair<const MeshGpu*, Mat4>>& meshes, const Camera& cam);
    bool ensureAoTargets(int w, int h);
    void freeAoTargets();

    std::unique_ptr<QOpenGLShaderProgram> faceProg_, lineProg_, skyProg_, flatProg_, depthProg_, aoProg_, aoBlendProg_;
    unsigned aoDepthFbo_ = 0, aoFbo_ = 0, aoDepthTex_ = 0, aoTex_ = 0;
    int aoW_ = 0, aoH_ = 0;
    unsigned shadowFbo_ = 0, shadowTex_ = 0;
    bool shadowsOn_ = false; // shadow map valid for this frame
    Mat4 lightVP_;
    Vec3 lightDir_;                     // towards the light of this frame
    bool sunOn_ = false;                // sun lighting (otherwise the plain modelling light)
    Vec3 sunColor_, skyAmbient_, groundAmbient_;
    double daylight_ = 1;               // brightness of the sky, 0..1
    bool culling_ = false;              // back faces are not drawn this frame
    double shadowBias_ = 0;
    bool softShadows_ = false, nearOn_ = false;
    double nearShadowRadius_ = 40.0, nearBias_ = 0, penumbra_ = 0, shadowSearch_ = 0;
    Mat4 nearVP_;
    unsigned nearFbo_ = 0, nearTex_ = 0, rawSampler_ = 0;
    unsigned dynVao_ = 0, dynVbo_ = 0;    // lines (pos3 + color4)
    unsigned flatVao_ = 0, flatVbo_ = 0;  // overlay triangles (pos3)
    unsigned skyVao_ = 0;
    bool initialized_ = false;
    int vpW_ = 1, vpH_ = 1; // framebuffer size in device pixels

    uint64_t builtMatRevision_ = 0;
    MeshGpu top_;
    std::map<Id, MeshGpu> defs_;
    std::map<std::pair<Id, int>, GpuTex> textures_;
    std::map<std::string, std::unique_ptr<QOpenGLShaderProgram>> surfaceProgs_; // by source; null = does not compile
    std::map<std::string, QString> surfaceLogs_;
    std::map<std::string, std::vector<ShaderProp>> surfaceProps_;
    std::unique_ptr<QOpenGLShaderProgram> pbrProg_; // the standard PBR shader
    QElapsedTimer clock_;
    bool animated_ = false;
    int lightCount_ = 0;
    float lightPos_[kMaxLights * 3] = {}, lightAim_[kMaxLights * 3] = {}, lightColor_[kMaxLights * 3] = {}, lightParam_[kMaxLights * 4] = {};
};

} // namespace gs
