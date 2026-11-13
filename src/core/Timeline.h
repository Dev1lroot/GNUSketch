// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the Animator's timeline (like a Flash timeline): layers, each
// moving one component placement, light or camera through keyframes; a
// motion tween between two keyframes moves it smoothly, otherwise it holds.
//
// Frames are numbered from 1. A layer's pose before its first keyframe is that
// keyframe's, after the last one the last one's.
#pragma once

#include "Material.h"
#include "Math.h"

#include <string>
#include <vector>

namespace gs {

struct ModelState;
using Id = int32_t;

struct TimelineKey {
    int frame = 1;
    bool tween = true; // motion tween from this keyframe to the next one
    // Component placement: its transform.
    Mat4 xf;
    // Light or camera.
    Vec3 pos, target;
    Color color;
    double intensity = 0, range = 0, cone = 0, fov = 0;
    bool enabled = true;
};

struct TimelineLayer {
    enum Kind { Instance, Object };
    std::string name;
    Kind kind = Instance;
    Id target = -1;      // instance or scene object id
    bool locked = false; // its keyframes cannot be changed
    Color color = Color(0.3f, 0.6f, 1.0f);
    std::vector<TimelineKey> keys; // sorted by frame, one per frame
    // Keyframe at exactly this frame (index), or -1.
    int keyAt(int frame) const;
    // The keyframe at or before frame (index), or -1 (also when before the first).
    int keyBefore(int frame) const;
};

struct Timeline {
    double fps = 24;
    int length = 120; // frames shown (grows with the last keyframe)
    std::vector<TimelineLayer> layers;
    int lastKeyFrame() const;
};

// Whether a layer's target still exists.
bool layerTargetAlive(const ModelState& st, const TimelineLayer& l);
// The target's current pose as a keyframe at `frame` (tween as given).
TimelineKey captureKey(const ModelState& st, const TimelineLayer& l, int frame, bool tween = true);
// The layer's pose at a (fractional) frame; false if it has no keyframes.
bool evaluateLayer(const TimelineLayer& l, double frame, TimelineKey& out);
// Puts the target in the pose; returns true if anything changed.
bool applyKey(ModelState& st, const TimelineLayer& l, const TimelineKey& k);
// Poses every layer at the frame. Not while a component is open (its
// contents then hold world coordinates). Returns true if anything moved.
bool applyTimeline(ModelState& st, double frame);
// The pose stored in k differs from the target's current pose.
bool keyDiffers(const ModelState& st, const TimelineLayer& l, const TimelineKey& k);
// Inserts or replaces a keyframe; keeps the order. Returns its index.
int setKey(TimelineLayer& l, const TimelineKey& k);
// Transform part way between two (with rotation, scale and position blended
// separately, so a turning component does not shrink on the way).
Mat4 interpolateTransform(const Mat4& a, const Mat4& b, double t);

} // namespace gs
