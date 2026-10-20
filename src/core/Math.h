// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - small linear algebra helpers (double precision, Z-up world).
#pragma once

#include <algorithm>
#include <cmath>

namespace gs {

// Global geometric tolerance in model units (meters). Two points closer than
// this are considered coincident; points closer than this to a plane lie on it.
constexpr double kTol = 1e-5;
constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0, y = 0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double cross(const Vec2& o) const { return x * o.y - y * o.x; }
    double length() const { return std::sqrt(x * x + y * y); }
};

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    double length2() const { return x * x + y * y + z * z; }
    Vec3 normalized() const {
        double l = length();
        return l > 1e-300 ? (*this) / l : Vec3(0, 0, 0);
    }
    bool nearlyEquals(const Vec3& o, double tol = kTol) const { return (*this - o).length2() <= tol * tol; }
};

inline Vec3 operator*(double s, const Vec3& v) { return v * s; }

inline Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

// Any unit vector perpendicular to n.
inline Vec3 anyPerpendicular(const Vec3& n) {
    Vec3 a = std::fabs(n.z) < 0.9 ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
    return n.cross(a).normalized();
}

// In-plane basis (u, v) for a plane with normal n such that u x v == n.
// Chooses u along world X (or a horizontal direction for vertical planes),
// which gives SketchUp-like texture/rectangle alignment.
inline void planeBasis(const Vec3& nIn, Vec3& u, Vec3& v) {
    Vec3 n = nIn.normalized();
    if (std::fabs(n.z) > 0.999) {
        u = Vec3(1, 0, 0);
        u = (u - n * u.dot(n)).normalized();
    } else {
        // Horizontal direction in the plane; v then tends to point "up".
        u = Vec3(0, 0, 1).cross(n).normalized();
    }
    v = n.cross(u).normalized();
}

struct Plane {
    Vec3 n;      // unit normal
    double d = 0; // n . p = d for points on the plane

    Plane() = default;
    Plane(const Vec3& normal, const Vec3& point) : n(normal.normalized()), d(n.dot(point)) {}
    double distance(const Vec3& p) const { return n.dot(p) - d; }
    Vec3 project(const Vec3& p) const { return p - n * distance(p); }
    bool contains(const Vec3& p, double tol = kTol) const { return std::fabs(distance(p)) <= tol; }
    // Same geometric plane regardless of normal orientation.
    bool coincident(const Plane& o, double tol = kTol) const {
        double c = n.dot(o.n);
        if (c > 1 - 1e-9) return std::fabs(d - o.d) <= tol;
        if (c < -1 + 1e-9) return std::fabs(d + o.d) <= tol;
        return false;
    }
    // Canonical orientation (first significant component positive) for dedup.
    Plane canonical() const {
        Plane p = *this;
        bool flip = false;
        if (std::fabs(n.x) > 1e-9) flip = n.x < 0;
        else if (std::fabs(n.y) > 1e-9) flip = n.y < 0;
        else flip = n.z < 0;
        if (flip) { p.n = -n; p.d = -d; }
        return p;
    }
};

struct Ray {
    Vec3 origin;
    Vec3 dir; // unit
    Vec3 at(double t) const { return origin + dir * t; }
};

// Ray/plane intersection. Returns false if parallel or behind the origin.
inline bool intersectRayPlane(const Ray& r, const Plane& p, double& t) {
    double denom = p.n.dot(r.dir);
    if (std::fabs(denom) < 1e-12) return false;
    t = (p.d - p.n.dot(r.origin)) / denom;
    return t > 0;
}

// Closest point on segment [a,b] to point p; returns parameter in [0,1].
inline double closestParamOnSegment(const Vec3& a, const Vec3& b, const Vec3& p) {
    Vec3 ab = b - a;
    double l2 = ab.length2();
    if (l2 < 1e-300) return 0;
    return std::clamp((p - a).dot(ab) / l2, 0.0, 1.0);
}

inline double distancePointSegment(const Vec3& a, const Vec3& b, const Vec3& p) {
    return (lerp(a, b, closestParamOnSegment(a, b, p)) - p).length();
}

// Closest points between two infinite lines p1 + s*d1 and p2 + t*d2.
// Returns false if lines are parallel.
inline bool closestLineLine(const Vec3& p1, const Vec3& d1, const Vec3& p2, const Vec3& d2,
                            double& s, double& t) {
    Vec3 r = p1 - p2;
    double a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    double c = d1.dot(r), b = d1.dot(d2);
    double denom = a * e - b * b;
    if (std::fabs(denom) < 1e-14 * a * e) return false;
    s = (b * f - c * e) / denom;
    t = (a * f - b * c) / denom;
    return true;
}

