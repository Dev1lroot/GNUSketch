// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Game Mode physics: a character walking on the model.
//
// The character is an upright capsule. It collides with every visible face
// from both sides, climbs low steps, walks up slopes that are not too steep and
// slides off the rest. Below the model there is a floor at the ground plane (or
// at the lowest point of the model if that is lower), so it can never fall forever.
#pragma once

#include "Document.h"

#include <vector>

namespace gs {

// All triangles of a model in world coordinates, with a grid for fast lookup.
class CollisionWorld {
public:
    void clear();
    void addTriangle(const Vec3& a, const Vec3& b, const Vec3& c);
    // Prepares the lookup; call after the last addTriangle.
    void build();
    // Top-level geometry and all visible component instances.
    void buildFromModel(const ModelState& st);

    struct Tri {
        Vec3 a, b, c, n; // n: unit normal
    };
    const std::vector<Tri>& triangles() const { return tris_; }
    double floorZ() const { return floorZ_; }
    // A separate set of triangles that can move (doors, buttons...). Returns its id.
    int addBody(const Mesh& m, const Mat4& xf);
    void setBodyTransform(int body, const Mat4& xf);
    // After a mesh deformed, not just moved.
    void setBodyMesh(int body, const Mesh& m, const Mat4& xf);
    int bodyCount() const { return (int)bodies_.size(); }
    const std::vector<Tri>& bodyTriangles(int body) const { return bodies_[body].world; }
    void bodyBounds(int body, Vec3& mn, Vec3& mx) const { mn = bodies_[body].mn; mx = bodies_[body].mx; }
    // Nearest hit along the ray within maxDist; body = -1 for static geometry.
    bool raycast(const Vec3& origin, const Vec3& dir, double maxDist, double& t, int& body) const;

    // Calls fn(tri) for the triangles whose bounds may touch the box.
    template <typename Fn>
    void forEachNear(const Vec3& mn, const Vec3& mx, Fn fn) const {
        for (const Body& b : bodies_)
            if (b.mn.x <= mx.x && b.mx.x >= mn.x && b.mn.y <= mx.y && b.mx.y >= mn.y && b.mn.z <= mx.z && b.mx.z >= mn.z)
                for (const Tri& t : b.world) fn(t);
        forEachStaticNear(mn, mx, fn);
    }
    // The same for the static geometry only.
    template <typename Fn>
    void forEachStaticNear(const Vec3& mn, const Vec3& mx, Fn fn) const {
        if (tris_.empty()) return;
        ++stampNow_;
        int x0, y0, x1, y1;
        cellRange(mn, mx, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (int t : cells_[y * nx_ + x]) {
                    if (stamp_[t] == stampNow_) continue;
                    stamp_[t] = stampNow_;
                    fn(tris_[t]);
                }
    }

private:
    struct Body {
        std::vector<Tri> local, world;
        Vec3 mn, mx;
    };
    void cellRange(const Vec3& mn, const Vec3& mx, int& x0, int& y0, int& x1, int& y1) const;

    std::vector<Tri> tris_;
    std::vector<Body> bodies_;
    std::vector<std::vector<int>> cells_;
    mutable std::vector<unsigned> stamp_;
    mutable unsigned stampNow_ = 0;
    double originX_ = 0, originY_ = 0, cell_ = 1, floorZ_ = 0;
    int nx_ = 1, ny_ = 1;
};

struct WalkerParams {
    double height = 1.80;   // capsule height, meters
    double radius = 0.15;   // capsule radius (0.30 m in diameter)
    double eyeHeight = 1.66;
    double walkSpeed = 3.0; // meters per second
    double runSpeed = 6.0;
    double jumpSpeed = 4.2; // about 0.9 m high
    double gravity = 9.81;
    double stepHeight = 0.35;
    double maxSlopeDegrees = 50;
};

struct WalkInput {
    double forward = 0, strafe = 0; // -1..1
    bool run = false;
    bool jump = false;
};

class Walker {
public:
    explicit Walker(const CollisionWorld& world, const WalkerParams& params = {});

    const WalkerParams& params() const { return p_; }
    // Places the feet at this point, moved out of any geometry it overlaps.
    void spawn(const Vec3& feet);
    // Advances the simulation (internally in fixed small steps).
    void advance(double seconds, const WalkInput& input);
    // Turns the view: yaw to the left and pitch upwards, in radians.
    void look(double dYaw, double dPitch);

    const Vec3& feet() const { return feet_; }
    const Vec3& velocity() const { return vel_; }
    bool grounded() const { return grounded_; }
    double yaw() const { return yaw_; } // radians; 0 looks along +X, grows towards +Y
    double pitch() const { return pitch_; }
    void setYaw(double y) { yaw_ = y; }
    Vec3 eye() const { return feet_ + Vec3(0, 0, p_.eyeHeight + eyeLag_); }
    Vec3 lookDirection() const;
    // True if a capsule at these feet would overlap the model.
    bool overlaps(const Vec3& feet) const;
    // The capsule comes within margin of a moving body.
    bool touches(int body, double margin) const;

private:
    struct Contact {
        bool ground = false, wall = false, ceiling = false;
    };
    void tick(double dt, const WalkInput& input);
    void moveAcross(const Vec3& delta);
    // Lowers the capsule at most maxDrop: z = last free height; true if it hit something.
    bool sweepDown(const Vec3& from, double maxDrop, double& z) const;
    Contact resolve(Vec3& feet, const Vec3& from) const;

    const CollisionWorld& world_;
    WalkerParams p_;
    Vec3 feet_, vel_;
    bool grounded_ = false;
    bool stepped_ = false; // climbed a step in this tick
    double yaw_ = 0, pitch_ = 0;
    double accumulator_ = 0;
    double eyeLag_ = 0; // eases the view over steps
    double slopeCos_ = 0;
};

} // namespace gs
