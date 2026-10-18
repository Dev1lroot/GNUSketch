// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - drawing tools: Line, Freehand, Rectangle, Circle, Polygon, Arc.
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

private:
    void commit(const Vec3& a, const Vec3& b);
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
    int forcedAxis_ = -1;
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
    bool hasStart_ = false, dragging_ = false;
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
    Id startFace_ = kNone;
    Plane plane_;
    double bulge_ = 0;
    int segments_ = 12;
    std::string hint_;
};

} // namespace gs
