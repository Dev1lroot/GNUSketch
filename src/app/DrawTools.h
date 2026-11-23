// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - drawing tools: Line, Freehand, Rectangle, Circle, Polygon, Arc, 3D Curve.
#pragma once

#include "Tool.h"

#include <vector>

namespace gs {

class LineTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Line; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Length"); }
    void reset() override;
    bool busy() const override { return hasStart_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;
    bool keyPress(QKeyEvent* e) override;
    bool keyRelease(QKeyEvent* e) override;

private:
    void commit(Vec3 a, Vec3 b);
    // The edge a new line starts from (clicked on its midpoint or along it).
    void setBase(const InferenceResult& r);
    std::vector<Id> edgesAt(Id vertex); // visible hard edges at a vertex
    std::optional<std::pair<Vec3, Vec3>> base_; // for the highlight
    bool hasStart_ = false;
    bool dragging_ = false;
    Vec3 start_;
    double pressX_ = 0, pressY_ = 0;
};

class FreehandTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Freehand; }
    QString status() const override { return QStringLiteral("Click and drag to draw a freehand curve."); }
    void reset() override { pts_.clear(); drawing_ = false; }
    bool busy() const override { return drawing_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    bool drawing_ = false;
    std::vector<Vec3> pts_;
    Plane plane_;
    double lastX_ = 0, lastY_ = 0;
};

// Shared plane handling for tools that draw planar shapes from a first point.
class PlanarShapeTool : public Tool {
public:
    using Tool::Tool;
    bool keyPress(QKeyEvent* e) override;

protected:
    // Resolves the drawing plane through p0 (forced axis, start face, hovered face, ground).
    Plane resolvePlane(const MouseEvent& e, const Vec3& p0, Id startFace);
    void shapeBasis(const Vec3& n, Vec3& u, Vec3& v) const;

    // The face a shape is drawn on and the edge direction it lines up with.
    struct Surface {
        bool ok = false;
        Plane plane;
        bool hasDir = false;
        Vec3 dir;
    };
    // Faces (loose or of components) the inferred point lies on.
    std::vector<InferenceEngine::SurfaceAt> surfacesOf(const InferenceResult& r, const MouseEvent& e);
    // Remembers the faces under the first point (call on the first click).
    void beginSurface(const InferenceResult& r, const MouseEvent& e);
    // The face to draw on through p0 (and through p1 if given): one under the
    // first point (on an edge or corner shared by several, the one under the
    // cursor), else the face of what the cursor snaps to if it holds p0.
    // `hover` are the faces of what the cursor snaps to (surfacesOf).
    Surface currentSurface(const MouseEvent& e, const std::vector<InferenceEngine::SurfaceAt>& hover, const Vec3& p0,
                           const std::optional<Vec3>& p1 = std::nullopt);
    // Of several faces (around an edge or corner): the one under the cursor,
    // else the last one chosen; -1 if none of them is. Faces seen almost
    // edge-on (|normal . view ray| < kMinFaceView, about 17 degrees) are left out.
    static constexpr double kMinFaceView = 0.3;
    int chooseSurface(const std::vector<InferenceEngine::SurfaceAt>& c, const MouseEvent& e);
    // u along s.dir (or the default basis of its plane), v square to it in the plane.
    void surfaceBasis(const Surface& s, Vec3& u, Vec3& v) const;
    std::vector<InferenceEngine::SurfaceAt> startSurfaces_;
    std::optional<Plane> lastSurface_; // sticky choice among several faces
    int forcedAxis_ = -1;
    int heldSaved_ = -1; // forcedAxis_ before X/Y/Z was held
public:
    void holdAxis(int axis) override;
protected:
};

