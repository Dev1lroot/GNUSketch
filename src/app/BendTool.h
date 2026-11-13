// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Bend tool: click the start and end of the stretch to bend, then
// set the angle with the mouse or by typing. See BendParams in core/Effects.h.
#pragma once

#include "Tool.h"
#include "core/Effects.h"

#include <utility>
#include <vector>

namespace gs {

class BendTool : public Tool {
public:
    explicit BendTool(Viewport* vp) : Tool(vp) {}
    ToolId id() const override { return ToolId::Bend; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Angle"); }
    void reset() override;
    bool keyPress(QKeyEvent* e) override;
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    bool ensureSelection(const MouseEvent& e);
    Vec3 axis() const;
    bool params(double angle, BendParams& out) const;
    void collectGhost();
    void commit();

    int stage_ = 0;
    int forcedAxis_ = -1;
    int heldSaved_ = -1;
public:
    void holdAxis(int axis) override;
private:
    Vec3 start_, end_, cur_;
    double angle_ = 0;
    int segments_ = 0;
    bool snapped_ = false;
    QString message_;
    std::vector<std::pair<Vec3, Vec3>> ghost_; // selected edges in world coordinates
};

} // namespace gs
