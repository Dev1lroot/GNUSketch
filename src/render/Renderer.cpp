// SPDX-License-Identifier: GPL-3.0-or-later
#include "Renderer.h"

#include "core/Components.h"
#include "core/Triangulate.h"
#include "io/ImageUtil.h"

#include <QOpenGLShaderProgram>

#include <algorithm>
#include <limits>
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
uniform mat4 uModel;
uniform mat3 uNrm; // normal matrix of the model transform
out vec3 vN; out vec3 vWorld; out vec2 vUvF; out vec2 vUvB; out vec4 vColF; out vec4 vColB;
void main() {
    vN = uNrm * aNormal; vUvF = aUvF; vUvB = aUvB; vColF = aColF; vColB = aColB;
    vWorld = (uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

// Shadow map lookup with a 3x3 PCF kernel (1 = lit, 0 = in shadow). The point
// is pushed along its normal by uShadowBias to avoid self-shadowing acne.
#define GS_SHADOW_GLSL R"(
uniform bool uShadows;
uniform mat4 uLightVP;
uniform float uShadowBias;
uniform sampler2DShadow uShadowMap;
float shadowAt(vec3 p, vec3 n) {
    vec4 c = uLightVP * vec4(p + n * uShadowBias, 1.0);
    vec3 s = c.xyz / c.w * 0.5 + 0.5;
    if (s.x <= 0.0 || s.x >= 1.0 || s.y <= 0.0 || s.y >= 1.0 || s.z >= 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) sum += texture(uShadowMap, vec3(s.xy + vec2(x, y) * texel, s.z));
    return sum / 9.0;
}
)"

// World light sources (no shadows): the radiance arriving at p and the direction
// towards the light.
#define GS_LIGHTS_GLSL R"(
uniform int uLightCount;
uniform vec3 uLightPos[16];
uniform vec3 uLightDir[16];    // towards the light (directional and spot)
uniform vec3 uLightColor[16];
uniform vec4 uLightParam[16];  // type (0 point, 1 spot, 2 directional), range, cos outer, cos inner
vec3 lightAt(int i, vec3 p, out vec3 L) {
    int type = int(uLightParam[i].x + 0.5);
    if (type == 2) { L = uLightDir[i]; return uLightColor[i]; }
    vec3 d = uLightPos[i] - p;
    float dist = max(length(d), 1e-4);
    L = d / dist;
    float k = dist / max(uLightParam[i].y, 1e-3);
    k = clamp(1.0 - k * k * k * k, 0.0, 1.0);
    float a = k * k / max(dist * dist, 0.01);
    if (type == 1) a *= smoothstep(uLightParam[i].z, uLightParam[i].w, dot(L, uLightDir[i]));
    return uLightColor[i] * a;
}
)"

const char* kFaceFS = "#version 330 core\n" GS_SHADOW_GLSL GS_LIGHTS_GLSL R"(
in vec3 vN; in vec3 vWorld; in vec2 vUvF; in vec2 vUvB; in vec4 vColF; in vec4 vColB;
uniform bool uTexF; uniform bool uTexB;
uniform sampler2D uSamF; uniform sampler2D uSamB;
uniform int uMode;          // 0 shaded, 1 textured, 2 hidden line, 3 monochrome
uniform vec4 uBg; uniform vec4 uMonoF; uniform vec4 uMonoB;
uniform vec3 uLight; uniform float uAlpha; uniform float uFade;
uniform bool uSunOn; uniform vec3 uSunColor; uniform vec3 uSkyAmbient; uniform vec3 uGroundAmbient;
out vec4 fragColor;
void main() {
    bool front = gl_FrontFacing;
    vec3 n = normalize(vN);
    if (!front) n = -n;
    if (uMode == 2) { fragColor = vec4(mix(uBg.rgb, vec3(1.0), uFade), uAlpha); return; }
    vec4 base = front ? vColF : vColB;
    if (uMode == 1) {
        if (front && uTexF) base = vec4(texture(uSamF, vUvF).rgb, base.a);
        if (!front && uTexB) base = vec4(texture(uSamB, vUvB).rgb, base.a);
    }
    if (uMode == 3) base = front ? uMonoF : uMonoB;
    float diff = max(dot(n, uLight), 0.0);
    vec3 extra = vec3(0.0);
    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        vec3 c = lightAt(i, vWorld, L);
        extra += c * max(dot(n, L), 0.0);
    }
    if (uSunOn) {
        // Light from the sky above and bounced off the ground, plus the sun.
        vec3 lit = mix(uGroundAmbient, uSkyAmbient, n.z * 0.5 + 0.5);
        float sh = (uShadows && diff > 0.0) ? shadowAt(vWorld, n) : 1.0;
        lit += uSunColor * diff * sh + extra;
        fragColor = vec4(mix(min(base.rgb * lit, vec3(1.0)), vec3(1.0), uFade), base.a * uAlpha);
        return;
    }
    float light = 0.60 + 0.40 * diff;
    if (uShadows) light = 0.50 + 0.50 * diff * (diff > 0.0 ? shadowAt(vWorld, n) : 1.0);
    fragColor = vec4(mix(min(base.rgb * (light + extra), vec3(1.0)), vec3(1.0), uFade), base.a * uAlpha);
}
)";

// Physically based shading (metallic/roughness, GGX). Colours are shaded in
// linear space. A custom shader's code goes between the head and the tail.
const char* kPbrHead = "#version 330 core\n" GS_SHADOW_GLSL GS_LIGHTS_GLSL R"(
in vec3 vN; in vec3 vWorld; in vec2 vUvF; in vec2 vUvB; in vec4 vColF; in vec4 vColB;
out vec4 fragColor;
uniform sampler2D _BaseMap, _NormalMap, _MetallicMap, _RoughnessMap, _OcclusionMap, _HeightMap, _EmissionMap, _OpacityMap;
uniform float _Time;
struct Surface {
    vec3 albedo;     // linear colour
    float metallic;
    float roughness;
    vec3 normal;     // world space, unit length
    vec3 emission;   // linear, added after lighting
    float ao;
    float alpha;
    vec2 uv;
    vec3 worldPos;
    vec3 viewDir;    // towards the viewer
};
)";

