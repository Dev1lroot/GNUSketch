// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextureMapping.h"

namespace gs {

UvAffine UvAffine::inverse() const {
    const double D = det();
    if (std::fabs(D) < 1e-300) return {};
    UvAffine r;
    r.a = d / D;
    r.b = -b / D;
    r.c = -c / D;
    r.d = a / D;
    r.e = -(r.a * e + r.b * f);
    r.f = -(r.c * e + r.d * f);
    return r;
}

UvAffine UvAffine::translate(double dx, double dy) {
    UvAffine r;
    r.e = dx;
    r.f = dy;
    return r;
}

UvAffine UvAffine::scaleAbout(const Vec2& c, double sx, double sy) {
    UvAffine r;
    r.a = sx;
    r.d = sy;
    r.e = c.x * (1 - sx);
    r.f = c.y * (1 - sy);
    return r;
}

UvAffine UvAffine::shearAbout(const Vec2& anchor, double k, bool alongU) {
    UvAffine r;
    if (alongU) {
        r.b = k;
        r.e = -k * anchor.y;
    } else {
        r.c = k;
        r.f = -k * anchor.x;
    }
    return r;
}

TexMap explicitTexMap(const Face& f, bool frontSide, const Material* m) {
    const FaceSide& s = frontSide ? f.front : f.back;
    if (s.tex.valid) return s.tex;
    // Mirrors projectedTexCoord: uv = (p . u / w, p . v / h).
    const Vec3 n = s.axis.length2() > 0.5 ? s.axis : (frontSide ? f.normal : -f.normal);
    Vec3 u, v;
    planeBasis(n, u, v);
    double w = 1, h = 1;
    if (m) m->tileSize(w, h);
    TexMap t;
    t.valid = true;
    t.origin = Vec3();
    t.u = u / w;
    t.v = v / h;
    return t;
}

bool moveTexMap(TexMap& map, const UvAffine& moved) {
    if (std::fabs(moved.det()) < 1e-12) return false;
    const UvAffine inv = moved.inverse();
    // uv' = inv(uv) gives new axes and an offset carried by the origin.
    const Vec3 u2 = map.u * inv.a + map.v * inv.b;
    const Vec3 v2 = map.u * inv.c + map.v * inv.d;
    const double uu = u2.dot(u2), uv = u2.dot(v2), vv = v2.dot(v2);
    const double G = uu * vv - uv * uv;
    if (G < 1e-24 * uu * vv || uu <= 0 || vv <= 0) return false;
    // origin - origin' = s, in the span of the axes, with s . u2 = e and s . v2 = f.
    const double alpha = (inv.e * vv - inv.f * uv) / G, beta = (inv.f * uu - inv.e * uv) / G;
    map.origin -= u2 * alpha + v2 * beta;
    map.u = u2;
    map.v = v2;
    map.valid = true;
    return true;
}

bool PlaneTexMap::set(const TexMap& map, const Vec3& normal, const Vec3& pointOnPlane) {
    map_ = map;
    on_ = pointOnPlane;
    planeBasis(normal, e1_, e2_);
    m_[0] = e1_.dot(map.u);
    m_[1] = e2_.dot(map.u);
    m_[2] = e1_.dot(map.v);
    m_[3] = e2_.dot(map.v);
    const Vec3 d = on_ - map.origin;
    t_[0] = d.dot(map.u);
    t_[1] = d.dot(map.v);
    return std::fabs(m_[0] * m_[3] - m_[1] * m_[2]) > 1e-12;
}

Vec2 PlaneTexMap::uvAt(const Vec3& p) const {
    const Vec3 d = p - map_.origin;
    return {d.dot(map_.u), d.dot(map_.v)};
}

Vec3 PlaneTexMap::pointAt(const Vec2& uv) const {
    const double D = m_[0] * m_[3] - m_[1] * m_[2];
    const double a = uv.x - t_[0], b = uv.y - t_[1];
    const double x = (m_[3] * a - m_[1] * b) / D, y = (m_[0] * b - m_[2] * a) / D;
    return on_ + e1_ * x + e2_ * y;
}

UvAffine PlaneTexMap::rotationAbout(const Vec2& c, double angle) const {
    // M R M^-1, with M the plane -> uv part: a turn in the world, whatever the
    // texture's proportions, stretch or skew.
    const double D = m_[0] * m_[3] - m_[1] * m_[2];
    const double co = std::cos(angle), si = std::sin(angle);
    const double i0 = m_[3] / D, i1 = -m_[1] / D, i2 = -m_[2] / D, i3 = m_[0] / D;
    const double r0 = co * i0 - si * i2, r1 = co * i1 - si * i3, r2 = si * i0 + co * i2, r3 = si * i1 + co * i3;
    UvAffine t;
    t.a = m_[0] * r0 + m_[1] * r2;
    t.b = m_[0] * r1 + m_[1] * r3;
    t.c = m_[2] * r0 + m_[3] * r2;
    t.d = m_[2] * r1 + m_[3] * r3;
    t.e = c.x - (t.a * c.x + t.b * c.y);
    t.f = c.y - (t.c * c.x + t.d * c.y);
    return t;
}

double PlaneTexMap::angleOf(const Vec2& c, const Vec3& p) const {
    const Vec3 d = p - pointAt(c);
    return std::atan2(d.dot(e2_), d.dot(e1_));
}

double PlaneTexMap::lengthU() const { return (pointAt(Vec2(1, 0)) - pointAt(Vec2(0, 0))).length(); }
double PlaneTexMap::lengthV() const { return (pointAt(Vec2(0, 1)) - pointAt(Vec2(0, 0))).length(); }

} // namespace gs
