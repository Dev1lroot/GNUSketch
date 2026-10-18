// SPDX-License-Identifier: GPL-3.0-or-later
#include "Renderer.h"

#include "core/Triangulate.h"
#include "io/ImageUtil.h"

#include <QOpenGLShaderProgram>

#include <algorithm>
#include <functional>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace gs {

namespace {

const char* kFaceVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUvF;
layout(location=3) in vec2 aUvB;
layout(location=4) in vec4 aColF;
layout(location=5) in vec4 aColB;
uniform mat4 uMVP;
out vec3 vN; out vec2 vUvF; out vec2 vUvB; out vec4 vColF; out vec4 vColB;
void main() {
    vN = aNormal; vUvF = aUvF; vUvB = aUvB; vColF = aColF; vColB = aColB;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kFaceFS = R"(#version 330 core
in vec3 vN; in vec2 vUvF; in vec2 vUvB; in vec4 vColF; in vec4 vColB;
uniform bool uTexF; uniform bool uTexB;
uniform sampler2D uSamF; uniform sampler2D uSamB;
uniform int uMode;          // 0 shaded, 1 textured, 2 hidden line, 3 monochrome
uniform vec4 uBg; uniform vec4 uMonoF; uniform vec4 uMonoB;
uniform vec3 uLight; uniform float uAlpha;
out vec4 fragColor;
void main() {
    bool front = gl_FrontFacing;
    vec3 n = normalize(vN);
    if (!front) n = -n;
    if (uMode == 2) { fragColor = vec4(uBg.rgb, uAlpha); return; }
    vec4 base = front ? vColF : vColB;
    if (uMode == 1) {
        if (front && uTexF) base = vec4(texture(uSamF, vUvF).rgb, base.a);
        if (!front && uTexB) base = vec4(texture(uSamB, vUvB).rgb, base.a);
    }
    if (uMode == 3) base = front ? uMonoF : uMonoB;
    float diff = max(dot(n, uLight), 0.0);
    float light = 0.60 + 0.40 * diff;
    fragColor = vec4(base.rgb * light, base.a * uAlpha);
}
)";

const char* kLineVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
uniform mat4 uMVP;
out vec4 vCol;
void main() { vCol = aCol; gl_Position = uMVP * vec4(aPos, 1.0); }
)";

// Expands lines into screen-space quads (core profile has no wide lines).
const char* kLineGS = R"(#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
in vec4 vCol[];
out vec4 gCol;
out float gDist;
uniform vec2 uViewport;
uniform float uWidth;
void main() {
    vec4 p0 = gl_in[0].gl_Position, p1 = gl_in[1].gl_Position;
    float e = 1e-5;
    // Clip against the near plane (w > 0).
    if (p0.w < e && p1.w < e) return;
    if (p0.w < e) { float t = (e - p0.w) / (p1.w - p0.w); p0 = mix(p0, p1, t); }
    if (p1.w < e) { float t = (e - p1.w) / (p0.w - p1.w); p1 = mix(p1, p0, t); }
    vec2 s0 = (p0.xy / p0.w) * 0.5 * uViewport;
    vec2 s1 = (p1.xy / p1.w) * 0.5 * uViewport;
    vec2 d = s1 - s0;
    float len = length(d);
    vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x) * (uWidth * 0.5);
    vec2 ext = dir * (uWidth * 0.25);
    vec2 o0 = (nrm - ext) / (0.5 * uViewport), o1 = (nrm + ext) / (0.5 * uViewport);
    vec2 o0n = (-nrm - ext) / (0.5 * uViewport), o1n = (-nrm + ext) / (0.5 * uViewport);
    gCol = vCol[0]; gDist = 0.0;
    gl_Position = vec4(p0.xy + o0 * p0.w, p0.z, p0.w); EmitVertex();
    gCol = vCol[0]; gDist = 0.0;
    gl_Position = vec4(p0.xy + o0n * p0.w, p0.z, p0.w); EmitVertex();
    gCol = vCol[1]; gDist = len;
    gl_Position = vec4(p1.xy + o1 * p1.w, p1.z, p1.w); EmitVertex();
    gCol = vCol[1]; gDist = len;
    gl_Position = vec4(p1.xy + o1n * p1.w, p1.z, p1.w); EmitVertex();
    EndPrimitive();
}
)";

