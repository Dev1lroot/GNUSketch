// SPDX-License-Identifier: GPL-3.0-or-later
#include "Walker.h"

#include "Components.h"
#include "Triangulate.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gs {

namespace {

constexpr double kTick = 1.0 / 120.0;
constexpr double kSkin = 0.001;

Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// Closest points of segments p1-q1 and p2-q2.
void closestSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const double a = d1.length2(), e = d2.length2(), f = d2.dot(r);
    double s = 0, t = 0;
    if (a <= 1e-18 && e <= 1e-18) {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= 1e-18) {
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = d1.dot(r);
        if (e <= 1e-18) {
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = d1.dot(d2), den = a * e - b * b;
            s = den > 1e-18 ? std::clamp((b * f - c * e) / den, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// Closest points between a segment and a triangle (distance 0 if they cross).
double segmentTriangle(const Vec3& p, const Vec3& q, const CollisionWorld::Tri& T, Vec3& onSeg, Vec3& onTri) {
    const double dp = T.n.dot(p - T.a), dq = T.n.dot(q - T.a);
    if ((dp > 0) != (dq > 0) && std::fabs(dp - dq) > 1e-12) {
        const Vec3 x = lerp(p, q, dp / (dp - dq));
        const Vec3 on = closestOnTriangle(x, T.a, T.b, T.c);
        if ((on - x).length2() < 1e-16) {
            onSeg = onTri = x;
            return 0;
        }
    }
    double best = std::numeric_limits<double>::infinity();
    auto consider = [&](const Vec3& s, const Vec3& t) {
        const double d = (s - t).length2();
        if (d < best) {
            best = d;
            onSeg = s;
            onTri = t;
        }
    };
    consider(p, closestOnTriangle(p, T.a, T.b, T.c));
    consider(q, closestOnTriangle(q, T.a, T.b, T.c));
    const Vec3* tv[3] = {&T.a, &T.b, &T.c};
    for (int i = 0; i < 3; ++i) {
        Vec3 s, t;
        closestSegments(p, q, *tv[i], *tv[(i + 1) % 3], s, t);
        consider(s, t);
    }
    return std::sqrt(best);
}

} // namespace

void CollisionWorld::clear() {
    tris_.clear();
    cells_.clear();
    stamp_.clear();
    floorZ_ = 0;
}

void CollisionWorld::addTriangle(const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 n = (b - a).cross(c - a);
    if (n.length2() < 1e-20) return;
    tris_.push_back({a, b, c, n.normalized()});
}

void CollisionWorld::cellRange(const Vec3& mn, const Vec3& mx, int& x0, int& y0, int& x1, int& y1) const {
    x0 = std::clamp((int)std::floor((mn.x - originX_) / cell_), 0, nx_ - 1);
    y0 = std::clamp((int)std::floor((mn.y - originY_) / cell_), 0, ny_ - 1);
    x1 = std::clamp((int)std::floor((mx.x - originX_) / cell_), 0, nx_ - 1);
    y1 = std::clamp((int)std::floor((mx.y - originY_) / cell_), 0, ny_ - 1);
}

void CollisionWorld::build() {
    floorZ_ = 0;
    nx_ = ny_ = 1;
    cell_ = 1;
    originX_ = originY_ = 0;
    stamp_.assign(tris_.size(), 0);
    stampNow_ = 0;
    if (tris_.empty()) {
        cells_.assign(1, {});
        return;
    }
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    for (const Tri& t : tris_)
        for (const Vec3* p : {&t.a, &t.b, &t.c})
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], (*p)[k]);
                mx[k] = std::max(mx[k], (*p)[k]);
            }
    floorZ_ = std::min(0.0, mn.z);
    originX_ = mn.x;
    originY_ = mn.y;
    cell_ = std::max({1.0, (mx.x - mn.x) / 512, (mx.y - mn.y) / 512});
    nx_ = std::max(1, (int)std::ceil((mx.x - mn.x) / cell_) + 1);
    ny_ = std::max(1, (int)std::ceil((mx.y - mn.y) / cell_) + 1);
    cells_.assign((size_t)nx_ * ny_, {});
    for (int i = 0; i < (int)tris_.size(); ++i) {
        const Tri& t = tris_[i];
        Vec3 a(std::min({t.a.x, t.b.x, t.c.x}), std::min({t.a.y, t.b.y, t.c.y}), 0);
        Vec3 b(std::max({t.a.x, t.b.x, t.c.x}), std::max({t.a.y, t.b.y, t.c.y}), 0);
        int x0, y0, x1, y1;
        cellRange(a, b, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells_[y * nx_ + x].push_back(i);
    }
}

void CollisionWorld::buildFromModel(const ModelState& st) {
    clear();
    auto addMesh = [&](const Mesh& m, const Mat4* xf) {
        for (Id f = 0; f < (Id)m.faces.size(); ++f) {
            const Face& F = m.faces[f];
            if (!F.alive || F.hidden) continue;
            Vec3 u, v;
            planeBasis(F.normal, u, v);
            std::vector<std::vector<Vec2>> loops;
            std::vector<Vec3> pts;
            auto add = [&](const std::vector<Id>& L) {
                loops.emplace_back();
                for (Id id : L) {
                    const Vec3& p = m.pos(id);
                    loops.back().emplace_back(p.dot(u), p.dot(v));
                    pts.push_back(xf ? xf->transformPoint(p) : p);
                }
            };
            add(F.outer);
            for (const auto& h : F.holes) add(h);
            for (const auto& t : triangulatePolygon(loops)) addTriangle(pts[t[0]], pts[t[1]], pts[t[2]]);
        }
    };
    addMesh(st.activeMesh(), nullptr);
    if (st.editing != kNone) addMesh(st.mesh, nullptr);
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        const Instance& I = st.instances[i];
        if (!I.alive || I.hidden || i == st.editing || I.def < 0 || I.def >= (Id)st.defs.size()) continue;
        const Mat4 xf = instanceWorldXf(st, i);
        addMesh(st.defs[I.def].mesh, &xf);
    }
    build();
}

