// SPDX-License-Identifier: GPL-3.0-or-later
#include "Physics.h"

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

constexpr double kTick = 1.0 / 120.0;
constexpr double kGravity = 9.81;
constexpr double kSlop = 0.002;   // allowed sink, keeps contacts alive between ticks
constexpr int kIterations = 12;

// Rotates by the quaternion (w x y z).
Vec3 rotate(const double q[4], const Vec3& v) {
    const Vec3 u(q[1], q[2], q[3]);
    const Vec3 t = u.cross(v) * 2.0;
    return v + t * q[0] + u.cross(t);
}
Vec3 rotateBack(const double q[4], const Vec3& v) {
    const double c[4] = {q[0], -q[1], -q[2], -q[3]};
    return rotate(c, v);
}

bool insideTriangle(const CollisionWorld::Tri& T, const Vec3& n, const Vec3& p) {
    const auto side = [&](const Vec3& a, const Vec3& b) { return (b - a).cross(p - a).dot(n) >= -1e-9; };
    return side(T.a, T.b) && side(T.b, T.c) && side(T.c, T.a);
}

} // namespace

int PhysicsWorld::addBody(const PhysicsParams& params, const std::vector<int>& cols, const CollisionWorld& world) {
    Body b;
    b.cols = cols;
    b.invMass = 1 / std::max(params.mass, 1e-3);
    b.friction = std::clamp(params.friction, 0.0, 2.0);
    b.bounce = std::clamp(params.bounce, 0.0, 1.0);
    // Corners, centre of the surface area, and the size for the inertia.
    std::vector<Vec3> pts;
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300), sum;
    double area = 0;
    for (int c : cols)
        for (const CollisionWorld::Tri& T : world.bodyTriangles(c)) {
            const double A = (T.b - T.a).cross(T.c - T.a).length() * 0.5;
            sum += (T.a + T.b + T.c) * (A / 3);
            area += A;
            for (const Vec3* p : {&T.a, &T.b, &T.c}) {
                bool known = false;
                for (const Vec3& q : pts) known = known || (q - *p).length2() < 1e-10;
                if (!known && pts.size() < 256) pts.push_back(*p);
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], (*p)[k]);
                    mx[k] = std::max(mx[k], (*p)[k]);
                }
            }
        }
    if (area <= 0) {
        mn = mx = Vec3();
        sum = Vec3();
        area = 1;
    }
    b.com0 = sum / area;
    b.pos = b.com0;
    const Vec3 d = mx - mn;
    const double m = 1 / b.invMass;
    const double minSize = 0.02;
    const double x = std::max(d.x, minSize), y = std::max(d.y, minSize), z = std::max(d.z, minSize);
    b.invInertia = Vec3(12 / (m * (y * y + z * z)), 12 / (m * (x * x + z * z)), 12 / (m * (x * x + y * y)));
    for (const Vec3& p : pts) {
        b.points.push_back(p - b.com0);
        b.radius = std::max(b.radius, (p - b.com0).length());
    }
    bodies_.push_back(std::move(b));
    const int id = (int)bodies_.size() - 1;
    if ((int)colOwner_.size() < world.bodyCount()) colOwner_.resize(world.bodyCount(), -1);
    for (int c : cols) colOwner_[c] = id;
    return id;
}

Mat4 PhysicsWorld::transform(int i) const {
    const Body& b = bodies_[i];
    Mat4 m;
    const Vec3 ex = rotate(b.q, Vec3(1, 0, 0)), ey = rotate(b.q, Vec3(0, 1, 0)), ez = rotate(b.q, Vec3(0, 0, 1));
    for (int r = 0; r < 3; ++r) {
        m.at(r, 0) = ex[r];
        m.at(r, 1) = ey[r];
        m.at(r, 2) = ez[r];
    }
    const Vec3 t = b.pos - rotate(b.q, b.com0);
    for (int r = 0; r < 3; ++r) m.at(r, 3) = t[r];
    return m;
}

void PhysicsWorld::wakeAll() {
    for (Body& b : bodies_) {
        b.asleep = false;
        b.still = 0;
    }
}

Vec3 PhysicsWorld::applyInvInertia(const Body& b, const Vec3& v) const {
    const Vec3 l = rotateBack(b.q, v);
    return rotate(b.q, Vec3(l.x * b.invInertia.x, l.y * b.invInertia.y, l.z * b.invInertia.z));
}

void PhysicsWorld::addImpulse(Body& b, const Vec3& j, const Vec3& p) {
    b.vel += j * b.invMass;
    b.omega += applyInvInertia(b, (p - b.pos).cross(j));
}