const char* kPbrTail = R"(
uniform int uHas[7];        // normal metallic roughness occlusion height emission opacity
uniform float uStrength[7];
uniform bool uHasBase; uniform float uBaseStrength; uniform bool uFlipY; uniform vec3 uEmission; uniform bool uBlend;
uniform vec3 uEye; uniform vec3 uForward; uniform bool uPerspective;
uniform vec3 uLight; uniform float uAlpha; uniform float uFade;
uniform bool uSunOn; uniform vec3 uSunColor; uniform vec3 uSkyAmbient; uniform vec3 uGroundAmbient;
uniform vec3 uEnvSky; uniform vec3 uEnvHorizon; uniform vec3 uEnvGround;
const float PI = 3.14159265;
vec3 lin(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
float distributionGGX(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}
float visibilitySmith(float NoV, float NoL, float a) {
    float k = a * 0.5;
    return 0.25 / max((NoV * (1.0 - k) + k) * (NoL * (1.0 - k) + k), 1e-4);
}
vec3 gSpecular = vec3(0.0); // what the surface reflects, whatever its opacity
vec3 directLight(vec3 radiance, vec3 L, vec3 N, vec3 V, vec3 albedo, vec3 F0, float metallic, float a) {
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(L + V);
    float NoV = max(dot(N, V), 1e-3), NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
    vec3 spec = F * distributionGGX(NoH, a) * visibilitySmith(NoV, NoL, a);
    // Scaled by PI so that a white surface facing a light of 1 comes out at 1.
    gSpecular += spec * PI * radiance * NoL;
    return albedo * (1.0 - metallic) * (1.0 - F) * radiance * NoL;
}
void main() {
    bool front = gl_FrontFacing;
    vec2 uv = front ? vUvF : vUvB;
    vec4 vertexColor = front ? vColF : vColB;
    vec3 Ng = normalize(vN);
    if (!front) Ng = -Ng;
    vec3 V = uPerspective ? normalize(uEye - vWorld) : -uForward;

    // Tangent frame from the screen-space derivatives of position and uv.
    vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2p = cross(dp2, Ng), dp1p = cross(Ng, dp1);
    vec3 T = dp2p * duv1.x + dp1p * duv2.x;
    vec3 B = dp2p * duv1.y + dp1p * duv2.y;
    float scale = sqrt(max(dot(T, T), dot(B, B)));
    bool frame = scale > 1e-12;
    if (frame) { T /= scale; B /= scale; }

    // Height: parallax occlusion in the direction of the viewer.
    if (frame && uHas[4] != 0 && uStrength[4] > 0.0) {
        vec3 tv = vec3(dot(V, T), dot(V, B), dot(V, Ng));
        const int steps = 24;
        float layer = 1.0 / float(steps);
        vec2 delta = tv.xy / max(tv.z, 0.2) * uStrength[4] * 0.05 * layer;
        float depth = 0.0;
        float h = 1.0 - textureGrad(_HeightMap, uv, duv1, duv2).r;
        for (int i = 0; i < steps && depth < h; ++i) {
            uv -= delta;
            depth += layer;
            h = 1.0 - textureGrad(_HeightMap, uv, duv1, duv2).r;
        }
    }

    Surface s;
    s.uv = uv;
    s.worldPos = vWorld;
    s.viewDir = V;
    vec3 base = vertexColor.rgb;
    float baseAlpha = 1.0;
    if (uHasBase) {
        vec4 t = texture(_BaseMap, uv);
        base = mix(base, t.rgb, uBaseStrength);
        if (uBlend) baseAlpha = mix(1.0, t.a, uBaseStrength);
    }
    s.albedo = lin(base);
    s.metallic = uStrength[1] * (uHas[1] != 0 ? texture(_MetallicMap, uv).r : 1.0);
    s.roughness = uStrength[2] * (uHas[2] != 0 ? texture(_RoughnessMap, uv).r : 1.0);
    s.normal = Ng;
    if (frame && uHas[0] != 0) {
        vec3 t = texture(_NormalMap, uv).xyz * 2.0 - 1.0;
        if (uFlipY) t.y = -t.y;
        t.xy *= uStrength[0];
        s.normal = normalize(T * t.x + B * t.y + Ng * max(t.z, 1e-3));
    }
    s.emission = lin(uEmission) * uStrength[5];
    if (uHas[5] != 0) s.emission *= lin(texture(_EmissionMap, uv).rgb);
    s.ao = uHas[3] != 0 ? mix(1.0, texture(_OcclusionMap, uv).r, uStrength[3]) : 1.0;
    s.alpha = vertexColor.a * baseAlpha;
    if (uHas[6] != 0) s.alpha *= mix(1.0, texture(_OpacityMap, uv).r, uStrength[6]);

    surface(s);

    vec3 N = normalize(s.normal);
    float metallic = clamp(s.metallic, 0.0, 1.0);
    float roughness = clamp(s.roughness, 0.045, 1.0);
    float a = roughness * roughness;
    vec3 albedo = max(s.albedo, vec3(0.0));
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NoV = max(dot(N, V), 1e-3);

    // The plain shader adds display colours: the light above the ambient keeps
    // the same total at a surface facing it.
    vec3 ambientDisplay = uSunOn ? mix(uGroundAmbient, uSkyAmbient, N.z * 0.5 + 0.5) : vec3(0.6);
    vec3 sunDisplay = uSunOn ? uSunColor : vec3(0.4);
    vec3 ambient = lin(ambientDisplay);
    vec3 sun = lin(ambientDisplay + sunDisplay) - ambient;
    float sh = (uShadows && dot(Ng, uLight) > 0.0) ? shadowAt(vWorld, Ng) : 1.0;
    vec3 color = directLight(sun * sh, uLight, N, V, albedo, F0, metallic, a);
    for (int i = 0; i < uLightCount; ++i) {
        vec3 L;
        vec3 c = lightAt(i, vWorld, L);
        color += directLight(c, L, N, V, albedo, F0, metallic, a);
    }

    // Image based light from the sky gradient (Karis' analytic split-sum fit).
    vec3 R = reflect(-V, N);
    vec3 env = R.z >= 0.0 ? mix(uEnvHorizon, uEnvSky, pow(clamp(R.z, 0.0, 1.0), 0.45))
                          : mix(uEnvHorizon, uEnvGround, clamp(-R.z * 6.0, 0.0, 1.0));
    env = mix(lin(env), ambient, roughness * roughness);
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    color += albedo * (1.0 - metallic) * ambient * s.ao + s.emission;
    gSpecular += env * (F0 * ab.x + ab.y) * s.ao;

    // Reflections stay at full strength on glass: they lie on top of the surface.
    float alpha = clamp(s.alpha, 0.0, 1.0);
    float reflected = clamp(max(gSpecular.r, max(gSpecular.g, gSpecular.b)), 0.0, 1.0);
    float outAlpha = alpha + reflected * (1.0 - alpha);
    color = (color * alpha + gSpecular) / max(outAlpha, 1e-3);
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    fragColor = vec4(mix(color, vec3(1.0), uFade), outAlpha * uAlpha);
}
)";

QByteArray pbrFragmentSource(const std::string& user) {
    QByteArray src(kPbrHead);
    if (user.empty()) {
        src += "void surface(inout Surface s) {}\n";
    } else {
        // String 0, so that messages count the lines of the user's code.
        src += "#line 1\n";
        src += QByteArray::fromStdString(user);
        src += "\n";
    }
    src += "#line 1000\n";
    src += kPbrTail;
    return src;
}

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

const char* kSkyFS = "#version 330 core\n" GS_SHADOW_GLSL R"(
in vec2 vNdc;
uniform mat4 uInvVP;
uniform vec4 uSky; uniform vec4 uHorizon; uniform vec4 uGround; uniform vec4 uBg;
uniform bool uUseSky; uniform bool uUseGround;
uniform bool uSunOn; uniform vec3 uSunDir; uniform vec3 uSunColor; uniform float uDaylight;
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
    if (uSunOn) {
        // Night falls with the sun; the sky around it glows, the disc burns out.
        float keep = mix(0.14, 1.0, uDaylight);
        if (uUseSky || uUseGround) fragColor.rgb *= keep;
        float c = dot(dir, uSunDir);
        if (uUseSky && dir.z >= 0.0) {
            // A soft glow around a disc brighter than anything near it.
            float halo = pow(max(c, 0.0), 900.0) * 0.45 + pow(max(c, 0.0), 40.0) * 0.14;
            fragColor.rgb += uSunColor * halo;
            float disc = smoothstep(0.99975, 0.99985, c);
            fragColor.rgb = mix(fragColor.rgb, vec3(1.0, 1.0, 0.97), disc * step(0.001, uSunDir.z + 0.03));
        } else if (uUseGround && dir.z < 0.0) {
            fragColor.rgb *= mix(0.85, 1.05, clamp(uSunDir.z * 1.5, 0.0, 1.0));
        }
    }
    // Shadows cast onto the ground plane (z = 0).
    vec3 o = a.xyz / a.w;
    if (uShadows && dir.z < 0.0 && o.z > 0.0) {
        vec3 g = o + dir * (-o.z / dir.z);
        fragColor.rgb *= 1.0 - 0.35 * (1.0 - shadowAt(g, vec3(0.0, 0.0, 1.0)));
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

const char* kDepthVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

const char* kDepthFS = R"(#version 330 core
void main() {}
)";