const char* kLineFS = R"(#version 330 core
in vec4 gCol;
in float gDist;
uniform int uDash;
uniform float uDpr;
out vec4 fragColor;
void main() {
    if (uDash == 1 && mod(gDist, 10.0 * uDpr) > 6.0 * uDpr) discard;
    if (uDash == 2 && mod(gDist, 5.0 * uDpr) > 2.0 * uDpr) discard;
    fragColor = gCol;
}
)";

const char* kSkyVS = R"(#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vNdc = p;
    gl_Position = vec4(p, 0.999999, 1.0);
}
)";

const char* kSkyFS = R"(#version 330 core
in vec2 vNdc;
uniform mat4 uInvVP;
uniform vec4 uSky; uniform vec4 uHorizon; uniform vec4 uGround; uniform vec4 uBg;
uniform bool uUseSky; uniform bool uUseGround;
out vec4 fragColor;
void main() {
    vec4 a = uInvVP * vec4(vNdc, -1.0, 1.0);
    vec4 b = uInvVP * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    if (dir.z >= 0.0) {
        if (!uUseSky) { fragColor = uBg; return; }
        float t = pow(clamp(dir.z, 0.0, 1.0), 0.45);
        fragColor = mix(uHorizon, uSky, t);
    } else {
        if (!uUseGround) { fragColor = uUseSky ? uHorizon : uBg; return; }
        float t = clamp(-dir.z * 6.0, 0.0, 1.0);
        fragColor = mix(mix(uGround, uHorizon, 0.35), uGround, t);
    }
}
)";

const char* kFlatVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* kFlatFS = R"(#version 330 core
uniform vec4 uColor;
uniform bool uStipple;
out vec4 fragColor;
void main() {
    if (uStipple) {
        ivec2 p = ivec2(gl_FragCoord.xy);
        int row = p.y % 4;
        if (row == 1 || row == 3) discard;
        if (((p.x + (row == 2 ? 2 : 0)) % 4) != 0) discard;
    }
    fragColor = uColor;
}
)";

void setColor(QOpenGLShaderProgram* p, const char* name, const Color& c) { p->setUniformValue(name, c.r, c.g, c.b, c.a); }

void setMat(QOpenGLShaderProgram* p, const char* name, const Mat4& m) {
    float f[16];
    m.toFloat(f);
    p->setUniformValue(name, QMatrix4x4(f).transposed());
}

constexpr int kFaceStride = 18; // floats per face vertex

} // namespace

Color Renderer::axisColor(int axis) {
    switch (axis) {
    case 0: return Color::rgb8(230, 0, 0);
    case 1: return Color::rgb8(0, 170, 0);
    default: return Color::rgb8(0, 0, 230);
    }
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::initialize(QString* log) {
    if (!initializeOpenGLFunctions()) {
        if (log) *log = "OpenGL 3.3 core functions unavailable";
        return false;
    }
    auto make = [&](std::unique_ptr<QOpenGLShaderProgram>& prog, const char* vs, const char* gs, const char* fs) {
        prog = std::make_unique<QOpenGLShaderProgram>();
        bool ok = prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vs);
        if (gs) ok = ok && prog->addShaderFromSourceCode(QOpenGLShader::Geometry, gs);
        ok = ok && prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
        ok = ok && prog->link();
        if (!ok && log) *log += prog->log() + "\n";
        return ok;
    };
    bool ok = make(faceProg_, kFaceVS, nullptr, kFaceFS);
    ok = make(lineProg_, kLineVS, kLineGS, kLineFS) && ok;
    ok = make(skyProg_, kSkyVS, nullptr, kSkyFS) && ok;
    ok = make(flatProg_, kFlatVS, nullptr, kFlatFS) && ok;
    if (!ok) return false;

    glGenVertexArrays(1, &faceVao_);
    glGenBuffers(1, &faceVbo_);
    glBindVertexArray(faceVao_);
    glBindBuffer(GL_ARRAY_BUFFER, faceVbo_);
    const int st = kFaceStride * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, (void*)(10 * sizeof(float)));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, (void*)(14 * sizeof(float)));

    glGenVertexArrays(1, &dynVao_);
    glGenBuffers(1, &dynVbo_);
    glBindVertexArray(dynVao_);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo_);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));

    glGenVertexArrays(1, &flatVao_);
    glGenBuffers(1, &flatVbo_);
    glBindVertexArray(flatVao_);
    glBindBuffer(GL_ARRAY_BUFFER, flatVbo_);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glGenVertexArrays(1, &skyVao_);
    glBindVertexArray(0);
    initialized_ = true;
    return true;
}

