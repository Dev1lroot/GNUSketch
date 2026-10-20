// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - selection and modification tools.
#pragma once

#include "Tool.h"

#include <QElapsedTimer>
#include <QPointF>

#include <array>
#include <map>
#include <vector>

namespace gs {

// Expands selected edges to whole curves (arcs/circles).
void expandCurves(const Mesh& m, std::set<Id>& edges);
// Vertex set referenced by a selection.
std::set<Id> selectionVertices(const Mesh& m, const Selection& s);
// Everything connected to the given seed edges/faces.
void selectConnected(const Mesh& m, Selection& s);

class SelectTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Select; }
    QString status() const override;
    void reset() override { dragging_ = false; pressed_ = false; update(); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    void paintOverlay(QPainter& p) override;

private:
    void applyPick(const Selection& picked, Qt::KeyboardModifiers mods);
    void rectSelect(Qt::KeyboardModifiers mods);
    bool pressed_ = false, dragging_ = false;
    QPointF pressPos_, curPos_;
    QElapsedTimer dblTimer_;
    QPointF dblPos_;
};

class EraserTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Eraser; }
    QString status() const override;
    void reset() override { marked_.clear(); markedInstances_.clear(); erasing_ = false; update(); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    void markAt(const MouseEvent& e);
    bool erasing_ = false;
    std::set<Id> marked_, markedInstances_;
    std::set<int> markedGuides_;
    Id hover_ = kNone;
};

class PaintTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Paint; }
    QString status() const override;
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    Id hover_ = kNone;
};

class PushPullTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::PushPull; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Distance"); }
    void reset() override;
    bool busy() const override { return pulling_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    void commit(double dist, bool keep);
    bool pulling_ = false, dragged_ = false;
    Id face_ = kNone, hover_ = kNone;
    Vec3 start_, normal_;
    double dist_ = 0, lastDist_ = 0;
    Id lastFace_ = kNone; // face id (in the state before the last commit)
    bool lastKeep_ = false, canAdjust_ = false;
    double pressX_ = 0, pressY_ = 0;
    std::set<Id> faceVerts_;
};

// Shared machinery for tools that transform the selection live (move/rotate/scale).
class TransformTool : public Tool {
public:
    using Tool::Tool;
    bool busy() const override { return active_; }
    void reset() override;
    bool keyPress(QKeyEvent* e) override;
    bool keyRelease(QKeyEvent* e) override;

protected:
    bool ensureSelection(const MouseEvent& e);
    bool begin(const char* opName, bool copy);
    void applyLive(const Mat4& m);
    void finish(const Mat4& m);
    void cancelLive();
    void toggleCopy();
    virtual Mat4 currentTransform() const = 0;
    // Creates n copies of the original selection transformed by m^k (k=1..n).
    void makeArray(const Mat4& step, int n);

    bool active_ = false, copy_ = false;
    std::string opName_;
    std::set<Id> verts_;
    std::map<Id, Vec3> orig_;
    std::map<Id, Mat4> instOrig_; // selected component instances: their own transform only
    std::vector<Id> affectedFaces_;
    Selection origSelection_;
    // Last finished copy (for array entry "3x" / "/3").
    bool lastWasCopy_ = false;
    Selection lastSourceSel_;
    Mat4 lastTransform_;
    Id hoverFace_ = kNone, hoverEdge_ = kNone, hoverInstance_ = kNone;
};