Walker::Walker(const CollisionWorld& world, const WalkerParams& params) : world_(world), p_(params) {
    slopeCos_ = std::cos(p_.maxSlopeDegrees * kPi / 180.0);
}

Vec3 Walker::lookDirection() const {
    const double c = std::cos(pitch_);
    return {std::cos(yaw_) * c, std::sin(yaw_) * c, std::sin(pitch_)};
}

void Walker::look(double dYaw, double dPitch) {
    yaw_ = std::remainder(yaw_ + dYaw, 2 * kPi);
    pitch_ = std::clamp(pitch_ + dPitch, -1.55, 1.55);
}

void Walker::spawn(const Vec3& feet) {
    vel_ = Vec3();
    eyeLag_ = 0;
    accumulator_ = 0;
    feet_ = feet + Vec3(0, 0, kSkin);
    // Starts above the spot it was placed on, so the first push is upwards.
    for (int i = 0; i < 8 && overlaps(feet_); ++i) resolve(feet_, feet_ + Vec3(0, 0, p_.stepHeight));
    grounded_ = false;
}

bool Walker::overlaps(const Vec3& feet) const {
    if (feet.z < world_.floorZ()) return true;
    const Vec3 a = feet + Vec3(0, 0, p_.radius), b = feet + Vec3(0, 0, p_.height - p_.radius);
    const Vec3 pad(p_.radius, p_.radius, p_.radius);
    bool hit = false;
    world_.forEachNear(a - pad, b + pad, [&](const CollisionWorld::Tri& T) {
        if (hit) return;
        Vec3 s, t;
        hit = segmentTriangle(a, b, T, s, t) < p_.radius - kSkin * 0.5;
    });
    return hit;
}

Walker::Contact Walker::resolve(Vec3& feet, const Vec3& from) const {
    Contact c;
    const double r = p_.radius;
    const Vec3 fromCentre = from + Vec3(0, 0, p_.height / 2);
    for (int iter = 0; iter < 6; ++iter) {
        const Vec3 a = feet + Vec3(0, 0, r), b = feet + Vec3(0, 0, p_.height - r);
        const Vec3 pad(r, r, r);
        double deepest = 0, lift = 0;
        Vec3 pushDir;
        world_.forEachNear(a - pad, b + pad, [&](const CollisionWorld::Tri& T) {
            Vec3 s, t;
            const double d = segmentTriangle(a, b, T, s, t);
            if (d >= r) return;
            Vec3 n;
            double depth = r - d;
            double rimLift = 0;
            if (d > 1e-9) {
                n = (s - t) / d;
                // On an edge of a face (a kerb, a step's rim) below the middle of the
                // foot: the round foot rolls up over it, however steep the contact.
                if (std::fabs(n.dot(T.n)) < 0.999 && n.z > 0.05) {
                    const double across = Vec3(s.x - t.x, s.y - t.y, 0).length();
                    rimLift = std::sqrt(std::max(r * r - across * across, 0.0)) - (s.z - t.z);
                }
            } else {
                // The axis runs through the triangle: back the way it came.
                n = T.n.dot(fromCentre - T.a) >= 0 ? T.n : -T.n;
                depth = r + std::max(-n.dot(a - T.a), -n.dot(b - T.a));
            }
            if (depth > deepest) {
                deepest = depth;
                pushDir = n;
                lift = rimLift;
            }
        });
        if (deepest <= 0) break;
        const double push = deepest + kSkin;
        if (lift > 0) {
            feet.z += lift + kSkin;
            c.ground = true;
        } else if (pushDir.z >= slopeCos_) {
            // Walkable: lift straight up, so standing on a slope does not creep.
            feet.z += push / pushDir.z;
            c.ground = true;
        } else {
            feet += pushDir * push;
            if (pushDir.z < -0.5) c.ceiling = true;
            else c.wall = true;
        }
    }
    if (feet.z <= world_.floorZ()) {
        feet.z = world_.floorZ();
        c.ground = true;
    }
    return c;
}

