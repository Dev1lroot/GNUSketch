// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the Animator panel: a timeline like Flash's. A layer per moving
// thing (component placement, light, camera) with its name, a visibility eye,
// a lock and a colour; frames to the right with keyframes, motion tweens
// (blue, with an arrow) and held frames (grey); a ruler with the red playhead.
// Below: play, keyframe and tween buttons, the current frame, frame rate and
// elapsed time, new / delete layer. Data: core/Timeline.h (saved with the model).
#pragma once

#include "core/Document.h"

#include <QElapsedTimer>
#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QScrollBar;
class QSpinBox;
class QTimer;
class QToolButton;

namespace gs {

class Viewport;
class AnimatorPanel;

class TimelineView : public QWidget {
    Q_OBJECT
public:
    TimelineView(AnimatorPanel* panel, QWidget* parent);
    static constexpr int kLeft = 230, kHeader = 22, kRow = 22, kCell = 10;
    int firstFrameShown() const;
    // Frame under an x position in the frames area (1-based), or 0.
    int frameAt(double x) const;
    double frameX(int frame) const; // left edge of a frame's cell
    int layerAt(double y) const;    // -1 if none
    QScrollBar* hbar = nullptr;
    QScrollBar* vbar = nullptr;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    AnimatorPanel* panel_;
    enum class Drag { None, Scrub, Key } drag_ = Drag::None;
    int dragLayer_ = -1, dragFrom_ = 0, dragTo_ = 0;
};

class AnimatorPanel : public QWidget {
    Q_OBJECT
public:
    AnimatorPanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);

    int frame() const { return frame_; }
    // Moves the playhead and poses the model there.
    void setFrame(int f);
    int currentLayer() const { return layer_; }
    void setCurrentLayer(int i);
    bool playing() const { return playing_; }
    void play();
    void stop();
    // On the current layer at the current frame.
    void insertKeyframe();
    void removeKeyframe();
    void toggleTween();
    // A layer for each selected component placement, light or camera that has none.
    void addLayersFromSelection();
    void deleteLayer();
    void renameLayer(int i);
    void toggleVisible(int i);
    void toggleLocked(int i);
    bool moveKey(int layer, int from, int to);
    // Frames the timeline shows (its length or past the last keyframe).
    int framesShown() const;
    bool autoKey() const { return autoKey_; }
    void setAutoKey(bool on);

    // After the document changed: with Auto Key, a layer whose thing was moved
    // away from its pose at the current frame gets a keyframe there.
    void documentChanged();
    // Undo, redo, opening a file: the model is posed as the timeline says.
    void reapply();
    // A different model (new, opened): back to frame 1.
    void modelReplaced();
    // Undo/redo: the model jumps to another moment, which is no edit to key.
    void setSuspended(bool on) { suspended_ = on; }
    void refresh();
    Document& doc() { return *doc_; }

signals:
    void frameChanged(int frame);

private:
    void tick();
    void commitTimelineOp();
    void syncControls();
    TimelineLayer* layerPtr();

    Document* doc_;
    Viewport* vp_;
    TimelineView* view_;
    QToolButton *play_, *autoKeyBtn_, *loopBtn_;
    QSpinBox* frameSpin_;
    QDoubleSpinBox* fps_;
    QSpinBox* length_;
    QLabel* elapsed_;
    QTimer* timer_;
    QElapsedTimer clock_;
    int frame_ = 1, layer_ = -1;
    double playFrom_ = 1;
    bool playing_ = false, autoKey_ = true, loop_ = true, applying_ = false, updating_ = false, suspended_ = false;
};

} // namespace gs