// Screen-space ambient occlusion: a full-screen triangle, then the occlusion
// estimated from the depth of the faces, then a depth-aware blur multiplied
// over the shaded faces.
const char* kScreenVS = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

#define GS_VIEWPOS_GLSL R"(
uniform sampler2D uDepth;
uniform mat4 uInvProj;
vec3 viewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 p = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}
)"

const char* kAoFS = "#version 330 core\n" GS_VIEWPOS_GLSL R"(
in vec2 vUv;
uniform mat4 uProj;
uniform float uRadius;
uniform vec2 uTexel;
out vec4 fragColor;
const int K = 16;
void main() {
    if (texture(uDepth, vUv).r >= 1.0) { fragColor = vec4(1.0); return; }
    vec3 P = viewPos(vUv);
    // Normal from the neighbour on the flatter side (keeps silhouettes clean).
    vec3 px = viewPos(vUv + vec2(uTexel.x, 0.0)) - P, nx = P - viewPos(vUv - vec2(uTexel.x, 0.0));
    vec3 py = viewPos(vUv + vec2(0.0, uTexel.y)) - P, ny = P - viewPos(vUv - vec2(0.0, uTexel.y));
    vec3 N = normalize(cross(abs(px.z) < abs(nx.z) ? px : nx, abs(py.z) < abs(ny.z) ? py : ny));
    vec3 toEye = length(P) > 0.0 && uProj[3][3] < 0.5 ? -P : vec3(0.0, 0.0, 1.0);
    if (dot(N, toEye) < 0.0) N = -N;
    vec3 T = normalize(cross(N, abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 B = cross(N, T);
    // A 4x4 pixel pattern rotates the samples; the blur pass averages it out.
    ivec2 ip = ivec2(gl_FragCoord.xy) % 4;
    float rnd = float(ip.x * 4 + ip.y) / 16.0;
    float occ = 0.0;
    for (int i = 0; i < K; ++i) {
        float fi = (float(i) + 0.5) / float(K);
        float a = float(i) * 2.39996323 + rnd * 6.2831853;
        float r = sqrt(fi);
        vec3 dir = T * (cos(a) * r) + B * (sin(a) * r) + N * sqrt(max(1.0 - fi, 0.05));
        float s = mix(0.15, 1.0, fract(fi + rnd) * fract(fi + rnd));
        vec3 S = P + dir * (uRadius * s);
        vec4 c = uProj * vec4(S, 1.0);
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;
        if (texture(uDepth, uv).r >= 1.0) continue;
        float sceneZ = viewPos(uv).z;
        float range = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sceneZ), 1e-6));
        occ += (sceneZ >= S.z + uRadius * 0.03 ? 1.0 : 0.0) * range;
    }
    fragColor = vec4(vec3(1.0 - occ / float(K)), 1.0);
}
)";

const char* kAoBlendFS = "#version 330 core\n" GS_VIEWPOS_GLSL R"(
in vec2 vUv;
uniform sampler2D uAo;
uniform vec2 uTexel;
uniform float uStrength;
out vec4 fragColor;
void main() {
    if (texture(uDepth, vUv).r >= 1.0) { fragColor = vec4(1.0); return; }
    float z0 = viewPos(vUv).z;
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y <= 1; ++y)
        for (int x = -2; x <= 1; ++x) {
            vec2 uv = vUv + vec2(float(x) + 0.5, float(y) + 0.5) * uTexel;
            float dz = abs(viewPos(uv).z - z0) / max(abs(z0), 1e-3);
            float w = 1.0 / (1.0 + dz * 200.0);
            sum += texture(uAo, uv).r * w;
            wsum += w;
        }
    float ao = sum / max(wsum, 1e-6);
    fragColor = vec4(vec3(mix(1.0, ao, uStrength)), 1.0);
}
)";

constexpr int kShadowSize = 4096;
const Vec3 kLightDir = Vec3(-0.3, -0.6, 0.75).normalized(); // the plain modelling light

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
    ok = make(depthProg_, kDepthVS, nullptr, kDepthFS) && ok;
    ok = make(aoProg_, kScreenVS, nullptr, kAoFS) && ok;
    ok = make(aoBlendProg_, kScreenVS, nullptr, kAoBlendFS) && ok;
    const QByteArray pbrFs = pbrFragmentSource({});
    ok = make(pbrProg_, kFaceVS, nullptr, pbrFs.constData()) && ok;
    if (!ok) return false;
    clock_.start();

    glGenTextures(1, &shadowTex_);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowSize, kShadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &shadowFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    bool fboOk = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!fboOk) {
        if (log) *log += "shadow framebuffer incomplete\n";
        return false;
    }

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
    freeMesh(top_);
    for (auto& [d, g] : defs_) freeMesh(g);
    defs_.clear();
    glDeleteBuffers(1, &dynVbo_);
    glDeleteBuffers(1, &flatVbo_);
    glDeleteVertexArrays(1, &dynVao_);
    glDeleteVertexArrays(1, &flatVao_);
    glDeleteVertexArrays(1, &skyVao_);
    glDeleteFramebuffers(1, &shadowFbo_);
    glDeleteTextures(1, &shadowTex_);
    shadowFbo_ = shadowTex_ = 0;
    freeAoTargets();
    surfaceProgs_.clear();
    surfaceLogs_.clear();
    surfaceProps_.clear();
    pbrProg_.reset();
    aoProg_.reset();
    aoBlendProg_.reset();
    depthProg_.reset();
    faceProg_.reset();
    lineProg_.reset();
    skyProg_.reset();
    flatProg_.reset();
    initialized_ = false;
}

unsigned Renderer::textureFor(Id mat, int slot) const {
    auto it = textures_.find({mat, slot});
    return it == textures_.end() ? 0 : it->second.id;
}