void PhysicsWorld::applyImpulse(int i, const Vec3& impulse, const Vec3& at) {
    Body& b = bodies_[i];
    b.asleep = false;
    b.still = 0;
    addImpulse(b, impulse, at);
}

void PhysicsWorld::collect(int i, const CollisionWorld& world, std::vector<Contact>& out) {
    const Body& A = bodies_[i];
    std::vector<Vec3> pts;
    pts.reserve(A.points.size());
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    for (const Vec3& l : A.points) {
        const Vec3 p = A.pos + rotate(A.q, l);
        pts.push_back(p);
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], p[k]);
            mx[k] = std::max(mx[k], p[k]);
        }
    }
    // As deep as it can be sunk: something placed overlapping a surface comes out
    // on the side its centre is on, and does not fall through.
    const double reach = std::max({0.25, A.radius, A.vel.length() * kTick * 3});
    auto add = [&](int other, const Vec3& p, const Vec3& n, double depth) {
        Contact c;
        c.a = i;
        c.b = other;
        c.p = p;
        c.n = n;
        c.depth = depth;
        out.push_back(c);
    };
    // The ground below everything.
    for (const Vec3& p : pts)
        if (p.z < world.floorZ() + kSlop) add(-1, p, Vec3(0, 0, 1), world.floorZ() - p.z);

    // Its corners against a triangle; the triangle's side is the one its centre is on.
    auto cornersOn = [&](const CollisionWorld::Tri& T, int other) {
        const double side = (A.pos - T.a).dot(T.n);
        const Vec3 n = side >= 0 ? T.n : -T.n;
        for (const Vec3& p : pts) {
            const double d = (p - T.a).dot(n);
            if (d < kSlop && d > -reach && insideTriangle(T, T.n, p)) add(other, p, n, -d);
        }
    };
    // A corner of something else against its own surface.
    auto cornerInto = [&](const Vec3& q, int other) {
        for (int c : A.cols)
            for (const CollisionWorld::Tri& U : world.bodyTriangles(c)) {
                const double side = (U.a - A.pos).dot(U.n);
                const Vec3 out2 = side >= 0 ? U.n : -U.n;
                const double d = (q - U.a).dot(out2);
                if (d < kSlop && d > -reach && insideTriangle(U, U.n, q)) add(other, q, -out2, -d);
            }
    };
    const Vec3 pad(0.01, 0.01, 0.01);
    world.forEachStaticNear(mn - pad, mx + pad, [&](const CollisionWorld::Tri& T) {
        cornersOn(T, -1);
        for (const Vec3* q : {&T.a, &T.b, &T.c})
            if (q->x >= mn.x && q->x <= mx.x && q->y >= mn.y && q->y <= mx.y && q->z >= mn.z && q->z <= mx.z) cornerInto(*q, -1);
    });
    for (int c = 0; c < world.bodyCount(); ++c) {
        const int owner = c < (int)colOwner_.size() ? colOwner_[c] : -1;
        if (owner == i) continue;
        Vec3 omn, omx;
        world.bodyBounds(c, omn, omx);
        if (omn.x > mx.x || omx.x < mn.x || omn.y > mx.y || omx.y < mn.y || omn.z > mx.z || omx.z < mn.z) continue;
        for (const CollisionWorld::Tri& T : world.bodyTriangles(c)) {
            cornersOn(T, owner);
            // Between two bodies the other one tests its corners on its own turn,
            // unless it sleeps; something that does not move never does.
            if (owner < 0 || bodies_[owner].asleep)
                for (const Vec3* q : {&T.a, &T.b, &T.c})
                    if (q->x >= mn.x && q->x <= mx.x && q->y >= mn.y && q->y <= mx.y && q->z >= mn.z && q->z <= mx.z) cornerInto(*q, owner);
        }
    }
}