class MoveTool : public TransformTool {
public:
    using TransformTool::TransformTool;
    ToolId id() const override { return ToolId::Move; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Distance"); }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

protected:
    Mat4 currentTransform() const override { return Mat4::translation(delta_); }

private:
    Vec3 base_, delta_;
    bool dragged_ = false;
    double pressX_ = 0, pressY_ = 0;
};

class RotateTool : public TransformTool {
public:
    using TransformTool::TransformTool;
    ToolId id() const override { return ToolId::Rotate; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Angle"); }
    void reset() override;
    bool keyPress(QKeyEvent* e) override;
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

protected:
    Mat4 currentTransform() const override;

private:
    Vec3 planeNormalAt(const MouseEvent& e, const InferenceResult& r);
    int stage_ = 0;
    int forcedAxis_ = -1;
    Vec3 center_, normal_ = Vec3(0, 0, 1), refDir_, cur_;
    double angle_ = 0;
    bool snapped_ = false;
};

// SketchUp-style scaling with grips on the selection's bounding box: corners
// scale uniformly (Shift: freely on all three axes), edge midpoints scale two
// axes and face centres one (Shift: uniformly). The opposite grip stays put;
// Ctrl scales about the centre. A single component instance gets a box along
// its own axes.
class ScaleTool : public TransformTool {
public:
    using TransformTool::TransformTool;
    ToolId id() const override { return ToolId::Scale; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Scale"); }
    void reset() override;
    bool keyPress(QKeyEvent* e) override;
    bool keyRelease(QKeyEvent* e) override;
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseRelease(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

protected:
    Mat4 currentTransform() const override;

private:
    using Grip = std::array<int, 3>; // per box axis: 0 = min side, 1 = middle, 2 = max side
    struct Box {
        Vec3 o;      // min corner
        Vec3 ax[3];  // orthonormal axes
        double len[3] = {0, 0, 0};
        bool valid = false;
        Vec3 at(const Grip& g) const { return o + ax[0] * (len[0] * g[0] / 2) + ax[1] * (len[1] * g[1] / 2) + ax[2] * (len[2] * g[2] / 2); }
    };
    Box selectionBox() const;
    std::vector<Grip> grips() const;
    int gripAt(double x, double y) const; // index into grips() or -1
    bool uniformMode() const;
    Vec3 anchor() const;
    void updateScale(const MouseEvent& e);
    void applyFactors(const Vec3& f);
    QString gripName(const Grip& g) const;

    Box box_;
    Grip grip_ = {2, 2, 2};
    int hover_ = -1;
    Vec3 factor_ = Vec3(1, 1, 1); // along the box axes
    Vec3 cur_;
    Qt::KeyboardModifiers mods_;
    bool dragged_ = false;
    double pressX_ = 0, pressY_ = 0;
    // The last scale, so a value typed afterwards can adjust it.
    bool canAdjust_ = false;
    Box lastBox_;
    Grip lastGrip_ = {2, 2, 2};
    bool lastCentered_ = false;
};

class OffsetTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Offset; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Distance"); }
    void reset() override;
    bool busy() const override { return active_; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    std::vector<std::vector<Vec3>> offsetLoops(Id f, double d) const;
    double signedDist(Id f, const Vec3& p) const;
    void commit(double d);
    bool active_ = false;
    Id face_ = kNone, hover_ = kNone;
    Vec3 start_;
    double dist_ = 0, lastDist_ = 0;
};

// Sweeps a profile face along a path of edges. The path is the selection when
// the tool starts (selected faces contribute their outlines); otherwise click
// the profile, then a path edge (its connected chain) or Alt-click a face to
// follow its outline.
class FollowMeTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::FollowMe; }
    QString status() const override;
    void activate() override;
    void reset() override;
    bool busy() const override { return profile_ != kNone; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    std::set<Id> pathUnder(const MouseEvent& e) const;
    void commit(const std::set<Id>& path);
    std::set<Id> selectedPath_, hoverPath_;
    Id profile_ = kNone, hoverFace_ = kNone;
    QString message_;
};

class TapeTool : public Tool {
public:
    using Tool::Tool;
    ToolId id() const override { return ToolId::Tape; }
    QString status() const override;
    QString vcbLabel() const override { return QStringLiteral("Length"); }
    void reset() override;
    bool busy() const override { return stage_ > 0; }
    void mousePress(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool vcbEnter(const QString& text) override;
    void buildPreview(Preview& pv) override;

private:
    int stage_ = 0;
    Vec3 start_, cur_;
    InferenceResult startInf_;
    double lastMeasured_ = 0;
};

} // namespace gs
