// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Terraform tools: Mountain, Pit and Flatten sculpting brushes.
#pragma once

#include "Tool.h"
#include "core/Terrain.h"

#include <QElapsedTimer>
#include <QObject>

#include <memory>
#include <optional>

class QTimer;

namespace gs {

// Brush settings shared by the three Terraform tools (edited in the tray,
// remembered between sessions).
class TerrainSettings : public QObject {
    Q_OBJECT
public:
    static TerrainSettings& instance();
    const TerrainBrush& brush() const { return brush_; }
    void setBrush(const TerrainBrush& b);

signals:
    void changed();

private:
    TerrainSettings();
    TerrainBrush brush_;
};

bool isTerrainTool(ToolId id);

// Press and hold on a surface: Mountain raises it, Pit sinks it and Flatten
// levels it to the height under the cursor where the stroke started. The
// brush follows the cursor while the button is held.
class TerrainTool : public Tool {
public:
    TerrainTool(Viewport* vp, ToolId id);
    ~TerrainTool() override;
    ToolId id() const override { return id_; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Radius"); }
    void reset() override;
    bool busy() const override { return stroke_.has_value(); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    TerrainMode mode() const;
    bool surfaceAt(double x, double y, Vec3& p, Id& face, Vec3& toViewer);
    void tick();
    void applyElapsed();

    ToolId id_;
    std::optional<TerrainStroke> stroke_;
    std::unique_ptr<QTimer> timer_;
    QElapsedTimer clock_;
    double applied_ = 0; // seconds of brushing applied in this stroke
    // Brush under the cursor.
    bool onSurface_ = false;
    Vec3 center_, normal_ = Vec3(0, 0, 1);
};

} // namespace gs