void PhysicsWorld::substep(double dt, CollisionWorld& world, const std::function<void(int)>& moved) {
    for (Body& b : bodies_)
        if (!b.asleep) b.vel.z -= kGravity * dt;

    std::vector<Contact> contacts;
    for (int i = 0; i < (int)bodies_.size(); ++i)
        if (!bodies_[i].asleep) collect(i, world, contacts);
    // Whatever is hit hard enough wakes up, and answers from then on.
    for (const Contact& c : contacts)
        if (c.b >= 0 && bodies_[c.b].asleep && bodies_[c.a].vel.length() > 0.3) {
            bodies_[c.b].asleep = false;
            bodies_[c.b].still = 0;
        }

    for (Contact& c : contacts) {
        const Vec3 helper = std::fabs(c.n.z) < 0.9 ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
        c.t1 = c.n.cross(helper).normalized();
        c.t2 = c.n.cross(c.t1);
        Body& A = bodies_[c.a];
        Vec3 rel = pointVelocity(A, c.p);
        double bounce = A.bounce;
        if (c.b >= 0) {
            rel -= pointVelocity(bodies_[c.b], c.p);
            bounce = std::max(bounce, bodies_[c.b].bounce);
        }
        const double vn = rel.dot(c.n);
        c.bias = vn < -1.0 ? -vn * bounce : 0; // gentle contacts do not bounce
    }

    auto effective = [&](const Contact& c, const Vec3& d) {
        const Body& A = bodies_[c.a];
        Vec3 r = c.p - A.pos;
        double k = A.invMass + applyInvInertia(A, r.cross(d)).cross(r).dot(d);
        if (c.b >= 0 && !bodies_[c.b].asleep) {
            const Body& B = bodies_[c.b];
            r = c.p - B.pos;
            k += B.invMass + applyInvInertia(B, r.cross(d)).cross(r).dot(d);
        }
        return k;
    };
    auto push = [&](Contact& c, const Vec3& d, double j) {
        addImpulse(bodies_[c.a], d * j, c.p);
        if (c.b >= 0 && !bodies_[c.b].asleep) addImpulse(bodies_[c.b], d * -j, c.p);
    };
    for (int it = 0; it < kIterations; ++it)
        for (Contact& c : contacts) {
            const Body& A = bodies_[c.a];
            const bool twoBodies = c.b >= 0 && !bodies_[c.b].asleep;
            Vec3 rel = pointVelocity(A, c.p);
            if (twoBodies) rel -= pointVelocity(bodies_[c.b], c.p);
            const double mu = c.b >= 0 ? std::sqrt(A.friction * bodies_[c.b].friction) : A.friction;
            // Along the normal: it may only push.
            double j = (c.bias - rel.dot(c.n)) / effective(c, c.n);
            const double jn0 = c.jn;
            c.jn = std::max(jn0 + j, 0.0);
            push(c, c.n, c.jn - jn0);
            // Sideways: at most mu times the push.
            for (int k = 0; k < 2; ++k) {
                const Vec3& t = k == 0 ? c.t1 : c.t2;
                double& acc = k == 0 ? c.jt1 : c.jt2;
                Vec3 r2 = pointVelocity(A, c.p);
                if (twoBodies) r2 -= pointVelocity(bodies_[c.b], c.p);
                const double jt = -r2.dot(t) / effective(c, t);
                const double old = acc;
                acc = std::clamp(old + jt, -mu * c.jn, mu * c.jn);
                push(c, t, acc - old);
            }
        }

    // Out of what it sank into, without adding speed.
    std::vector<Vec3> correction(bodies_.size());
    std::vector<int> touching(bodies_.size(), 0);
    for (const Contact& c : contacts) {
        touching[c.a] = 1;
        if (c.b >= 0) touching[c.b] = 1;
        const double fix = std::min(std::max(c.depth - kSlop, 0.0) * 0.2, 0.03); // comes out, does not jump
        if (fix <= 0) continue;
        const Vec3 v = c.n * fix;
        for (int k = 0; k < 3; ++k)
            correction[c.a][k] = std::fabs(v[k]) > std::fabs(correction[c.a][k]) ? v[k] : correction[c.a][k];
    }

    for (int i = 0; i < (int)bodies_.size(); ++i) {
        Body& b = bodies_[i];
        if (b.asleep) continue;
        b.pos += b.vel * dt + correction[i];
        const Vec3 w = b.omega * (dt * 0.5);
        const double* q = b.q;
        double n[4] = {q[0] - w.x * q[1] - w.y * q[2] - w.z * q[3], q[1] + w.x * q[0] + w.y * q[3] - w.z * q[2],
                       q[2] - w.x * q[3] + w.y * q[0] + w.z * q[1], q[3] + w.x * q[2] - w.y * q[1] + w.z * q[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        for (int k = 0; k < 4; ++k) b.q[k] = n[k] / len;
        b.omega = b.omega * 0.999;
        moved(i);
        // Resting on something and hardly moving: stop until disturbed.
        if (touching[i] && b.vel.length() < 0.06 && b.omega.length() < 0.12) {
            b.still += dt;
            if (b.still > 0.5) {
                b.asleep = true;
                b.vel = b.omega = Vec3();
            }
        } else {
            b.still = 0;
        }
    }
}

void PhysicsWorld::step(double seconds, CollisionWorld& world, const std::function<void(int)>& moved) {
    if (bodies_.empty()) return;
    accumulator_ += std::min(seconds, 0.25);
    while (accumulator_ >= kTick) {
        substep(kTick, world, moved);
        accumulator_ -= kTick;
    }
}

} // namespace gs
