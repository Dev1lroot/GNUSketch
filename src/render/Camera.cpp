// SPDX-License-Identifier: GPL-3.0-or-later
#include "Camera.h"

namespace gs {

Vec3 Camera::right() const {
    Vec3 r = forward().cross(up);
    if (r.length2() < 1e-12) r = anyPerpendicular(forward());
    return r.normalized();
}

Vec3 Camera::trueUp() const { return right().cross(forward()).normalized(); }

Mat4 Camera::view() const { return Mat4::lookAt(eye, target, up); }

double Camera::orthoHeight() const {
    // Parallel projection shows the same extent as perspective at the target.
    return 2.0 * distance() * std::tan(fovY * kPi / 360.0);
}

void Camera::clipPlanes(double& zn, double& zf) const {
    double d = (sceneCenter_ - eye).length();
    double r = std::max(sceneRadius_, 1.0);
    zf = d + r * 2.0 + distance() * 2.0 + 100.0;
    zf = std::max(zf, 1000.0);
    // Near plane scaled with camera distance to keep depth precision.
    zn = std::clamp(distance() * 0.002, 0.001, 5.0);
    if (!perspective) {
        zn = -zf;
    }
}

Mat4 Camera::projection() const {
    double zn, zf;
    clipPlanes(zn, zf);
    if (perspective) return Mat4::perspective(fovY * kPi / 180.0, aspect(), zn, zf);
    double h = orthoHeight() * 0.5, w = h * aspect();
    return Mat4::ortho(-w, w, -h, h, zn, zf);
}

Ray Camera::ray(double px, double py) const {
    double nx = 2.0 * px / width - 1.0;
    double ny = 1.0 - 2.0 * py / height;
    Vec3 f = forward(), r = right(), u = trueUp();
    double th = std::tan(fovY * kPi / 360.0);
    if (perspective) {
        Vec3 d = f + r * (nx * th * aspect()) + u * (ny * th);
        return {eye, d.normalized()};
    }
    double h = orthoHeight() * 0.5, w = h * aspect();
    Vec3 o = eye + r * (nx * w) + u * (ny * h);
    // Start far behind so that geometry behind the eye is still pickable.
    return {o - f * 1e4, f};
}

bool Camera::project(const Vec3& p, double& sx, double& sy) const {
    double w;
    Vec3 c = viewProjection().transformPoint(p, &w);
    if (perspective && w <= 1e-9) return false;
    sx = (c.x + 1.0) * 0.5 * width;
    sy = (1.0 - c.y) * 0.5 * height;
    return true;
}

double Camera::pixelSize(const Vec3& p) const {
    double d = perspective ? std::max(1e-6, (p - eye).dot(forward())) : distance();
    return 2.0 * d * std::tan(fovY * kPi / 360.0) / height;
}

void Camera::orbit(double dx, double dy) { orbitAround(target, dx, dy); }

void Camera::orbitAround(const Vec3& pivot, double dx, double dy) {
    const double k = 0.006; // radians per pixel
    Mat4 yaw = Mat4::rotation(Vec3(0, 0, 1), -dx * k);
    Vec3 rgt = right();
    // Clamp pitch so we never flip over the poles.
    Vec3 f = forward();
    double pitch = std::asin(std::clamp(f.z, -1.0, 1.0));
    double dp = dy * k;
    double newPitch = std::clamp(pitch - dp, -kPi / 2 + 0.01, kPi / 2 - 0.01);
    dp = pitch - newPitch;
    Mat4 pitchM = Mat4::rotation(rgt, -dp);
    Mat4 R = yaw * pitchM;
    eye = pivot + R.transformDir(eye - pivot);
    target = pivot + R.transformDir(target - pivot);
    up = Vec3(0, 0, 1);
}

void Camera::pan(double dx, double dy) {
    double s = pixelSize(target);
    Vec3 d = right() * (-dx * s) + trueUp() * (dy * s);
    eye += d;
    target += d;
}

void Camera::panTo(const Vec3& grabbed, double px, double py) {
    // Move the camera so that `grabbed` stays under the cursor.
    double gx, gy;
    if (!project(grabbed, gx, gy)) return;
    double s = pixelSize(grabbed);
    Vec3 d = right() * (-(px - gx) * s) + trueUp() * ((py - gy) * s);
    eye += d;
    target += d;
}

void Camera::zoom(double factor, const Vec3* towards) {
    // factor < 1 zooms in.
    if (!perspective) {
        Vec3 dir = eye - target;
        Vec3 shift;
        if (towards) {
            // Keep the point under the cursor fixed while scaling the view.
            Vec3 tp = *towards - target;
            Vec3 lateral = tp - forward() * tp.dot(forward());
            shift = lateral * (1.0 - factor);
        }
        target += shift;
        eye = target + dir * factor;
        return;
    }
    Vec3 p = towards ? *towards : target;
    eye = p + (eye - p) * factor;
    target = p + (target - p) * factor;
    // Keep the orbit target at a sensible distance in front of the camera.
    if (distance() < 0.01) target = eye + forward() * 0.01;
}

void Camera::zoomExtents(const Vec3& mn, const Vec3& mx) {
    Vec3 c = (mn + mx) * 0.5;
    double r = std::max((mx - mn).length() * 0.5, 0.5);
    Vec3 f = forward();
    double half = std::tan(fovY * kPi / 360.0);
    double halfMin = std::min(half, half * aspect());
    double d = r / std::sin(std::atan(halfMin)) * 1.05;
    target = c;
    eye = c - f * d;
}

void Camera::zoomWindow(double x0, double y0, double x1, double y1) {
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double w = std::max(std::fabs(x1 - x0), 4.0), h = std::max(std::fabs(y1 - y0), 4.0);
    double factor = std::max(w / width, h / height);
    Ray r = ray(cx, cy);
    // Target the point at the current target depth under the window centre.
    double depth = (target - eye).dot(forward());
    double t = depth / std::max(1e-9, r.dir.dot(forward()));
    Vec3 p = perspective ? r.at(t) : r.origin + forward() * (1e4 + depth);
    Vec3 off = p - target;
    eye += off;
    target += off;
    zoom(factor);
}

void Camera::setStandardView(const Vec3& dir, const Vec3& upHint) {
    double d = distance();
    eye = target - dir.normalized() * d;
    up = upHint;
    if (std::fabs(dir.normalized().z) > 0.999) {
        // Looking straight down/up: nudge so the Z-up lookAt stays defined.
        eye = target - dir.normalized() * d + Vec3(0, -1e-6 * d, 0);
    }
    up = Vec3(0, 0, 1);
}

Camera Camera::lerped(const Camera& o, double t) const {
    Camera c = *this;
    c.eye = lerp(eye, o.eye, t);
    c.target = lerp(target, o.target, t);
    return c;
}

} // namespace gs