bool Walker::sweepDown(const Vec3& from, double maxDrop, double& z) const {
    // Small enough steps not to pass a thin face; the capsule's foot is much wider.
    const double stride = p_.radius / 3;
    double free = from.z;
    for (double dropped = 0; dropped < maxDrop;) {
        dropped = std::min(dropped + stride, maxDrop);
        const double below = from.z - dropped;
        if (!overlaps(Vec3(from.x, from.y, below))) {
            free = below;
            continue;
        }
        double lo = below, hi = free;
        for (int i = 0; i < 10; ++i) {
            const double mid = (lo + hi) / 2;
            if (overlaps(Vec3(from.x, from.y, mid))) lo = mid;
            else hi = mid;
        }
        z = hi;
        return true;
    }
    z = free;
    return false;
}

void Walker::moveAcross(const Vec3& delta) {
    const Vec3 start = feet_;
    Vec3 plain = start + delta;
    const Contact c = resolve(plain, start);

    if (grounded_ && c.wall) {
        // Blocked while walking: is there room one step up, and something to stand on?
        const Vec3 up = start + Vec3(0, 0, p_.stepHeight);
        if (!overlaps(up)) {
            Vec3 over = up + delta;
            resolve(over, up);
            if (!overlaps(over)) {
                // Down to whatever is there, without passing through it.
                double hi = over.z;
                sweepDown(over, p_.stepHeight, hi);
                const Vec3 landed(over.x, over.y, hi);
                const Vec3 movedPlain = plain - start, movedOver = landed - start;
                const double plainLen = Vec3(movedPlain.x, movedPlain.y, 0).length();
                const double overLen = Vec3(movedOver.x, movedOver.y, 0).length();
                // Something must be under it: not a step into thin air past a wall top.
                if (overLen > plainLen + 1e-5 && hi > start.z + 1e-4 && hi < up.z - kSkin) {
                    eyeLag_ -= hi - start.z;
                    feet_ = landed;
                    stepped_ = true;
                    return;
                }
            }
        }
    }
    feet_ = plain;
}

void Walker::tick(double dt, const WalkInput& in) {
    // Horizontal velocity follows the keys: quickly on the ground, a little in the air.
    const Vec3 fwd(std::cos(yaw_), std::sin(yaw_), 0), right(std::sin(yaw_), -std::cos(yaw_), 0);
    Vec3 wish = fwd * in.forward + right * in.strafe;
    if (wish.length2() > 1) wish = wish.normalized();
    wish *= in.run ? p_.runSpeed : p_.walkSpeed;
    const double accel = grounded_ ? 40.0 : 6.0;
    Vec3 change = wish - Vec3(vel_.x, vel_.y, 0);
    const double most = accel * dt;
    if (change.length() > most) change = change.normalized() * most;
    vel_.x += change.x;
    vel_.y += change.y;

    if (grounded_ && in.jump) {
        vel_.z = p_.jumpSpeed;
        grounded_ = false;
    }
    if (!grounded_) vel_.z -= p_.gravity * dt;
    vel_.z = std::max(vel_.z, -54.0); // terminal velocity

    // Moves at most a third of the radius at a time, so nothing is tunnelled through.
    const Vec3 total = vel_ * dt;
    const int parts = std::max(1, (int)std::ceil(total.length() / (p_.radius / 3)));
    const Vec3 across = Vec3(total.x, total.y, 0) / (double)parts;
    const double dz = total.z / parts;
    const bool wasGrounded = grounded_;
    bool landed = false;
    stepped_ = false;
    for (int i = 0; i < parts; ++i) {
        if (across.length2() > 0) moveAcross(across);
        if (dz != 0) {
            const Vec3 before = feet_;
            feet_.z += dz;
            const Contact c = resolve(feet_, before);
            if (c.ground && dz < 0) landed = true;
            if (c.ceiling && vel_.z > 0) vel_.z = 0;
        }
    }

    // Stays on the surface walking downhill and over steps down; leaves it past
    // an edge. Standing still also runs this, which is how the ground is detected.
    if (vel_.z <= 0 && !stepped_) {
        double z = feet_.z;
        if (sweepDown(feet_, wasGrounded ? p_.stepHeight : 2 * kSkin, z)) {
            const Vec3 on(feet_.x, feet_.y, z);
            Vec3 touching = on - Vec3(0, 0, 2 * kSkin);
            if (resolve(touching, on).ground) {
                if (wasGrounded) eyeLag_ += feet_.z - z;
                feet_ = on;
                landed = true;
            }
        }
    }
    grounded_ = landed || stepped_;
    if (grounded_) vel_.z = 0;
    eyeLag_ *= std::max(0.0, 1.0 - 14.0 * dt);
}

void Walker::advance(double seconds, const WalkInput& input) {
    accumulator_ += seconds;
    while (accumulator_ >= kTick) {
        tick(kTick, input);
        accumulator_ -= kTick;
    }
}

} // namespace gs
