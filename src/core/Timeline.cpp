// SPDX-License-Identifier: GPL-3.0-or-later
#include "Timeline.h"

#include "Document.h"

#include <algorithm>
#include <cmath>

namespace gs {

int TimelineLayer::keyAt(int frame) const {
    for (int i = 0; i < (int)keys.size(); ++i)
        if (keys[i].frame == frame) return i;
    return -1;
}

int TimelineLayer::keyBefore(int frame) const {
    int r = -1;
    for (int i = 0; i < (int)keys.size() && keys[i].frame <= frame; ++i) r = i;
    return r;
}

int Timeline::lastKeyFrame() const {
    int f = 0;
    for (const TimelineLayer& l : layers)
        if (!l.keys.empty()) f = std::max(f, l.keys.back().frame);
    return f;
}

bool layerTargetAlive(const ModelState& st, const TimelineLayer& l) {
    if (l.kind == TimelineLayer::Instance) return l.target >= 0 && l.target < (Id)st.instances.size() && st.instances[l.target].alive;
    return l.target >= 0 && l.target < (Id)st.objects.size() && st.objects[l.target].alive;
}

TimelineKey captureKey(const ModelState& st, const TimelineLayer& l, int frame, bool tween) {
    TimelineKey k;
    k.frame = frame;
    k.tween = tween;
    if (!layerTargetAlive(st, l)) return k;
    if (l.kind == TimelineLayer::Instance) {
        k.xf = st.instances[l.target].xf;
    } else {
        const SceneObject& o = st.objects[l.target];
        k.pos = o.pos;
        k.target = o.target;
        k.color = o.color;
        k.intensity = o.intensity;
        k.range = o.range;
        k.cone = o.coneAngle;
        k.fov = o.fov;
        k.enabled = o.enabled;
    }
    return k;
}

namespace {

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;
};

Quat quatFromRotation(const double r[3][3]) {
    Quat q;
    const double tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q.w = 0.25 * s;
        q.x = (r[2][1] - r[1][2]) / s;
        q.y = (r[0][2] - r[2][0]) / s;
        q.z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2;
        q.w = (r[2][1] - r[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (r[0][1] + r[1][0]) / s;
        q.z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2;
        q.w = (r[0][2] - r[2][0]) / s;
        q.x = (r[0][1] + r[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (r[1][2] + r[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2;
        q.w = (r[1][0] - r[0][1]) / s;
        q.x = (r[0][2] + r[2][0]) / s;
        q.y = (r[1][2] + r[2][1]) / s;
        q.z = 0.25 * s;
    }
    return q;
}

Quat slerp(Quat a, Quat b, double t) {
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    double ka, kb;
    if (d > 0.9995) {
        ka = 1 - t;
        kb = t;
    } else {
        const double th = std::acos(std::clamp(d, -1.0, 1.0));
        ka = std::sin((1 - t) * th) / std::sin(th);
        kb = std::sin(t * th) / std::sin(th);
    }
    Quat q{ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z};
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n > 0) q = {q.w / n, q.x / n, q.y / n, q.z / n};
    return q;
}

// Splits m into position, rotation and (signed) scale along its own axes.
void decompose(const Mat4& m, Vec3& pos, Quat& q, Vec3& scale) {
    pos = Vec3(m.at(0, 3), m.at(1, 3), m.at(2, 3));
    Vec3 c[3];
    for (int j = 0; j < 3; ++j) c[j] = Vec3(m.at(0, j), m.at(1, j), m.at(2, j));
    for (int j = 0; j < 3; ++j) scale[j] = c[j].length();
    if (m.det3() < 0) scale[0] = -scale[0]; // a mirror
    double r[3][3];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) r[i][j] = std::fabs(scale[j]) > 1e-12 ? c[j][i] / scale[j] : (i == j ? 1 : 0);
    q = quatFromRotation(r);
}

Mat4 compose(const Vec3& pos, const Quat& q, const Vec3& s) {
    const double w = q.w, x = q.x, y = q.y, z = q.z;
    const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    Mat4 m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[j * 4 + i] = r[i][j] * s[j];
    m.m[12] = pos.x;
    m.m[13] = pos.y;
    m.m[14] = pos.z;
    return m;
}

} // namespace

Mat4 interpolateTransform(const Mat4& a, const Mat4& b, double t) {
    Vec3 pa, pb, sa, sb;
    Quat qa, qb;
    decompose(a, pa, qa, sa);
    decompose(b, pb, qb, sb);
    return compose(pa + (pb - pa) * t, slerp(qa, qb, t), sa + (sb - sa) * t);
}

bool evaluateLayer(const TimelineLayer& l, double frame, TimelineKey& out) {
    if (l.keys.empty()) return false;
    if (frame <= l.keys.front().frame) {
        out = l.keys.front();
        return true;
    }
    int i = l.keyBefore((int)std::floor(frame));
    if (i < 0) i = 0;
    const TimelineKey& a = l.keys[i];
    if (i + 1 >= (int)l.keys.size() || !a.tween) {
        out = a;
        return true;
    }
    const TimelineKey& b = l.keys[i + 1];
    const double t = std::clamp((frame - a.frame) / double(std::max(1, b.frame - a.frame)), 0.0, 1.0);
    out = a;
    out.xf = interpolateTransform(a.xf, b.xf, t);
    out.pos = a.pos + (b.pos - a.pos) * t;
    out.target = a.target + (b.target - a.target) * t;
    auto mix = [t](float x, float y) { return x + (y - x) * (float)t; };
    out.color = Color(mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b), mix(a.color.a, b.color.a));
    out.intensity = a.intensity + (b.intensity - a.intensity) * t;
    out.range = a.range + (b.range - a.range) * t;
    out.cone = a.cone + (b.cone - a.cone) * t;
    out.fov = a.fov + (b.fov - a.fov) * t;
    return true;
}

bool keyDiffers(const ModelState& st, const TimelineLayer& l, const TimelineKey& k) {
    if (!layerTargetAlive(st, l)) return false;
    const TimelineKey now = captureKey(st, l, k.frame);
    if (l.kind == TimelineLayer::Instance) {
        for (int i = 0; i < 16; ++i)
            if (std::fabs(now.xf.m[i] - k.xf.m[i]) > 1e-9) return true;
        return false;
    }
    return !now.pos.nearlyEquals(k.pos, 1e-9) || !now.target.nearlyEquals(k.target, 1e-9) || !(now.color == k.color) ||
           std::fabs(now.intensity - k.intensity) > 1e-9 || std::fabs(now.range - k.range) > 1e-9 || std::fabs(now.cone - k.cone) > 1e-9 ||
           std::fabs(now.fov - k.fov) > 1e-9 || now.enabled != k.enabled;
}

bool applyKey(ModelState& st, const TimelineLayer& l, const TimelineKey& k) {
    if (!layerTargetAlive(st, l) || !keyDiffers(st, l, k)) return false;
    if (l.kind == TimelineLayer::Instance) {
        st.instances[l.target].xf = k.xf;
    } else {
        SceneObject& o = st.objects[l.target];
        o.pos = k.pos;
        o.target = k.target;
        o.color = k.color;
        o.intensity = k.intensity;
        o.range = k.range;
        o.coneAngle = k.cone;
        o.fov = k.fov;
        o.enabled = k.enabled;
    }
    return true;
}

bool applyTimeline(ModelState& st, double frame) {
    if (st.editing != kNone) return false;
    bool any = false;
    for (const TimelineLayer& l : st.timeline.layers) {
        TimelineKey k;
        if (evaluateLayer(l, frame, k)) any = applyKey(st, l, k) || any;
    }
    return any;
}

int setKey(TimelineLayer& l, const TimelineKey& k) {
    const int at = l.keyAt(k.frame);
    if (at >= 0) {
        l.keys[at] = k;
        return at;
    }
    auto it = std::lower_bound(l.keys.begin(), l.keys.end(), k.frame, [](const TimelineKey& a, int f) { return a.frame < f; });
    return (int)(l.keys.insert(it, k) - l.keys.begin());
}

} // namespace gs