void Renderer::syncTextures(const Document& doc) {
    const auto& mats = doc.state.materials;
    auto texOf = [&](Id m, int slot) -> const Texture& { return slot < 0 ? mats[m].texture : mats[m].maps[slot].tex; };
    // Drop textures of removed/changed materials.
    for (auto it = textures_.begin(); it != textures_.end();) {
        const auto [m, slot] = it->first;
        bool keep = m < (Id)mats.size() && texOf(m, slot).valid() && texOf(m, slot).revision == it->second.revision;
        if (!keep) {
            glDeleteTextures(1, &it->second.id);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
    for (Id m = 0; m < (Id)mats.size(); ++m)
        for (int slot = -1; slot < MapSlotCount; ++slot) {
            const Texture& tex = texOf(m, slot);
            if (!tex.valid() || textures_.count({m, slot})) continue;
            QImage img = decodeTexture(tex);
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
            t.revision = tex.revision;
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            textures_[{m, slot}] = t;
        }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::invalidate() {
    top_.revision = 0;
    for (auto& [d, g] : defs_) g.revision = 0;
    builtMatRevision_ = 0;
}

bool Renderer::compileSurfaceShader(const std::string& source, QString* log) {
    if (!initialized_) {
        if (log) *log = QStringLiteral("The renderer is not ready.");
        return false;
    }
    auto it = surfaceProgs_.find(source);
    if (it == surfaceProgs_.end()) {
        auto prog = std::make_unique<QOpenGLShaderProgram>();
        const QByteArray fs = pbrFragmentSource(source);
        bool ok = prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kFaceVS);
        ok = ok && prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fs.constData());
        ok = ok && prog->link();
        if (!ok) {
            surfaceLogs_[source] = prog->log().trimmed();
            prog.reset();
        }
        it = surfaceProgs_.emplace(source, std::move(prog)).first;
    }
    if (log) *log = it->second ? QString() : surfaceLogs_[source];
    return it->second != nullptr;
}

QOpenGLShaderProgram* Renderer::surfaceProgram(const Material& m) {
    if (!m.customShader()) return pbrProg_.get();
    // A shader that does not compile falls back to the standard look.
    return compileSurfaceShader(m.shader) ? surfaceProgs_[m.shader].get() : pbrProg_.get();
}

void Renderer::freeMesh(MeshGpu& g) {
    if (g.vbo) glDeleteBuffers(1, &g.vbo);
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    g = MeshGpu();
}

Renderer::MeshGpu& Renderer::meshGpu(const Document& doc, Id def) {
    MeshGpu& g = def == kNone ? top_ : defs_[def];
    const Mesh& m = def == kNone ? doc.state.mesh : doc.state.defs[def].mesh;
    if (g.revision != m.revision || g.matRevision != doc.materialsRevision) {
        buildMesh(g, m, doc);
        g.revision = m.revision;
        g.matRevision = doc.materialsRevision;
    }
    return g;
}

void Renderer::buildMesh(MeshGpu& g, const Mesh& m, const Document& doc) {
    const RenderStyle& st = doc.style;
    if (!g.vao) {
        glGenVertexArrays(1, &g.vao);
        glGenBuffers(1, &g.vbo);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        const int stride = kFaceStride * sizeof(float);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
        glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(10 * sizeof(float)));
        glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)(14 * sizeof(float)));
        glBindVertexArray(0);
    }
    g.faceTris.assign(m.faces.size(), {});

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
    std::map<Id, Entry> paintGroups; // per painted material, drawn in id order
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
        // Triangles wind around the front side, which back face culling relies on.
        for (auto& t : tris) {
            const Vec3 n3 = (m.pos(flat[t[1]]) - m.pos(flat[t[0]])).cross(m.pos(flat[t[2]]) - m.pos(flat[t[0]]));
            if (n3.dot(F.normal) < 0) std::swap(t[1], t[2]);
        }

        const Material* mf = doc.material(F.front.material);
        const Material* mb = doc.material(F.back.material);
        bool texF = mf && mf->hasTexture();
        bool texB = mb && mb->hasTexture();
        const bool advF = mf && mf->advanced(), advB = mb && mb->advanced();
        Color cf = colorOf(F.front.material, true), cb = colorOf(F.back.material, false);
        bool transparent = cf.a < 0.999f || cb.a < 0.999f || (mf && mf->blended()) || (mb && mb->blended());
        const bool keyF = texF || advF, keyB = texB || advB;
        auto gk = std::make_tuple(keyF ? F.front.material : -1, keyB ? F.back.material : -1, transparent ? 1 : 0);
        Entry& en = groups[gk];
        en.b.matFront = keyF ? F.front.material : kNone;
        en.b.matBack = keyB ? F.back.material : kNone;
        en.b.texFront = texF;
        en.b.texBack = texB;
        en.b.advFront = advF;
        en.b.advBack = advB;
        en.b.transparent = transparent;

        auto& ft = g.faceTris[f];
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

        // Painted textures: one blended layer per material over this face.
        std::vector<Id> layers;
        for (Id vid : flat)
            for (const PaintWeight& w : m.vertices[vid].paint)
                if (std::find(layers.begin(), layers.end(), w.material) == layers.end()) layers.push_back(w.material);
        for (Id L : layers) {
            // Layers are composited over each other in material id order, so a
            // vertex's normalised weights turn into "over" opacities.
            auto alphaAt = [&](Id vid) {
                const auto& P = m.vertices[vid].paint;
                double rest = 1;
                for (const PaintWeight& w : P) rest -= w.weight;
                double cum = std::max(rest, 0.0);
                for (Id mat = -1; ; ) {
                    // next material id above `mat` present at this vertex
                    Id next = std::numeric_limits<Id>::max();
                    float wn = 0;
                    for (const PaintWeight& w : P)
                        if (w.material > mat && w.material < next) { next = w.material; wn = w.weight; }
                    if (next == std::numeric_limits<Id>::max()) return 0.0f;
                    cum += wn;
                    if (next == L) return cum > 1e-9 ? (float)(wn / cum) : 0.0f;
                    mat = next;
                }
            };
            const Material* ml = doc.material(L);
            const bool tex = ml && ml->hasTexture();
            Color c = colorOf(L, true);
            Entry& pe = paintGroups[L];
            const bool adv = ml && ml->advanced();
            pe.b.matFront = pe.b.matBack = (tex || adv) ? L : kNone;
            pe.b.texFront = pe.b.texBack = tex;
            pe.b.advFront = pe.b.advBack = adv;
            pe.b.paint = true;
            for (const auto& t : tris) {
                float a[3];
                for (int i = 0; i < 3; ++i) a[i] = alphaAt(flat[t[i]]);
                if (a[0] <= 0 && a[1] <= 0 && a[2] <= 0) continue;
                for (int i = 0; i < 3; ++i) {
                    Id vid = flat[t[i]];
                    const Vec3& p = m.pos(vid);
                    Vec3 n = F.normal;
                    auto it = smoothN.find(key(vid, f));
                    if (it != smoothN.end()) n = it->second;
                    Vec2 uf = projectedTexCoord(F, true, p, ml), ub = projectedTexCoord(F, false, p, ml);
                    float ca = c.a * a[i];
                    float vals[kFaceStride] = {(float)p.x, (float)p.y, (float)p.z, (float)n.x, (float)n.y, (float)n.z,
                                               (float)uf.x, (float)uf.y, (float)ub.x, (float)ub.y,
                                               c.r, c.g, c.b, ca, c.r, c.g, c.b, ca};
                    pe.data.insert(pe.data.end(), vals, vals + kFaceStride);
                }
            }
        }
    }

    std::vector<float> all;
    g.batches.clear();
    auto pushBatch = [&](Entry& en) {
        en.b.first = (int)(all.size() / kFaceStride);
        en.b.count = (int)(en.data.size() / kFaceStride);
        all.insert(all.end(), en.data.begin(), en.data.end());
        g.batches.push_back(en.b);
    };
    // Opaque first, then painted layers over them, transparent last.
    for (auto& [k, en] : groups)
        if (!en.b.transparent) pushBatch(en);
    for (auto& [mat, en] : paintGroups)
        if (!en.data.empty()) pushBatch(en);
    for (auto& [k, en] : groups)
        if (en.b.transparent) pushBatch(en);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
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
    skyProg_->setUniformValue("uSunOn", sunOn_);
    skyProg_->setUniformValue("uSunDir", QVector3D((float)lightDir_.x, (float)lightDir_.y, (float)lightDir_.z));
    skyProg_->setUniformValue("uSunColor", QVector3D((float)sunColor_.x, (float)sunColor_.y, (float)sunColor_.z));
    skyProg_->setUniformValue("uDaylight", (float)daylight_);
    setShadowUniforms(skyProg_.get());
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::drawMeshFaces(const MeshGpu& g, const Document& doc, const Camera& cam, const Mat4& model, float fade) {
    const RenderStyle& st = doc.style;
    if (st.faceStyle == RenderStyle::Wireframe || g.batches.empty()) return;
    int mode = 0;
    switch (st.faceStyle) {
    case RenderStyle::Shaded: mode = 0; break;
    case RenderStyle::ShadedTextures: mode = 1; break;
    case RenderStyle::HiddenLine: mode = 2; break;
    case RenderStyle::Monochrome: mode = 3; break;
    default: break;
    }
    faceProg_->bind();
    setMat(faceProg_.get(), "uMVP", cam.viewProjection() * model);
    setMat(faceProg_.get(), "uModel", model);
    setShadowUniforms(faceProg_.get());
    setLightUniforms(faceProg_.get());
    {
        // Normal matrix: inverse-transpose of the linear part.
        Mat4 inv = model.inverted();
        float n[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) n[r * 3 + c] = (float)inv.at(c, r); // row-major
        faceProg_->setUniformValue("uNrm", QMatrix3x3(n));
    }
    faceProg_->setUniformValue("uFade", fade > 0 ? 0.55f : 0.0f);
    faceProg_->setUniformValue("uMode", mode);
    setColor(faceProg_.get(), "uBg", st.background);
    setColor(faceProg_.get(), "uMonoF", Color::rgb8(255, 255, 255));
    setColor(faceProg_.get(), "uMonoB", st.backColor);
    const Vec3& L = lightDir_;
    faceProg_->setUniformValue("uLight", QVector3D((float)L.x, (float)L.y, (float)L.z));
    faceProg_->setUniformValue("uSunOn", sunOn_);
    faceProg_->setUniformValue("uSunColor", QVector3D((float)sunColor_.x, (float)sunColor_.y, (float)sunColor_.z));
    faceProg_->setUniformValue("uSkyAmbient", QVector3D((float)skyAmbient_.x, (float)skyAmbient_.y, (float)skyAmbient_.z));
    faceProg_->setUniformValue("uGroundAmbient", QVector3D((float)groundAmbient_.x, (float)groundAmbient_.y, (float)groundAmbient_.z));
    faceProg_->setUniformValue("uSamF", 0);
    faceProg_->setUniformValue("uSamB", 1);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    // A mirroring transform flips the triangle winding.
    const bool mirrored = model.det3() < 0;
    if (mirrored) glFrontFace(GL_CW);
    const bool cull = culling_ && fade <= 0;
    if (cull) glEnable(GL_CULL_FACE);
    glBindVertexArray(g.vao);
    const bool xray = st.xray;
    for (const Batch& b : g.batches) {
        // Paint layers only show in the shaded styles.
        if (b.paint && (mode == 2 || mode == 3)) continue;
        bool transparent = b.transparent || b.paint || xray || fade > 0;
        if (transparent) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the framebuffer itself stays opaque
            glDepthMask(GL_FALSE);
        }
        const float alpha = (xray ? 0.5f : 1.0f) * (fade > 0 ? fade : 1.0f);
        const Material* advF = mode == 1 && b.advFront ? doc.material(b.matFront) : nullptr;
        const Material* advB = mode == 1 && b.advBack ? doc.material(b.matBack) : nullptr;
        if (advF || advB) {
            // Each side on its own: the PBR renderer works one material at a time.
            for (const bool front : {false, true}) { // far side first, for what is see-through
                if (!front && cull) continue;
                glEnable(GL_CULL_FACE);
                glCullFace(front ? GL_BACK : GL_FRONT);
                if (const Material* adv = front ? advF : advB) {
                    drawSurfaceSide(b, *adv, front ? b.matFront : b.matBack, cam, st, model, fade, alpha);
                    continue;
                }
                faceProg_->bind();
                faceProg_->setUniformValue("uAlpha", alpha);
                faceProg_->setUniformValue("uTexF", front && b.texFront);
                faceProg_->setUniformValue("uTexB", !front && b.texBack);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, b.texFront ? textureFor(b.matFront) : 0);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, b.texBack ? textureFor(b.matBack) : 0);
                glDrawArrays(GL_TRIANGLES, b.first, b.count);
            }
            glCullFace(GL_BACK);
            if (!cull) glDisable(GL_CULL_FACE);
            faceProg_->bind();
            if (transparent) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
            continue;
        }
        faceProg_->setUniformValue("uAlpha", alpha);
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
    if (cull) glDisable(GL_CULL_FACE);
    if (mirrored) glFrontFace(GL_CCW);
}

