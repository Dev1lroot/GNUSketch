// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Position Texture tool: move, resize, rotate, skew and mirror the
// texture of the selected faces directly on the model, in real time.
#pragma once

#include "Tool.h"
#include "core/TextureMapping.h"

#include <QImage>
#include <QPointF>

#include <optional>
#include <vector>

namespace gs {

class TextureTool : public Tool {
public:
    explicit TextureTool(Viewport* vp) : Tool(vp) {}
    ToolId id() const override { return ToolId::TexturePosition; }
    QString status() const override;
    QString vcbLabel() const override;
    void activate() override;
    void reset() override;
    bool busy() const override { return drag_ != Handle::None; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;
    void paintOverlay(QPainter& p) override;

    // Corners, edges and skew handles count from the texture's bottom left /
    // bottom edge, anticlockwise in texture coordinates.
    enum class Handle { None, Move, Rotate, Corner0, Corner1, Corner2, Corner3, Edge0, Edge1, Edge2, Edge3, Skew0, Skew1, Skew2, Skew3 };
    // Where a handle is on the screen right now (for the self test).
    std::optional<QPointF> handleAt(Handle h);

private:
    struct Target {
        Id face;
        bool front;
        TexMap start;
    };
    struct Gizmo {
        PlaneTexMap plane;
        Plane facePlane;
        Vec2 corner; // the tile's lower corner in texture coordinates
        bool ok = false;
    };
    FaceSide& sideOf(const Target& t);
    std::vector<Target> currentTargets();
    Gizmo gizmo();
    bool cursorOnPlane(const MouseEvent& e, const Gizmo& g, Vec3& p) const;
    Handle handleUnder(const MouseEvent& e);
    bool screen(const Vec3& p, QPointF& out) const;
    void applyMoved(const UvAffine& moved);
    void updateDrag(const MouseEvent& e);
    void commitDrag();
    void cancelDrag();
    void selectUnder(const MouseEvent& e);

    Id reference_ = kNone;
    Handle hover_ = Handle::None;
    Handle drag_ = Handle::None;
    Gizmo dragGizmo_;
    std::vector<Target> dragTargets_;
    Selection dragSelection_;
    Vec3 grabbed_;
    Vec2 grabbedUv_;
    double grabbedAngle_ = 0;
    double pressX_ = 0, pressY_ = 0;
    bool movedMouse_ = false;  // moved far enough: release ends the drag
    bool clickedOther_ = false;
    bool sticky_ = false;      // a handle was clicked: it follows until the next click
    UvAffine moved_;
    QString message_;
    QImage image_;
    uint64_t imageRevision_ = 0;
};

} // namespace gs
