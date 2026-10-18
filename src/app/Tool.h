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
    PushPull, Move, Rotate, Scale, Offset, Tape, Orbit, Pan, Zoom, ZoomWindow
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
    virtual void buildPreview(Preview&) {}
    virtual void paintOverlay(QPainter&) {}
    virtual QCursor cursor() const;

protected:
    Document& doc();
    Mesh& mesh();
    Camera& cam();
    InferenceEngine& engine();
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
    bool hasLock() const { return lockDir_.has_value(); }
    Color lockColor() const;
    // Colour for a rubber-band line from a to b (axis colours when aligned).
    Color lineColor(const Vec3& a, const Vec3& b) const;

    Viewport* vp_;
    InferenceResult inf_;
    std::optional<Vec3> lockDir_;
    int lockAxis_ = -1;
    bool shiftLock_ = false;
    MouseEvent lastMouse_;
};

} // namespace gs
