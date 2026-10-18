// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - orbit camera (Z-up), perspective or parallel projection.
#pragma once

#include "core/Math.h"

namespace gs {

class Camera {
public:
    Vec3 eye = Vec3(-6.5, -9.0, 5.5);
    Vec3 target = Vec3(0, 0, 0.8);
    Vec3 up = Vec3(0, 0, 1);
    double fovY = 35.0; // degrees
    bool perspective = true;
    int width = 1, height = 1;

    void setViewport(int w, int h) { width = std::max(1, w); height = std::max(1, h); }
    double aspect() const { return double(width) / double(height); }
    double distance() const { return (eye - target).length(); }
    Vec3 forward() const { return (target - eye).normalized(); }
    Vec3 right() const;
    Vec3 trueUp() const;

    Mat4 view() const;
    Mat4 projection() const;
    Mat4 viewProjection() const { return projection() * view(); }
    void clipPlanes(double& zn, double& zf) const;
    void setSceneRadius(const Vec3& center, double radius) { sceneCenter_ = center; sceneRadius_ = radius; }

    // Pixel coordinates (origin top-left).
    Ray ray(double px, double py) const;
    // Returns screen position; false if behind the camera.
    bool project(const Vec3& p, double& sx, double& sy) const;
    // World size of one pixel at point p.
    double pixelSize(const Vec3& p) const;

    void orbit(double dxPixels, double dyPixels);
    void orbitAround(const Vec3& pivot, double dxPixels, double dyPixels);
    void pan(double dxPixels, double dyPixels);
    void panTo(const Vec3& grabbed, double px, double py);
    // Zoom by factor towards a world point (e.g. under the cursor).
    void zoom(double factor, const Vec3* towards = nullptr);
    void zoomExtents(const Vec3& mn, const Vec3& mx);
    void zoomWindow(double x0, double y0, double x1, double y1);
    void setStandardView(const Vec3& dir, const Vec3& upHint);

    Camera lerped(const Camera& o, double t) const;

private:
    Vec3 sceneCenter_;
    double sceneRadius_ = 10;
    double orthoHeight() const;
};

} // namespace gs