void Renderer::drawSurfaceSide(const Batch& b, const Material& mat, Id matId, const Camera& cam, const RenderStyle& st,
                               const Mat4& model, float fade, float alpha) {
    QOpenGLShaderProgram* p = surfaceProgram(mat);
    p->bind();
    setMat(p, "uMVP", cam.viewProjection() * model);
    setMat(p, "uModel", model);
    {
        Mat4 inv = model.inverted();
        float n[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) n[r * 3 + c] = (float)inv.at(c, r); // row-major
        p->setUniformValue("uNrm", QMatrix3x3(n));
    }
    setShadowUniforms(p);
    setLightUniforms(p);
    auto vec3 = [](const Vec3& v) { return QVector3D((float)v.x, (float)v.y, (float)v.z); };
    auto rgb = [](const Color& c, float k = 1.0f) { return QVector3D(c.r * k, c.g * k, c.b * k); };
    p->setUniformValue("uFade", fade > 0 ? 0.55f : 0.0f);
    p->setUniformValue("uAlpha", alpha);
    if (const int timeLoc = p->uniformLocation("_Time"); timeLoc >= 0) { // only when the shader really reads it
        p->setUniformValue(timeLoc, (float)(clock_.elapsed() * 0.001));
        animated_ = true;
    }
    p->setUniformValue("uEye", vec3(cam.eye));
    p->setUniformValue("uForward", vec3(cam.forward()));
    p->setUniformValue("uPerspective", cam.perspective);
    p->setUniformValue("uLight", vec3(lightDir_));
    p->setUniformValue("uSunOn", sunOn_);
    p->setUniformValue("uSunColor", vec3(sunColor_));
    p->setUniformValue("uSkyAmbient", vec3(skyAmbient_));
    p->setUniformValue("uGroundAmbient", vec3(groundAmbient_));
    const float keep = sunOn_ ? (float)(0.14 + 0.86 * daylight_) : 1.0f;
    p->setUniformValue("uEnvSky", rgb(st.skyColor, keep));
    p->setUniformValue("uEnvHorizon", rgb(st.horizonColor, keep));
    p->setUniformValue("uEnvGround", rgb(st.groundColor, keep));
    p->setUniformValue("uHasBase", mat.hasTexture());
    p->setUniformValue("uBaseStrength", mat.baseStrength);
    p->setUniformValue("uFlipY", mat.flipNormalY);
    p->setUniformValue("uEmission", rgb(mat.emission));
    p->setUniformValue("uBlend", mat.blend);
    GLint has[MapSlotCount];
    GLfloat strength[MapSlotCount];
    for (int i = 0; i < MapSlotCount; ++i) {
        has[i] = mat.maps[i].tex.valid() ? 1 : 0;
        strength[i] = mat.maps[i].strength;
    }
    p->setUniformValueArray("uHas", has, MapSlotCount);
    p->setUniformValueArray("uStrength", strength, MapSlotCount, 1);
    // Texture units: 2 is the shadow map, the maps follow it.
    static const char* const kSamplers[MapSlotCount] = {"_NormalMap", "_MetallicMap", "_RoughnessMap", "_OcclusionMap",
                                                        "_HeightMap", "_EmissionMap", "_OpacityMap"};
    p->setUniformValue("_BaseMap", 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, textureFor(matId));
    for (int i = 0; i < MapSlotCount; ++i) {
        p->setUniformValue(kSamplers[i], 4 + i);
        glActiveTexture(GL_TEXTURE4 + i);
        glBindTexture(GL_TEXTURE_2D, textureFor(matId, i));
    }
    glActiveTexture(GL_TEXTURE0);
    if (mat.customShader()) {
        auto cached = surfaceProps_.find(mat.shader);
        if (cached == surfaceProps_.end()) cached = surfaceProps_.emplace(mat.shader, parseShaderProps(mat.shader)).first;
        for (const ShaderProp& sp : cached->second) {
            auto it = mat.shaderProps.find(sp.name);
            const std::array<float, 4> v = it != mat.shaderProps.end() ? it->second : sp.def;
            const QByteArray name = QByteArray::fromStdString(sp.name);
            switch (sp.components) {
            case 1: p->setUniformValue(name.constData(), v[0]); break;
            case 2: p->setUniformValue(name.constData(), QVector2D(v[0], v[1])); break;
            case 3: p->setUniformValue(name.constData(), QVector3D(v[0], v[1], v[2])); break;
            default: p->setUniformValue(name.constData(), QVector4D(v[0], v[1], v[2], v[3])); break;
            }
        }
    }
    glDrawArrays(GL_TRIANGLES, b.first, b.count);
    for (int i = 0; i <= MapSlotCount; ++i) {
        glActiveTexture(GL_TEXTURE3 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::updateLights(const ModelState& ms) {
    // Directional lights reach everywhere; of the others the strongest count.
    std::vector<const SceneObject*> lights;
    for (const SceneObject& o : ms.objects)
        if (o.alive && o.enabled && o.isLight() && o.intensity > 0) lights.push_back(&o);
    std::stable_sort(lights.begin(), lights.end(), [](const SceneObject* a, const SceneObject* b) {
        const bool da = a->kind == SceneObject::DirectionalLight, db = b->kind == SceneObject::DirectionalLight;
        return da != db ? da : a->intensity > b->intensity;
    });
    lightCount_ = std::min<int>((int)lights.size(), kMaxLights);
    for (int i = 0; i < lightCount_; ++i) {
        const SceneObject& o = *lights[i];
        const Vec3 d = o.aims() ? (o.pos - o.target).normalized() : Vec3(0, 0, 1);
        const double half = std::clamp(o.coneAngle, 1.0, 179.0) * 0.5 * kPi / 180;
        const double inner = half * (1 - std::clamp(o.softness, 0.0, 1.0));
        const float type = o.kind == SceneObject::SpotLight ? 1 : o.kind == SceneObject::DirectionalLight ? 2 : 0;
        const float k = (float)o.intensity;
        const float vals[] = {(float)o.pos.x, (float)o.pos.y, (float)o.pos.z};
        const float dirs[] = {(float)d.x, (float)d.y, (float)d.z};
        const float cols[] = {o.color.r * k, o.color.g * k, o.color.b * k};
        const float par[] = {type, (float)std::max(o.range, 1e-3), (float)std::cos(half), (float)std::cos(inner)};
        std::copy(vals, vals + 3, lightPos_ + i * 3);
        std::copy(dirs, dirs + 3, lightAim_ + i * 3);
        std::copy(cols, cols + 3, lightColor_ + i * 3);
        std::copy(par, par + 4, lightParam_ + i * 4);
    }
}

void Renderer::setLightUniforms(QOpenGLShaderProgram* p) {
    p->setUniformValue("uLightCount", lightCount_);
    if (lightCount_ == 0) return;
    p->setUniformValueArray("uLightPos", lightPos_, lightCount_, 3);
    p->setUniformValueArray("uLightDir", lightAim_, lightCount_, 3);
    p->setUniformValueArray("uLightColor", lightColor_, lightCount_, 3);
    p->setUniformValueArray("uLightParam", lightParam_, lightCount_, 4);
}

void Renderer::drawObjects(const Document& doc, const Camera& cam, double dpr) {
    const auto& st = doc.state;
    std::vector<float> lines, dashed;
    for (Id i = 0; i < (Id)st.objects.size(); ++i) {
        const SceneObject& o = st.objects[i];
        if (!o.alive) continue;
        const bool sel = doc.selection.objects.count(i) > 0;
        Color c = sel ? Color::rgb8(0, 0, 255)
                : !o.isLight() ? Color::rgb8(40, 40, 40)
                : o.enabled    ? Color::rgb8(225, 150, 0)
                               : Color::rgb8(150, 150, 150);
        const double px = std::max(cam.pixelSize(o.pos), 1e-6);
        auto circle = [&](const Vec3& center, const Vec3& a, const Vec3& b, double r, int n = 20) {
            for (int k = 0; k < n; ++k) {
                const double t0 = 2 * kPi * k / n, t1 = 2 * kPi * (k + 1) / n;
                pushLine(lines, center + (a * std::cos(t0) + b * std::sin(t0)) * r, center + (a * std::cos(t1) + b * std::sin(t1)) * r, c);
            }
        };
        const Vec3 aim = o.aims() ? (o.target - o.pos).normalized() : Vec3(0, 0, -1);
        Vec3 u, v;
        planeBasis(aim, u, v);
        if (o.kind == SceneObject::ViewCamera) {
            // A small frustum in the camera's own axes.
            Vec3 right = aim.cross(Vec3(0, 0, 1));
            right = right.length2() < 1e-9 ? u : right.normalized();
            const Vec3 up = right.cross(aim);
            const double depth = px * 34;
            const double h = std::tan(o.fov * 0.5 * kPi / 180) * depth, w = h * 16.0 / 9.0;
            const Vec3 e = o.pos + aim * depth;
            const Vec3 k[4] = {e - right * w - up * h, e + right * w - up * h, e + right * w + up * h, e - right * w + up * h};
            for (int j = 0; j < 4; ++j) {
                pushLine(lines, o.pos, k[j], c);
                pushLine(lines, k[j], k[(j + 1) % 4], c);
            }
            pushLine(lines, e + up * (h * 1.1) - right * (w * 0.3), e + up * (h * 1.5), c);
            pushLine(lines, e + up * (h * 1.5), e + up * (h * 1.1) + right * (w * 0.3), c);
        } else {
            const double r = px * 9;
            circle(o.pos, Vec3(1, 0, 0), Vec3(0, 1, 0), r);
            circle(o.pos, Vec3(1, 0, 0), Vec3(0, 0, 1), r);
            circle(o.pos, Vec3(0, 1, 0), Vec3(0, 0, 1), r);
            if (o.kind == SceneObject::PointLight) {
                for (int s = 0; s < 3; ++s) {
                    Vec3 d;
                    d[s] = 1;
                    pushLine(lines, o.pos + d * (r * 1.3), o.pos + d * (r * 2), c);
                    pushLine(lines, o.pos - d * (r * 1.3), o.pos - d * (r * 2), c);
                }
            } else if (o.kind == SceneObject::SpotLight) {
                const double dist = (o.target - o.pos).length();
                const double cr = std::tan(std::clamp(o.coneAngle, 1.0, 170.0) * 0.5 * kPi / 180) * dist;
                circle(o.target, u, v, cr, 32);
                for (int s = 0; s < 4; ++s) {
                    const double t = s * kPi * 0.5;
                    pushLine(lines, o.pos, o.target + (u * std::cos(t) + v * std::sin(t)) * cr, c);
                }
            } else {
                // Parallel rays.
                for (int s = 0; s < 4; ++s) {
                    const double t = s * kPi * 0.5;
                    const Vec3 start = o.pos + (u * std::cos(t) + v * std::sin(t)) * r;
                    pushLine(lines, start, start + aim * (px * 40), c);
                }
            }
        }
        if (sel && o.aims()) pushLine(dashed, o.pos, o.target, c);
    }
    glDisable(GL_DEPTH_TEST);
    drawLines(lines, cam, 1.3f, Dash::None, dpr);
    drawLines(dashed, cam, 1.0f, Dash::Dashed, dpr);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::updateSun(const RenderStyle& st) {
    sunOn_ = st.sun;
    lightDir_ = st.sun ? st.sunDirection() : kLightDir;
    if (!st.sun) return;
    // Warm and dim near the horizon, gone below it; the sky fades a little later.
    const double h = lightDir_.z;
    const double above = std::clamp((h + 0.02) / 0.12, 0.0, 1.0);
    const double high = std::clamp(h / 0.35, 0.0, 1.0);
    const double I = std::max(st.sunIntensity, 0.0);
    const Vec3 warm(1.0, 0.55, 0.28), white(1.0, 0.96, 0.86);
    sunColor_ = lerp(warm, white, high) * (0.62 * I * above * above * (3 - 2 * above));
    daylight_ = std::clamp((h + 0.12) / 0.45, 0.0, 1.0);
    const double amb = 0.22 + 0.78 * daylight_;
    skyAmbient_ = Vec3(0.46, 0.50, 0.56) * amb;
    groundAmbient_ = Vec3(0.36, 0.34, 0.30) * amb;
}

void Renderer::setShadowUniforms(QOpenGLShaderProgram* p) {
    p->setUniformValue("uShadows", shadowsOn_);
    setMat(p, "uLightVP", lightVP_);
    p->setUniformValue("uShadowBias", (float)shadowBias_);
    p->setUniformValue("uShadowMap", 2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::freeAoTargets() {
    if (aoDepthFbo_) glDeleteFramebuffers(1, &aoDepthFbo_);
    if (aoFbo_) glDeleteFramebuffers(1, &aoFbo_);
    if (aoDepthTex_) glDeleteTextures(1, &aoDepthTex_);
    if (aoTex_) glDeleteTextures(1, &aoTex_);
    aoDepthFbo_ = aoFbo_ = aoDepthTex_ = aoTex_ = 0;
    aoW_ = aoH_ = 0;
}

bool Renderer::ensureAoTargets(int w, int h) {
    if (aoDepthFbo_ && aoW_ == w && aoH_ == h) return true;
    freeAoTargets();
    auto tex = [&](unsigned& t, GLint internal, GLenum format, GLenum type) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    tex(aoDepthTex_, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT);
    tex(aoTex_, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &aoDepthFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, aoDepthFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, aoDepthTex_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &aoFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, aoFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aoTex_, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) freeAoTargets();
    aoW_ = w;
    aoH_ = h;
    return ok;
}

void Renderer::drawAmbientOcclusion(const std::vector<std::pair<const MeshGpu*, Mat4>>& meshes, const Camera& cam) {
    // The widget renders into its own framebuffer: remember it before
    // (re)creating the AO targets binds theirs.
    GLint prevFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    const bool ready = ensureAoTargets(vpW_, vpH_);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    if (!ready) return;

    // 1. Depth of the opaque faces.
    glBindFramebuffer(GL_FRAMEBUFFER, aoDepthFbo_);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    depthProg_->bind();
    if (culling_) glEnable(GL_CULL_FACE);
    for (const auto& [g, xf] : meshes) {
        setMat(depthProg_.get(), "uMVP", cam.viewProjection() * xf);
        if (xf.det3() < 0) glFrontFace(GL_CW);
        glBindVertexArray(g->vao);
        for (const Batch& b : g->batches)
            if (!b.transparent && !b.paint) glDrawArrays(GL_TRIANGLES, b.first, b.count);
        glFrontFace(GL_CCW);
    }
    glDisable(GL_CULL_FACE);

    // 2. Occlusion from that depth.
    const Mat4 proj = cam.projection();
    const double radius = std::clamp(cam.distance() * 0.04, 0.01, 5.0);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, aoFbo_);
    aoProg_->bind();
    setMat(aoProg_.get(), "uProj", proj);
    setMat(aoProg_.get(), "uInvProj", proj.inverted());
    aoProg_->setUniformValue("uRadius", (float)radius);
    aoProg_->setUniformValue("uTexel", QVector2D(1.0f / vpW_, 1.0f / vpH_));
    aoProg_->setUniformValue("uDepth", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, aoDepthTex_);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // 3. Blurred and multiplied over the shaded faces.
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    aoBlendProg_->bind();
    setMat(aoBlendProg_.get(), "uInvProj", proj.inverted());
    aoBlendProg_->setUniformValue("uTexel", QVector2D(1.0f / vpW_, 1.0f / vpH_));
    aoBlendProg_->setUniformValue("uStrength", 0.85f);
    aoBlendProg_->setUniformValue("uDepth", 0);
    aoBlendProg_->setUniformValue("uAo", 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, aoTex_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::renderShadowMap(const std::vector<std::pair<const MeshGpu*, Mat4>>& casters, const Vec3& mn, const Vec3& mx) {
    // Orthographic sun view fitted around the model's bounding sphere; the depth
    // range reaches down to the ground plane so ground shadows are covered.
    Vec3 c = (mn + mx) * 0.5;
    double r = std::max((mx - mn).length() * 0.5, 0.5);
    double below = std::max(0.0, c.z + r) / std::max(lightDir_.z, 0.1); // centre to ground along the light
    Mat4 view = Mat4::lookAt(c + lightDir_ * (r * 2), c, Vec3(0, 0, 1));
    lightVP_ = Mat4::ortho(-r, r, -r, r, r * 0.5, r * 3 + below) * view;
    shadowBias_ = 2 * r / kShadowSize * 1.5;

    GLint prevFbo = 0, prevVp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, kShadowSize, kShadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    depthProg_->bind();
    for (const auto& [g, xf] : casters) {
        setMat(depthProg_.get(), "uMVP", lightVP_ * xf);
        glBindVertexArray(g->vao);
        for (const Batch& b : g->batches)
            if (!b.transparent && !b.paint) glDrawArrays(GL_TRIANGLES, b.first, b.count);
    }
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
}

void Renderer::collectEdges(const Mesh& m, const Mat4& model, const Document& doc, const Camera& cam,
                            const Preview& preview, bool active, bool faded, EdgeLists& out) {
    const RenderStyle& st = doc.style;
    const bool identity = model.isIdentity();
    const bool persp = cam.perspective;
    const Vec3 eye = cam.eye, fwd = cam.forward();
    auto facing = [&](Id f, const Vec3& p) {
        Vec3 viewDir = persp ? (eye - p) : -fwd;
        Vec3 n = identity ? m.faces[f].normal : transformCovector(model, m.faces[f].normal);
        return n.dot(viewDir) > 0;
    };
    const bool drawEdges = st.edges || st.faceStyle == RenderStyle::Wireframe;
    Color edgeColor = st.edgeColor;
    // Faded geometry keeps its edges, just dimmer than the open component's.
    if (faded) edgeColor = Color(edgeColor.r * 0.5f + 0.42f, edgeColor.g * 0.5f + 0.42f, edgeColor.b * 0.5f + 0.42f, 0.9f);
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        const Edge& E = m.edges[e];
        if (!E.alive) continue;
        const Vec3 a = identity ? m.pos(E.v[0]) : model.transformPoint(m.pos(E.v[0]));
        const Vec3 b = identity ? m.pos(E.v[1]) : model.transformPoint(m.pos(E.v[1]));
        bool isSel = active && (doc.selection.edges.count(e) || preview.highlightEdges.count(e));
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
        if (culling_ && nf > 0 && !isSel) {
            // Every face of this edge is culled: it would hang in empty space.
            bool anyFront = false;
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive && !m.faces[f].hidden && facing(f, mid)) anyFront = true;
            if (!anyFront) continue;
        }
        if (nf == 1) isProfile = true;
        else if (nf == 2) isProfile = facing(f0, mid) != facing(f1, mid);
        if (st.faceStyle == RenderStyle::Wireframe) isProfile = false;

        std::vector<float>& normal = faded ? out.faded : out.normal;
        std::vector<float>& profile = faded ? out.fadedProfile : out.profile;
        if (E.hidden || E.soft) {
            if (isSel) pushLine(out.selected, a, b, Color::rgb8(0, 0, 255));
            else if (st.profiles && isProfile && E.soft && !E.hidden && drawEdges) pushLine(profile, a, b, edgeColor);
            else if (st.showHidden && !faded) pushLine(out.hidden, a, b, Color::rgb8(110, 110, 110));
            continue;
        }
        if (isSel) {
            pushLine(out.selected, a, b, Color::rgb8(0, 0, 255));
            continue;
        }
        if (!drawEdges) continue;
        if (st.profiles && isProfile) pushLine(profile, a, b, edgeColor);
        else pushLine(normal, a, b, edgeColor);
    }
}

void Renderer::render(const Document& doc, const Camera& cam, const Preview& preview, double dpr) {
    if (!initialized_) return;
    animated_ = false;
    const ModelState& ms = doc.state;
    const RenderStyle& st = doc.style;
    vpW_ = (int)std::lround(cam.width * dpr);
    vpH_ = (int)std::lround(cam.height * dpr);
    glViewport(0, 0, vpW_, vpH_);
    // QPainter on the previous frame leaves its state behind: drawing text, for
    // one, keeps a blend mode on that would wash this whole frame out to white.
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    glClearColor(st.background.r, st.background.g, st.background.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    if (st.antialiasing) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);

    if (builtMatRevision_ != doc.materialsRevision) {
        syncTextures(doc);
        builtMatRevision_ = doc.materialsRevision;
    }
    // Forget definitions that no longer exist.
    for (auto it = defs_.begin(); it != defs_.end();) {
        if (it->first >= (Id)ms.defs.size()) {
            freeMesh(it->second);
            it = defs_.erase(it);
        } else {
            ++it;
        }
    }

    // While a component is open, it is drawn normally and everything else
    // (loose geometry and other instances) faded.
    const bool editing = ms.editing != kNone;
    const Id editDef = editing ? ms.instances[ms.editing].def : kNone;
    struct Placed {
        Id inst, def;
        Mat4 xf;
        bool active;
    };
    std::vector<Placed> placed;
    for (const PlacedInstance& p : placedInstances(ms))
        if (p.inst != ms.editing) placed.push_back({p.inst, p.def, p.xf, p.active});
    const MeshGpu& activeGpu = meshGpu(doc, editDef);
    const Mesh& active = ms.activeMesh();

    updateSun(st);
    updateLights(ms);
    culling_ = st.backfaceCulling && !st.xray && st.faceStyle != RenderStyle::Wireframe;
    shadowsOn_ = false;
    if (st.shadows && lightDir_.z > 0.02 && st.faceStyle != RenderStyle::Wireframe && st.faceStyle != RenderStyle::HiddenLine) {
        Vec3 mn, mx, amn, amx;
        bool any = false, anyActive = false;
        modelBounds(ms, mn, mx, any);
        active.bounds(amn, amx, anyActive);
        if (anyActive) {
            if (!any) mn = amn, mx = amx;
            for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], amn[k]), mx[k] = std::max(mx[k], amx[k]);
            any = true;
        }
        if (any) {
            std::vector<std::pair<const MeshGpu*, Mat4>> casters{{&activeGpu, Mat4()}};
            if (editing) casters.push_back({&meshGpu(doc, kNone), Mat4()});
            for (const Placed& p : placed) casters.push_back({&meshGpu(doc, p.def), p.xf});
            renderShadowMap(casters, mn, mx);
            shadowsOn_ = true;
        }
    }

    drawBackground(doc, cam);
    drawMeshFaces(activeGpu, doc, cam, Mat4(), 0.0f);
    for (const Placed& p : placed)
        if (p.active) drawMeshFaces(meshGpu(doc, p.def), doc, cam, p.xf, 0.0f);
    if (st.ambientOcclusion && !st.xray && st.faceStyle != RenderStyle::Wireframe) {
        std::vector<std::pair<const MeshGpu*, Mat4>> meshes{{&activeGpu, Mat4()}};
        for (const Placed& p : placed)
            if (p.active) meshes.push_back({&meshGpu(doc, p.def), p.xf});
        drawAmbientOcclusion(meshes, cam);
    }

    // Selection / highlight stipple over faces.
    {
        std::vector<float> sel;
        auto addFace = [&](Id f) {
            if (f >= 0 && f < (Id)activeGpu.faceTris.size())
                sel.insert(sel.end(), activeGpu.faceTris[f].begin(), activeGpu.faceTris[f].end());
        };
        if (!preview.hideFaceSelection)
            for (Id f : doc.selection.faces) addFace(f);
        for (Id f : preview.highlightFaces)
            if (!doc.selection.faces.count(f)) addFace(f);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        if (culling_) glEnable(GL_CULL_FACE);
        drawOverlayFaces(sel, cam, Color::rgb8(0, 0, 220), true);
        glDisable(GL_CULL_FACE);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // Edges.
    EdgeLists el;
    collectEdges(active, Mat4(), doc, cam, preview, true, false, el);
    if (editing) collectEdges(ms.mesh, Mat4(), doc, cam, preview, false, true, el);
    for (const Placed& p : placed) collectEdges(ms.defs[p.def].mesh, p.xf, doc, cam, preview, false, !p.active, el);

    // Component boxes: selected / hovered instances, and the open one.
    std::vector<float> boxes, openBox;
    auto addBox = [&](Id inst, std::vector<float>& out, const Color& c) {
        Vec3 k[8];
        if (!instanceBoxCorners(ms, inst, k)) return;
        static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& ed : E) pushLine(out, k[ed[0]], k[ed[1]], c);
    };
    if (editing) addBox(ms.editing, openBox, Color::rgb8(90, 90, 90));
    for (Id i : doc.selection.instances) addBox(i, boxes, Color::rgb8(0, 0, 255));
    for (Id i : preview.highlightInstances)
        if (!doc.selection.instances.count(i)) addBox(i, boxes, Color::rgb8(80, 80, 255));

    if (st.xray) glDisable(GL_DEPTH_TEST);
    drawLines(el.normal, cam, 1.0f, Dash::None, dpr);
    drawLines(el.profile, cam, st.profileWidth, Dash::None, dpr);
    drawLines(el.hidden, cam, 1.0f, Dash::Dashed, dpr);
    if (st.backEdges && !st.xray && st.faceStyle != RenderStyle::Wireframe) {
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        std::vector<float> back = el.normal;
        back.insert(back.end(), el.profile.begin(), el.profile.end());
        drawLines(back, cam, 1.0f, Dash::Dashed, dpr);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
    }
    glEnable(GL_DEPTH_TEST);

    if (editing) {
        // Faded rest of the model, translucent over the open component.
        drawMeshFaces(meshGpu(doc, kNone), doc, cam, Mat4(), 0.35f);
        for (const Placed& p : placed)
            if (!p.active) drawMeshFaces(meshGpu(doc, p.def), doc, cam, p.xf, 0.35f);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the framebuffer itself stays opaque
        glDepthMask(GL_FALSE);
        drawLines(el.faded, cam, 1.0f, Dash::None, dpr);
        drawLines(el.fadedProfile, cam, std::max(1.0f, st.profileWidth * 0.7f), Dash::None, dpr);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        drawLines(openBox, cam, 1.0f, Dash::Dashed, dpr);
        glEnable(GL_DEPTH_TEST);
    }
    drawLines(el.selected, cam, 1.8f, Dash::None, dpr);
    drawLines(boxes, cam, 1.5f, Dash::None, dpr);

    // Guides.
    if (st.showGuides && !doc.state.guides.empty()) {
        std::vector<float> g;
        for (const Guide& gd : doc.state.guides) {
            if (gd.kind != Guide::Line) continue;
            pushLine(g, gd.p - gd.dir * 2e4, gd.p + gd.dir * 2e4, Color::rgb8(60, 60, 60, 220));
        }
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the framebuffer itself stays opaque
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

    if (st.showObjects && !ms.objects.empty()) drawObjects(doc, cam, dpr);

    // Tool preview.
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the framebuffer itself stays opaque
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