// Newell's method: robust normal (non-normalized, length = 2*area) of a 3D polygon.
template <typename GetPt>
inline Vec3 newellNormal(int count, GetPt get) {
    Vec3 n;
    for (int i = 0; i < count; ++i) {
        Vec3 a = get(i), b = get((i + 1) % count);
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

// Signed area of a 2D polygon (CCW positive).
template <typename GetPt>
inline double signedArea2D(int count, GetPt get) {
    double a = 0;
    for (int i = 0; i < count; ++i) {
        Vec2 p = get(i), q = get((i + 1) % count);
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5;
}

// Point-in-polygon (even-odd) in 2D. Points exactly on the boundary are unspecified.
template <typename GetPt>
inline bool pointInPolygon2D(int count, GetPt get, const Vec2& p) {
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        Vec2 a = get(i), b = get(j);
        if ((a.y > p.y) != (b.y > p.y)) {
            double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

// 4x4 column-major matrix (OpenGL convention).
struct Mat4 {
    double m[16];
    Mat4() { setIdentity(); }
    void setIdentity() {
        for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0 : 0.0;
    }
    double& at(int row, int col) { return m[col * 4 + row]; }
    double at(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
                r.at(rr, c) = s;
            }
        return r;
    }
    // Transforms a point with perspective divide.
    Vec3 transformPoint(const Vec3& p, double* wOut = nullptr) const {
        double x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
        double y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
        double z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
        double w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
        if (wOut) *wOut = w;
        if (std::fabs(w) > 1e-300) return {x / w, y / w, z / w};
        return {x, y, z};
    }
    Vec3 transformDir(const Vec3& p) const {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z,
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z,
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z};
    }
    void toFloat(float out[16]) const {
        for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
        return r;
    }
    // Rotation about an axis through the origin (Rodrigues).
    static Mat4 rotation(const Vec3& axisIn, double angle) {
        Vec3 a = axisIn.normalized();
        double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
        Mat4 r;
        r.at(0, 0) = t * a.x * a.x + c;       r.at(0, 1) = t * a.x * a.y - s * a.z; r.at(0, 2) = t * a.x * a.z + s * a.y;
        r.at(1, 0) = t * a.x * a.y + s * a.z; r.at(1, 1) = t * a.y * a.y + c;       r.at(1, 2) = t * a.y * a.z - s * a.x;
        r.at(2, 0) = t * a.x * a.z - s * a.y; r.at(2, 1) = t * a.y * a.z + s * a.x; r.at(2, 2) = t * a.z * a.z + c;
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& upIn) {
        Vec3 f = (target - eye).normalized();
        Vec3 s = f.cross(upIn).normalized();
        if (s.length2() < 1e-20) s = anyPerpendicular(f);
        Vec3 u = s.cross(f);
        Mat4 r;
        r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -s.dot(eye);
        r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -u.dot(eye);
        r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = f.dot(eye);
        return r;
    }
    static Mat4 perspective(double fovYRad, double aspect, double zn, double zf) {
        double f = 1.0 / std::tan(fovYRad / 2);
        Mat4 r;
        for (double& v : r.m) v = 0;
        r.at(0, 0) = f / aspect;
        r.at(1, 1) = f;
        r.at(2, 2) = (zf + zn) / (zn - zf);
        r.at(2, 3) = 2 * zf * zn / (zn - zf);
        r.at(3, 2) = -1;
        return r;
    }
    static Mat4 ortho(double l, double r_, double b, double t, double zn, double zf) {
        Mat4 r;
        r.at(0, 0) = 2 / (r_ - l);
        r.at(1, 1) = 2 / (t - b);
        r.at(2, 2) = -2 / (zf - zn);
        r.at(0, 3) = -(r_ + l) / (r_ - l);
        r.at(1, 3) = -(t + b) / (t - b);
        r.at(2, 3) = -(zf + zn) / (zf - zn);
        return r;
    }
    Mat4 inverted() const;
    // Determinant of the linear 3x3 part (negative for mirroring transforms).
    double det3() const {
        return at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) -
               at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
               at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
    }
    bool isIdentity(double tol = 1e-12) const {
        for (int i = 0; i < 16; ++i)
            if (std::fabs(m[i] - ((i % 5 == 0) ? 1.0 : 0.0)) > tol) return false;
        return true;
    }
};

inline Mat4 Mat4::inverted() const {
    const double* a = m;
    double inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-300) return r;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}

// Inverse-transpose of the linear 3x3 part of m applied to a covector
// (normals, texture axes).
inline Vec3 transformCovector(const Mat4& m, const Vec3& c) {
    Mat4 inv = m.inverted();
    return {inv.at(0, 0) * c.x + inv.at(1, 0) * c.y + inv.at(2, 0) * c.z,
            inv.at(0, 1) * c.x + inv.at(1, 1) * c.y + inv.at(2, 1) * c.z,
            inv.at(0, 2) * c.x + inv.at(1, 2) * c.y + inv.at(2, 2) * c.z};
}

} // namespace gs