void Renderer::cleanup() {
    if (!initialized_) return;
    for (auto& [id, t] : textures_) glDeleteTextures(1, &t.id);
    textures_.clear();
    glDeleteBuffers(1, &faceVbo_);
    glDeleteBuffers(1, &dynVbo_);
    glDeleteBuffers(1, &flatVbo_);
    glDeleteVertexArrays(1, &faceVao_);
    glDeleteVertexArrays(1, &dynVao_);
    glDeleteVertexArrays(1, &flatVao_);
    glDeleteVertexArrays(1, &skyVao_);
    faceProg_.reset();
    lineProg_.reset();
    skyProg_.reset();
    flatProg_.reset();
    initialized_ = false;
}

unsigned Renderer::textureFor(Id mat) const {
    auto it = textures_.find(mat);
    return it == textures_.end() ? 0 : it->second.id;
}

void Renderer::syncTextures(const Document& doc) {
    const auto& mats = doc.state.materials;
    // Drop textures of removed/changed materials.
    for (auto it = textures_.begin(); it != textures_.end();) {
        Id m = it->first;
        bool keep = m < (Id)mats.size() && mats[m].hasTexture() && mats[m].texture.revision == it->second.revision;
        if (!keep) {
            glDeleteTextures(1, &it->second.id);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
    for (Id m = 0; m < (Id)mats.size(); ++m) {
        if (!mats[m].hasTexture() || textures_.count(m)) continue;
        QImage img = decodeTexture(mats[m].texture);
        if (img.isNull()) continue;
        // Limit size for GPU memory; keep power-of-two friendly mipmapping.
        if (img.width() > 4096 || img.height() > 4096)
            img = img.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        img = img.convertToFormat(QImage::Format_RGBA8888);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        img = img.flipped(Qt::Vertical);
#else
        img = img.mirrored(false, true);
#endif
        GpuTex t;
        t.revision = mats[m].texture.revision;
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        textures_[m] = t;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::rebuildFaces(const Document& doc) {
    const Mesh& m = doc.mesh();
    const RenderStyle& st = doc.style;
    faceTris_.assign(m.faces.size(), {});

    // Smooth normals: group faces around each vertex connected through
    // soft/smooth edges and average their normals.
    std::unordered_map<uint64_t, Vec3> smoothN; // key (vertex, face)
    auto key = [](Id v, Id f) { return (uint64_t(uint32_t(v)) << 32) | uint32_t(f); };
    {
        std::vector<char> smoothVertex(m.vertices.size(), 0);
        for (const Edge& e : m.edges)
            if (e.alive && (e.soft || e.smooth)) smoothVertex[e.v[0]] = smoothVertex[e.v[1]] = 1;
        for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
            if (!smoothVertex[v] || !m.vertices[v].alive) continue;
            std::vector<Id> fs;
            for (Id e : m.vertexEdges(v))
                for (Id f : m.edgeFaces(e))
                    if (m.faces[f].alive && std::find(fs.begin(), fs.end(), f) == fs.end()) fs.push_back(f);
            if (fs.size() < 2) continue;
            std::vector<int> parent(fs.size());
            std::iota(parent.begin(), parent.end(), 0);
            std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
            auto idx = [&](Id f) { return int(std::find(fs.begin(), fs.end(), f) - fs.begin()); };
            for (Id e : m.vertexEdges(v)) {
                const Edge& E = m.edges[e];
                if (!E.alive || !(E.soft || E.smooth)) continue;
                const auto& ef = m.edgeFaces(e);
                if (ef.size() != 2) continue;
                // Only smooth if the two faces are consistently oriented.
                if (m.faces[ef[0]].normal.dot(m.faces[ef[1]].normal) < -0.2) continue;
                parent[find(idx(ef[0]))] = find(idx(ef[1]));
            }
            std::unordered_map<int, Vec3> sum;
            for (size_t i = 0; i < fs.size(); ++i) sum[find((int)i)] += m.faces[fs[i]].normal * m.faceArea(fs[i]) + m.faces[fs[i]].normal * 1e-9;
            for (size_t i = 0; i < fs.size(); ++i) smoothN[key(v, fs[i])] = sum[find((int)i)].normalized();
        }
    }

    struct Entry {
        std::vector<float> data;
        Batch b;
    };
    std::map<std::tuple<int, int, int>, Entry> groups;
    auto colorOf = [&](Id mat, bool front) {
        const Material* mm = doc.material(mat);
        if (!mm) return front ? st.frontColor : st.backColor;
        Color c = mm->color;
        c.a = mm->opacity;
        return c;
    };

    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        Vec3 u, v;
        planeBasis(F.normal, u, v);
        std::vector<std::vector<Vec2>> loops;
        std::vector<Id> flat;
        auto add = [&](const std::vector<Id>& L) {
            std::vector<Vec2> l;
            for (Id id : L) {
                l.emplace_back(m.pos(id).dot(u), m.pos(id).dot(v));
                flat.push_back(id);
            }
            loops.push_back(std::move(l));
        };
        add(F.outer);
        for (const auto& h : F.holes) add(h);
        auto tris = triangulatePolygon(loops);
        if (tris.empty()) continue;

        const Material* mf = doc.material(F.front.material);
        const Material* mb = doc.material(F.back.material);
        bool texF = mf && mf->hasTexture();
        bool texB = mb && mb->hasTexture();
        Color cf = colorOf(F.front.material, true), cb = colorOf(F.back.material, false);
        bool transparent = cf.a < 0.999f || cb.a < 0.999f;
        auto gk = std::make_tuple(texF ? F.front.material : -1, texB ? F.back.material : -1, transparent ? 1 : 0);
        Entry& en = groups[gk];
        en.b.matFront = texF ? F.front.material : kNone;
        en.b.matBack = texB ? F.back.material : kNone;
        en.b.texFront = texF;
        en.b.texBack = texB;
        en.b.transparent = transparent;

        auto& ft = faceTris_[f];
        for (const auto& t : tris) {
            for (int k : t) {
                Id vid = flat[k];
                const Vec3& p = m.pos(vid);
                Vec3 n = F.normal;
                auto it = smoothN.find(key(vid, f));
                if (it != smoothN.end()) n = it->second;
                Vec2 uf = faceTexCoord(F, true, p, mf), ub = faceTexCoord(F, false, p, mb);
                float vals[kFaceStride] = {(float)p.x, (float)p.y, (float)p.z, (float)n.x, (float)n.y, (float)n.z,
                                           (float)uf.x, (float)uf.y, (float)ub.x, (float)ub.y,
                                           cf.r, cf.g, cf.b, cf.a, cb.r, cb.g, cb.b, cb.a};
                en.data.insert(en.data.end(), vals, vals + kFaceStride);
                ft.push_back((float)p.x);
                ft.push_back((float)p.y);
                ft.push_back((float)p.z);
            }
        }
    }

    std::vector<float> all;
    batches_.clear();
    // Opaque first, transparent last.
    for (int pass = 0; pass < 2; ++pass)
        for (auto& [k, en] : groups) {
            if (en.b.transparent != (pass == 1)) continue;
            en.b.first = (int)(all.size() / kFaceStride);
            en.b.count = (int)(en.data.size() / kFaceStride);
            all.insert(all.end(), en.data.begin(), en.data.end());
            batches_.push_back(en.b);
        }
    glBindBuffer(GL_ARRAY_BUFFER, faceVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(float)), all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::pushLine(std::vector<float>& d, const Vec3& a, const Vec3& b, const Color& c) {
    float v[14] = {(float)a.x, (float)a.y, (float)a.z, c.r, c.g, c.b, c.a,
                   (float)b.x, (float)b.y, (float)b.z, c.r, c.g, c.b, c.a};
    d.insert(d.end(), v, v + 14);
}

void Renderer::drawLines(const std::vector<float>& data, const Camera& cam, float width, Dash dash, double dpr) {
    if (data.empty()) return;
    lineProg_->bind();
    setMat(lineProg_.get(), "uMVP", cam.viewProjection());
    lineProg_->setUniformValue("uViewport", QVector2D((float)vpW_, (float)vpH_));
    lineProg_->setUniformValue("uWidth", (float)(width * dpr));
    lineProg_->setUniformValue("uDash", (int)dash);
    lineProg_->setUniformValue("uDpr", (float)dpr);
    glBindVertexArray(dynVao_);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, (GLsizei)(data.size() / 7));
    glBindVertexArray(0);
}

void Renderer::drawOverlayFaces(const std::vector<float>& tris, const Camera& cam, const Color& c, bool stipple) {
    if (tris.empty()) return;
    flatProg_->bind();
    setMat(flatProg_.get(), "uMVP", cam.viewProjection());
    setColor(flatProg_.get(), "uColor", c);
    flatProg_->setUniformValue("uStipple", stipple);
    glBindVertexArray(flatVao_);
    glBindBuffer(GL_ARRAY_BUFFER, flatVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tris.size() * sizeof(float)), tris.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(tris.size() / 3));
    glBindVertexArray(0);
}

void Renderer::drawBackground(const Document& doc, const Camera& cam) {
    const RenderStyle& st = doc.style;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    skyProg_->bind();
    setMat(skyProg_.get(), "uInvVP", cam.viewProjection().inverted());
    setColor(skyProg_.get(), "uSky", st.skyColor);
    setColor(skyProg_.get(), "uHorizon", st.horizonColor);
    setColor(skyProg_.get(), "uGround", st.groundColor);
    setColor(skyProg_.get(), "uBg", st.background);
    skyProg_->setUniformValue("uUseSky", st.sky);
    skyProg_->setUniformValue("uUseGround", st.ground);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawFaces(const Document& doc, const Camera& cam) {
    const RenderStyle& st = doc.style;
    if (st.faceStyle == RenderStyle::Wireframe) return;
    int mode = 0;
    switch (st.faceStyle) {
    case RenderStyle::Shaded: mode = 0; break;
    case RenderStyle::ShadedTextures: mode = 1; break;
    case RenderStyle::HiddenLine: mode = 2; break;
    case RenderStyle::Monochrome: mode = 3; break;
    default: break;
    }
    faceProg_->bind();
    setMat(faceProg_.get(), "uMVP", cam.viewProjection());
    faceProg_->setUniformValue("uMode", mode);
    setColor(faceProg_.get(), "uBg", st.background);
    setColor(faceProg_.get(), "uMonoF", Color::rgb8(255, 255, 255));
    setColor(faceProg_.get(), "uMonoB", st.backColor);
    Vec3 L = Vec3(-0.3, -0.6, 0.75).normalized();
    faceProg_->setUniformValue("uLight", QVector3D((float)L.x, (float)L.y, (float)L.z));
    faceProg_->setUniformValue("uSamF", 0);
    faceProg_->setUniformValue("uSamB", 1);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    glBindVertexArray(faceVao_);
    const bool xray = st.xray;
    for (const Batch& b : batches_) {
        bool transparent = b.transparent || xray;
        if (transparent) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        faceProg_->setUniformValue("uAlpha", xray ? 0.5f : 1.0f);
        faceProg_->setUniformValue("uTexF", b.texFront && mode == 1);
        faceProg_->setUniformValue("uTexB", b.texBack && mode == 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, b.texFront ? textureFor(b.matFront) : 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, b.texBack ? textureFor(b.matBack) : 0);
        glDrawArrays(GL_TRIANGLES, b.first, b.count);
        if (transparent) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::render(const Document& doc, const Camera& cam, const Preview& preview, double dpr) {
    if (!initialized_) return;
    const Mesh& m = doc.mesh();
    const RenderStyle& st = doc.style;
    vpW_ = (int)std::lround(cam.width * dpr);
    vpH_ = (int)std::lround(cam.height * dpr);
    glViewport(0, 0, vpW_, vpH_);
    glClearColor(st.background.r, st.background.g, st.background.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    if (builtMatRevision_ != doc.materialsRevision) syncTextures(doc);
    if (builtRevision_ != m.revision || builtMatRevision_ != doc.materialsRevision) {
        rebuildFaces(doc);
        builtRevision_ = m.revision;
        builtMatRevision_ = doc.materialsRevision;
    }

    drawBackground(doc, cam);
    drawFaces(doc, cam);

    // Selection / highlight stipple over faces.
    {
        std::vector<float> sel;
        auto addFace = [&](Id f) {
            if (f >= 0 && f < (Id)faceTris_.size()) sel.insert(sel.end(), faceTris_[f].begin(), faceTris_[f].end());
        };
        for (Id f : doc.selection.faces) addFace(f);
        for (Id f : preview.highlightFaces)
            if (!doc.selection.faces.count(f)) addFace(f);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        drawOverlayFaces(sel, cam, Color::rgb8(0, 0, 220), true);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Edges.
    std::vector<float> normal, profile, selected, hidden;
    const bool persp = cam.perspective;
    const Vec3 eye = cam.eye, fwd = cam.forward();
    auto facing = [&](Id f, const Vec3& p) {
        Vec3 viewDir = persp ? (eye - p) : -fwd;
        return m.faces[f].normal.dot(viewDir) > 0;
    };
    const bool drawEdges = st.edges || st.faceStyle == RenderStyle::Wireframe;
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        const Edge& E = m.edges[e];
        if (!E.alive) continue;
        const Vec3& a = m.pos(E.v[0]);
        const Vec3& b = m.pos(E.v[1]);
        bool isSel = doc.selection.edges.count(e) || preview.highlightEdges.count(e);
        // Silhouette test.
        bool isProfile = false;
        int nf = 0;
        Id f0 = kNone, f1 = kNone;
        for (Id f : m.edgeFaces(e)) {
            if (!m.faces[f].alive || m.faces[f].hidden) continue;
            if (nf == 0) f0 = f; else if (nf == 1) f1 = f;
            ++nf;
        }
        Vec3 mid = (a + b) * 0.5;
        if (nf == 1) isProfile = true;
        else if (nf == 2) isProfile = facing(f0, mid) != facing(f1, mid);
        if (st.faceStyle == RenderStyle::Wireframe) isProfile = false;

        if (E.hidden || E.soft) {
            if (isSel) pushLine(selected, a, b, Color::rgb8(0, 0, 255));
            else if (st.profiles && isProfile && E.soft && !E.hidden && drawEdges) pushLine(profile, a, b, st.edgeColor);
            else if (st.showHidden) pushLine(hidden, a, b, Color::rgb8(110, 110, 110));
            continue;
        }
        if (isSel) {
            pushLine(selected, a, b, Color::rgb8(0, 0, 255));
            continue;
        }
        if (!drawEdges) continue;
        if (st.profiles && isProfile) pushLine(profile, a, b, st.edgeColor);
        else pushLine(normal, a, b, st.edgeColor);
    }

    if (st.xray) glDisable(GL_DEPTH_TEST);
    drawLines(normal, cam, 1.0f, Dash::None, dpr);
    drawLines(profile, cam, st.profileWidth, Dash::None, dpr);
    drawLines(hidden, cam, 1.0f, Dash::Dashed, dpr);
    if (st.backEdges && !st.xray && st.faceStyle != RenderStyle::Wireframe) {
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        std::vector<float> back = normal;
        back.insert(back.end(), profile.begin(), profile.end());
        drawLines(back, cam, 1.0f, Dash::Dashed, dpr);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
    }
    glEnable(GL_DEPTH_TEST);
    drawLines(selected, cam, 1.8f, Dash::None, dpr);

    // Guides.
    if (st.showGuides && !doc.state.guides.empty()) {
        std::vector<float> g;
        for (const Guide& gd : doc.state.guides) {
            if (gd.kind != Guide::Line) continue;
            pushLine(g, gd.p - gd.dir * 2e4, gd.p + gd.dir * 2e4, Color::rgb8(60, 60, 60, 220));
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        drawLines(g, cam, 1.0f, Dash::Dashed, dpr);
        glDisable(GL_BLEND);
    }

    // Axes.
    if (st.showAxes) {
        std::vector<float> pos, neg;
        const double L = 2e4;
        for (int i = 0; i < 3; ++i) {
            Vec3 d;
            d[i] = 1;
            pushLine(pos, Vec3(), d * L, axisColor(i));
            pushLine(neg, Vec3(), d * -L, axisColor(i));
        }
        drawLines(pos, cam, 1.2f, Dash::None, dpr);
        drawLines(neg, cam, 1.0f, Dash::Dotted, dpr);
    }

    // Tool preview.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (const Preview::Poly& p : preview.polys) {
        if (p.pts.size() < 3) continue;
        std::vector<float> tris;
        Vec3 n = newellNormal((int)p.pts.size(), [&](int i) { return p.pts[i]; });
        if (n.length2() < 1e-20) continue;
        Vec3 u, v;
        planeBasis(n, u, v);
        std::vector<std::vector<Vec2>> loops(1);
        for (const Vec3& q : p.pts) loops[0].emplace_back(q.dot(u), q.dot(v));
        for (const auto& t : triangulatePolygon(loops))
            for (int k : t) {
                tris.push_back((float)p.pts[k].x);
                tris.push_back((float)p.pts[k].y);
                tris.push_back((float)p.pts[k].z);
            }
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        drawOverlayFaces(tris, cam, p.color, false);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
    }
    // Group preview lines by (width, dash, depth).
    std::map<std::tuple<float, int, bool>, std::vector<float>> groups;
    for (const Preview::Line& l : preview.lines) pushLine(groups[{l.width, (int)l.dash, l.depthTest}], l.a, l.b, l.color);
    for (auto& [k, data] : groups) {
        if (std::get<2>(k)) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        drawLines(data, cam, std::get<0>(k), (Dash)std::get<1>(k), dpr);
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

} // namespace gs