class RectangleTool : public PlanarShapeTool {
public:
    using PlanarShapeTool::PlanarShapeTool;
    ToolId id() const override { return ToolId::Rectangle; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Dimensions"); }
    void reset() override;
    bool busy() const override { return hasStart_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    void commit();
    std::vector<Vec3> corners() const;
    // Axis plane implied by the cursor (snap off the ground, drag up blue), or -1.
    int cursorAxis(const MouseEvent& e);
    bool hasStart_ = false, dragging_ = false;
    int autoAxis_ = -1; // sticky vertical plane chosen by dragging along blue
    Vec3 p0_;
    Id startFace_ = kNone;
    Plane plane_;
    Vec3 u_, v_;
    double w_ = 0, h_ = 0;
    std::string hint_;
    double pressX_ = 0, pressY_ = 0;
};

class CircleTool : public PlanarShapeTool {
public:
    CircleTool(Viewport* vp, bool polygon) : PlanarShapeTool(vp), polygon_(polygon), sides_(polygon ? 6 : 24) {}
    ToolId id() const override { return polygon_ ? ToolId::Polygon : ToolId::Circle; }
    QString status() const override;
    QString vcbLabel() const override;
    void reset() override;
    bool busy() const override { return hasCenter_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    std::vector<Vec3> points() const;
    void commit();
    bool polygon_;
    int sides_;
    bool hasCenter_ = false, dragging_ = false;
    Vec3 center_, radiusDir_ = Vec3(1, 0, 0);
    double radius_ = 0;
    Plane plane_;
    Id startFace_ = kNone;
    double pressX_ = 0, pressY_ = 0;
};

class ArcTool : public PlanarShapeTool {
public:
    using PlanarShapeTool::PlanarShapeTool;
    ToolId id() const override { return ToolId::Arc; }
    QString status() const override;
    QString vcbLabel() const override;
    void reset() override;
    bool busy() const override { return stage_ > 0; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    std::vector<Vec3> points() const;
    void commit();
    Plane arcPlane(const MouseEvent& e);
    int stage_ = 0; // 0 none, 1 start set, 2 end set (bulge)
    Vec3 a_, b_, cur_;
    std::vector<InferenceEngine::SurfaceAt> endSurfaces_; // faces under the end point
    Id startFace_ = kNone;
    Plane plane_;
    double bulge_ = 0;
    int segments_ = 12;
    std::string hint_;
};

// 3D Curve: draw a straight line, then move its anchor points anywhere in
// space; the curve is a smooth spline through them.
class Curve3DTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Curve3D; }
    QString status() const override;
    QString vcbLabel() const override;
    void reset() override;
    void deactivate() override; // a curve being edited is kept
    bool busy() const override { return stage_ > 0; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool keyPress(QKeyEvent* e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

    const std::vector<Vec3>& anchors() const { return anchors_; }
    int activeAnchor() const { return active_; } // being moved, or -1
    // The edges the curve becomes; span[i] is the anchor span point i ends.
    std::vector<Vec3> curvePoints(std::vector<int>* span = nullptr, int segments = 0) const;

private:
    void finishLine(const Vec3& end);
    void commit();
    void setAnchorCount(int n);
    void placeAnchor(const MouseEvent& e);
    void pick(int anchor, const MouseEvent& e);
    int anchorAt(double x, double y) const;
    // Point on the curve under the cursor and the anchor index it would get.
    bool curveAt(double x, double y, Vec3& p, int& insertAt) const;
    int stage_ = 0; // 0 nothing, 1 start set, 2 moving anchors
    Vec3 start_;
    std::vector<Vec3> anchors_;
    int anchorCount_ = 4;
    int segments_ = 24;
    int active_ = -1; // anchor being moved
    int hot_ = -1;    // anchor under the cursor
    int last_ = -1;   // anchor moved last (typed coordinates move it)
    Vec3 orig_;       // the active anchor before it moved
    bool dragging_ = false;
    bool travelled_ = false; // the mouse went more than a few pixels while pressed
    double pressX_ = 0, pressY_ = 0;
};

} // namespace gs
