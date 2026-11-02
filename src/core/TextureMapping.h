// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - moving a texture over its faces: an affine change of a face side's
// texture coordinates (move, resize, rotate, skew, mirror).
#pragma once

#include "Mesh.h"

namespace gs {

// (x, y) -> (a x + b y + e, c x + d y + f)
struct UvAffine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec2 apply(const Vec2& p) const { return {a * p.x + b * p.y + e, c * p.x + d * p.y + f}; }
    double det() const { return a * d - b * c; }
    UvAffine inverse() const; // identity if singular
    static UvAffine translate(double dx, double dy);
    static UvAffine scaleAbout(const Vec2& c, double sx, double sy);
    // x moves by k * (y - anchor y) (alongU), or y by k * (x - anchor x).
    static UvAffine shearAbout(const Vec2& anchor, double k, bool alongU);
};

// The side's mapping as an explicit one, also when it uses the default projection.
TexMap explicitTexMap(const Face& f, bool frontSide, const Material* m);
// The map under which the texture appears moved by `moved` (in its own texture
// coordinates): what showed at uv now shows where moved(uv) lay. False if `moved`
// or the map is singular; the map is then left as it was.
bool moveTexMap(TexMap& map, const UvAffine& moved);

// A texture map within one plane: world position <-> texture coordinates.
class PlaneTexMap {
public:
    // False if the texture is seen edge-on from this plane.
    bool set(const TexMap& map, const Vec3& normal, const Vec3& pointOnPlane);
    Vec2 uvAt(const Vec3& p) const;
    Vec3 pointAt(const Vec2& uv) const;
    // Rotation by `angle` in the plane, as seen in texture coordinates, about uv c.
    UvAffine rotationAbout(const Vec2& c, double angle) const;
    // Angle of p seen from uv c, in the plane.
    double angleOf(const Vec2& c, const Vec3& p) const;
    // World length of one texture repeat along u and along v.
    double lengthU() const;
    double lengthV() const;

private:
    TexMap map_;
    Vec3 e1_, e2_, on_;
    double m_[4] = {1, 0, 0, 1}; // plane (x, y) -> uv, linear part
    double t_[2] = {0, 0};       // uv of the plane's origin point
};

} // namespace gs
