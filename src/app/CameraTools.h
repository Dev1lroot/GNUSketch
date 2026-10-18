// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - camera tools: Orbit, Pan, Zoom, Zoom Window.
#pragma once

#include "Tool.h"

#include <QPointF>

namespace gs {

class OrbitTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Orbit; }
    QString status() const override { return QStringLiteral("Drag to orbit. Shift = pan."); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent&) override { dragging_ = false; }

private:
    bool dragging_ = false;
    QPointF last_;
    Vec3 pivot_, grab_;
};

class PanTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Pan; }
    QString status() const override { return QStringLiteral("Drag to pan."); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent&) override { dragging_ = false; }

private:
    bool dragging_ = false;
    Vec3 grab_;
};

class ZoomTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Zoom; }
    QString status() const override { return QStringLiteral("Drag up or down to zoom. Type a field of view in degrees."); }
    QString vcbLabel() const override { return QStringLiteral("Field of View"); }
    void activate() override;
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent&) override { dragging_ = false; }
    bool vcbEnter(const QString& text) override;

private:
    bool dragging_ = false;
    QPointF last_;
    Vec3 focus_;
};

class ZoomWindowTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::ZoomWindow; }
    QString status() const override { return QStringLiteral("Drag a window around the area to zoom into."); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void paintOverlay(QPainter& p) override;

private:
    bool dragging_ = false;
    QPointF a_, b_;
};

// Returns a good orbit/zoom focus point under the cursor (geometry or ground).
Vec3 focusPointAt(InferenceEngine& eng, const Camera& cam, double x, double y);

} // namespace gs
