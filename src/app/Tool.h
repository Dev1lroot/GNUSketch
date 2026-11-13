// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - tool base class (SketchUp-style modal tools).
#pragma once

#include "Inference.h"
#include "core/Document.h"
#include "render/Renderer.h"

#include <QCursor>
#include <QString>

#include <optional>

class QKeyEvent;
class QPainter;

namespace gs {

class Viewport;

enum class ToolId {
    Select, Eraser, Paint, Line, Freehand, Rectangle, Circle, Polygon, Arc,
    PushPull, Move, Rotate, Scale, Offset, FollowMe, Tape, Orbit, Pan, Zoom, ZoomWindow,
    Mountain, Pit, Flatten, TextureBrush,
    SolidUnion, SolidIntersect, SolidSubtract, SolidTrim, SolidSplit,
    PlaceCharacter, Bend, TexturePosition,
    PlacePointLight, PlaceSpotLight, PlaceDirectionalLight, PlaceCamera
};

struct MouseEvent {
    double x = 0, y = 0;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers mods;
    bool ctrl() const { return mods & Qt::ControlModifier; }
    bool shift() const { return mods & Qt::ShiftModifier; }
    bool alt() const { return mods & Qt::AltModifier; }
};

class Tool {
public:
    explicit Tool(Viewport* vp) : vp_(vp) {}
    virtual ~Tool() = default;

    virtual ToolId id() const = 0;
    virtual QString status() const { return {}; }
    virtual QString vcbLabel() const { return QStringLiteral("Measurements"); }
    virtual void activate() {}
    virtual void deactivate() { reset(); }
    // Cancels any operation in progress (Esc).
    virtual void reset() {}
    virtual bool busy() const { return false; }

    virtual void mousePress(const MouseEvent&) {}
    virtual void mouseMove(const MouseEvent&) {}
    virtual void mouseRelease(const MouseEvent&) {}
    virtual void mouseDoubleClick(const MouseEvent&) {}
    virtual bool keyPress(QKeyEvent* e);
    virtual bool keyRelease(QKeyEvent* e);
    virtual bool vcbEnter(const QString&) { return false; }
    // X/Y/Z held down: keep the tool on that axis (0 red, 1 green, 2 blue)
    // until called with -1. Tools with an axis of their own (the plane of a
    // shape, the axis of a rotation or bend) use that; others lock their
    // direction to it.
    virtual void holdAxis(int axis);
    // Mouse wheel (steps of one notch, positive = away from the user). Return
    // true to consume it instead of zooming.
    virtual bool wheel(double /*steps*/, Qt::KeyboardModifiers) { return false; }
    virtual void buildPreview(Preview&) {}
    virtual void paintOverlay(QPainter&) {}
    virtual QCursor cursor() const;

protected:
    Document& doc();
    Mesh& mesh();
    Camera& cam();
    InferenceEngine& engine();
    // Default free-space plane normal: ground, or the vertical axis plane facing
    // the camera when the view is close to horizontal.
    Vec3 viewPlaneNormal();
    void update();
    void refreshStatus();
    void setVcb(const QString& value);
    QString fmtLen(double meters) const;
    bool parseLen(const QString& text, double& meters) const;
    // Parses "a,b" (or "a;b") into two lengths; missing parts stay unset.
    bool parseLen2(const QString& text, std::optional<double>& a, std::optional<double>& b) const;

    // Inference with the tool's current axis/shift locks.
    InferenceResult inferPoint(const MouseEvent& e, std::optional<Vec3> ref,
                               std::optional<Plane> plane = std::nullopt,
                               const std::set<Id>* exclude = nullptr, bool axes = true);
    void clearLocks();
    // Length snapping (Model Info > Units): a length in whole steps.
    double snapLen(double meters);
    // Whether a point found this way may be moved to a whole length from the
    // reference point (not when it sits on a vertex, an edge or a guide).
    static bool lengthSnappable(const InferenceResult& r);
    // inferPoint() snaps the distance from the reference point (tools that
    // snap their sizes themselves turn this off).
    bool snapDistance_ = true;
    bool hasLock() const { return lockDir_.has_value() || heldAxis_ >= 0; }
    Color lockColor() const;
    // Colour for a rubber-band line from a to b (axis colours when aligned).
    Color lineColor(const Vec3& a, const Vec3& b) const;

    Viewport* vp_;
    InferenceResult inf_;
    std::optional<Vec3> lockDir_;
    int lockAxis_ = -1;
    int heldAxis_ = -1; // X/Y/Z held
    bool shiftLock_ = false;
    MouseEvent lastMouse_;
};

} // namespace gs
