// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - rigid bodies for Game Mode: components that fall, slide and tumble.
//
// A small impulse solver. Contacts come from vertices of one thing against
// triangles of another (both ways round, so a box lands on a thin post too);
// edge against edge is not tested. Friction is Coulomb, and bodies at rest sleep.
#pragma once

#include "Math.h"
#include "Walker.h"

#include <functional>
#include <set>
#include <vector>

namespace gs {

struct PhysicsParams {
    double mass = 10;       // kg
    double friction = 0.5;  // 0 ice .. 1 rubber
    double bounce = 0.1;    // 0 .. 1
};

class PhysicsWorld {
public:
    // A body made of the given collision bodies of the world, which it moves.
    // Returns its id. worldPoints are its corners where it stands now.
    int addBody(const PhysicsParams& params, const std::vector<int>& collisionBodies, const CollisionWorld& world);
    // Advances; moved(body) must update the collision world from transform(body).
    void step(double seconds, CollisionWorld& world, const std::function<void(int)>& moved);
    // The motion since addBody: apply it on the left of the original transforms.
    Mat4 transform(int body) const;
    int count() const { return (int)bodies_.size(); }
    bool asleep(int body) const { return bodies_[body].asleep; }
    void wakeAll();
    Vec3 velocity(int body) const { return bodies_[body].vel; }
    Vec3 centre(int body) const { return bodies_[body].pos; }
    double mass(int body) const { return 1 / bodies_[body].invMass; }
    // Pushes it with an impulse (N s) at a world point; the body wakes.
    void applyImpulse(int body, const Vec3& impulse, const Vec3& at);
    const std::vector<int>& collisionBodies(int body) const { return bodies_[body].cols; }

private:
    struct Body {
        double invMass = 1, friction = 0.5, bounce = 0.1;
        Vec3 invInertia;           // diagonal, in its own axes
        Vec3 pos, com0, vel, omega;
        double q[4] = {1, 0, 0, 0}; // w x y z
        std::vector<Vec3> points;  // relative to the centre, unrotated
        std::vector<int> cols;
        double radius = 0;         // the farthest corner from the centre
        bool asleep = false;
        double still = 0;
    };
    struct Contact {
        int a, b; // b = -1: something that does not move
        Vec3 p, n; // n pushes a away from b
        double depth;
        double jn = 0, jt1 = 0, jt2 = 0;
        Vec3 t1, t2;
        double bias = 0;
    };
    void substep(double dt, CollisionWorld& world, const std::function<void(int)>& moved);
    void collect(int i, const CollisionWorld& world, std::vector<Contact>& out);
    Vec3 applyInvInertia(const Body& b, const Vec3& v) const;
    Vec3 pointVelocity(const Body& b, const Vec3& p) const { return b.vel + b.omega.cross(p - b.pos); }
    void addImpulse(Body& b, const Vec3& j, const Vec3& p);

    std::vector<Body> bodies_;
    std::vector<int> colOwner_; // collision body -> physics body, or -1
    double accumulator_ = 0;
};

} // namespace gs
