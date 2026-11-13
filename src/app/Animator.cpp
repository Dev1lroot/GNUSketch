// SPDX-License-Identifier: GPL-3.0-or-later
#include "Animator.h"

#include "Icons.h"
#include "Viewport.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

QString targetName(const ModelState& st, const TimelineLayer& l) {
    if (!layerTargetAlive(st, l)) return QStringLiteral("(gone)");
    if (l.kind == TimelineLayer::Object) return QString::fromStdString(st.objects[l.target].name);
    return QString::fromStdString(st.defs[st.instances[l.target].def].name);
}

bool targetHidden(const ModelState& st, const TimelineLayer& l) {
    if (!layerTargetAlive(st, l)) return false;
    return l.kind == TimelineLayer::Object ? st.objects[l.target].hidden : st.instances[l.target].hidden;
}

// Layer colours, like Flash's outline squares.
const Color kLayerColors[] = {Color(0.25f, 0.85f, 0.35f), Color(0.65f, 0.35f, 0.95f), Color(1.0f, 0.55f, 0.15f),
                              Color(0.25f, 0.6f, 1.0f),   Color(0.95f, 0.3f, 0.75f),  Color(0.2f, 0.85f, 0.85f),
                              Color(0.95f, 0.85f, 0.2f),  Color(0.9f, 0.3f, 0.3f)};

} // namespace

// ---------------------------------------------------------------------------
// Timeline view
// ---------------------------------------------------------------------------

TimelineView::TimelineView(AnimatorPanel* panel, QWidget* parent) : QWidget(parent), panel_(panel) {
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(kHeader + kRow * 3);
}

int TimelineView::firstFrameShown() const { return 1 + (hbar ? hbar->value() / kCell : 0); }

double TimelineView::frameX(int frame) const { return kLeft + (frame - 1) * kCell - (hbar ? hbar->value() : 0); }

int TimelineView::frameAt(double x) const {
    if (x < kLeft) return 0;
    return std::max(1, int(std::floor((x - kLeft + (hbar ? hbar->value() : 0)) / kCell)) + 1);
}

int TimelineView::layerAt(double y) const {
    if (y < kHeader) return -1;
    const int i = int((y - kHeader) / kRow) + (vbar ? vbar->value() : 0);
    return i < (int)panel_->doc().state.timeline.layers.size() ? i : -1;
}

void TimelineView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    panel_->refresh();
}

void TimelineView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QPalette& pal = palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const ModelState& st = panel_->doc().state;
    const Timeline& tl = st.timeline;
    const QColor base = pal.color(QPalette::Base), window = pal.color(QPalette::Window), text = pal.color(QPalette::Text);
    const QColor grid = dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 30);
    const QColor every5 = dark ? QColor(255, 255, 255, 10) : QColor(0, 0, 0, 14);
    const QColor holdFill = dark ? QColor(90, 92, 98) : QColor(200, 200, 200);
    const QColor tweenFill = dark ? QColor(56, 92, 140) : QColor(178, 215, 255);
    const QColor keyColor = dark ? QColor(235, 235, 235) : QColor(20, 20, 20);
    const QColor selRow = pal.color(QPalette::Highlight);
    p.fillRect(rect(), base);
    const int shown = panel_->framesShown();
    const int first = firstFrameShown();
    const int lastVisible = std::min(shown, first + (width() - kLeft) / kCell + 1);
    const int vOff = vbar ? vbar->value() : 0;

    // Rows.
    for (int i = vOff; i < (int)tl.layers.size(); ++i) {
        const int y = kHeader + (i - vOff) * kRow;
        if (y > height()) break;
        const TimelineLayer& l = tl.layers[i];
        const bool current = i == panel_->currentLayer();
        // Left column: name, eye, lock, colour.
        QRect left(0, y, kLeft, kRow);
        p.fillRect(left, current ? selRow : window);
        p.setPen(current ? pal.color(QPalette::HighlightedText) : text);
        const QPixmap layerIcon = Icons::get(l.kind == TimelineLayer::Object ? "light_point" : "component").pixmap(16, 16);
        p.drawPixmap(6, y + 3, layerIcon);
        p.drawText(QRect(26, y, kLeft - 26 - 66, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(QString::fromStdString(l.name), Qt::ElideRight, kLeft - 26 - 66));
        p.setOpacity(targetHidden(st, l) ? 0.35 : 1.0);
        p.drawPixmap(kLeft - 62, y + 3, Icons::get("eye").pixmap(16, 16));
        p.setOpacity(l.locked ? 1.0 : 0.35);
        p.drawPixmap(kLeft - 42, y + 3, Icons::get("lock").pixmap(16, 16));
        p.setOpacity(1.0);
        p.setPen(QColor::fromRgbF(l.color.r, l.color.g, l.color.b));
        p.setBrush(Qt::NoBrush);
        p.drawRect(kLeft - 19, y + 6, 10, 10);
        p.setPen(grid);
        p.drawLine(0, y + kRow - 1, width(), y + kRow - 1);

        // Frames.
        p.save();
        p.setClipRect(QRect(kLeft, y, width() - kLeft, kRow));
        if (current) p.fillRect(QRect(kLeft, y, width() - kLeft, kRow - 1), dark ? QColor(255, 255, 255, 8) : QColor(0, 60, 160, 14));
        for (int f = first; f <= lastVisible; ++f)
            if (f % 5 == 0) p.fillRect(QRectF(frameX(f), y, kCell, kRow - 1), every5);
        for (size_t k = 0; k < l.keys.size(); ++k) {
            const TimelineKey& a = l.keys[k];
            const int end = k + 1 < l.keys.size() ? l.keys[k + 1].frame - 1 : a.frame;
            const QRectF span(frameX(a.frame), y + 1, (end - a.frame + 1) * kCell, kRow - 3);
            const bool tween = a.tween && k + 1 < l.keys.size();
            p.fillRect(span, tween ? tweenFill : holdFill);
            p.setPen(grid);
            p.drawRect(span);
            if (tween && end > a.frame) {
                // The arrow of a motion tween.
                const double ay = y + kRow / 2.0;
                p.setPen(QPen(keyColor, 1));
                p.drawLine(QPointF(frameX(a.frame) + kCell, ay), QPointF(frameX(end + 1) - 3, ay));
                p.drawLine(QPointF(frameX(end + 1) - 3, ay), QPointF(frameX(end + 1) - 7, ay - 3));
                p.drawLine(QPointF(frameX(end + 1) - 3, ay), QPointF(frameX(end + 1) - 7, ay + 3));
            } else if (end > a.frame) {
                // Held: a small hollow box on the span's last frame.
                p.setPen(QPen(keyColor, 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(QRectF(frameX(end) + 2, y + kRow / 2.0 - 1, kCell - 5, 6));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(keyColor);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.drawEllipse(QPointF(frameX(a.frame) + kCell / 2.0, y + kRow / 2.0 + 3), 3, 3);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setBrush(Qt::NoBrush);
        }
        p.restore();
    }

    // Header: column icons and the ruler.
    p.fillRect(QRect(0, 0, width(), kHeader), window);
    p.drawPixmap(kLeft - 62, 3, Icons::get("eye").pixmap(16, 16));
    p.drawPixmap(kLeft - 42, 3, Icons::get("lock").pixmap(16, 16));
    p.setPen(text);
    p.drawRect(kLeft - 19, 6, 10, 10);
    p.save();
    p.setClipRect(QRect(kLeft, 0, width() - kLeft, height()));
    for (int f = first; f <= lastVisible; ++f) {
        const double x = frameX(f);
        p.setPen(grid);
        p.drawLine(QPointF(x, kHeader - (f % 5 == 0 ? 8 : 4)), QPointF(x, kHeader - 1));
        if (f == 1 || f % 5 == 0) {
            p.setPen(text);
            p.drawText(QRectF(x + 1, 1, 40, kHeader - 6), Qt::AlignLeft | Qt::AlignVCenter, QString::number(f));
        }
    }
    // Past the end of the timeline.
    const double endX = frameX(shown + 1);
    if (endX < width()) p.fillRect(QRectF(endX, kHeader, width() - endX, height()), window);
    // Playhead.
    const double px = frameX(panel_->frame());
    const QColor red(230, 60, 60);
    p.fillRect(QRectF(px + 1, 1, kCell - 1, kHeader - 3), QColor(255, 120, 120));
    p.setPen(red);
    p.drawRect(QRectF(px + 1, 1, kCell - 2, kHeader - 3));
    p.drawLine(QPointF(px + kCell / 2.0, kHeader), QPointF(px + kCell / 2.0, height()));
    p.restore();
    p.setPen(grid);
    p.drawLine(0, kHeader - 1, width(), kHeader - 1);
    p.drawLine(kLeft - 1, 0, kLeft - 1, height());
    if (tl.layers.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect().adjusted(kLeft + 10, kHeader, -10, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Select a component, light or camera and press New Layer (+) to animate it."));
    }
}

void TimelineView::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (e->button() != Qt::LeftButton) return;
    if (pos.y() < kHeader) {
        if (pos.x() >= kLeft) {
            drag_ = Drag::Scrub;
            panel_->setFrame(frameAt(pos.x()));
        }
        return;
    }
    const int layer = layerAt(pos.y());
    if (layer < 0) return;
    panel_->setCurrentLayer(layer);
    if (pos.x() < kLeft) {
        if (pos.x() >= kLeft - 64 && pos.x() < kLeft - 44) panel_->toggleVisible(layer);
        else if (pos.x() >= kLeft - 44 && pos.x() < kLeft - 24) panel_->toggleLocked(layer);
        return;
    }
    const int f = frameAt(pos.x());
    const TimelineLayer& l = panel_->doc().state.timeline.layers[layer];
    if (l.keyAt(f) >= 0 && !l.locked) {
        drag_ = Drag::Key;
        dragLayer_ = layer;
        dragFrom_ = dragTo_ = f;
    }
    panel_->setFrame(f);
}

void TimelineView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    if (drag_ == Drag::Scrub) panel_->setFrame(std::max(1, frameAt(std::max<double>(pos.x(), kLeft))));
    else if (drag_ == Drag::Key) {
        dragTo_ = std::max(1, frameAt(std::max<double>(pos.x(), kLeft)));
        setCursor(dragTo_ != dragFrom_ ? Qt::ClosedHandCursor : Qt::ArrowCursor);
    }
}

void TimelineView::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ == Drag::Key && dragTo_ != dragFrom_) {
        if (panel_->moveKey(dragLayer_, dragFrom_, dragTo_)) panel_->setFrame(dragTo_);
    }
    unsetCursor();
    drag_ = Drag::None;
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const int layer = layerAt(pos.y());
    if (layer < 0) return;
    if (pos.x() < kLeft - 64) {
        panel_->renameLayer(layer);
        return;
    }
    if (pos.x() >= kLeft) {
        panel_->setCurrentLayer(layer);
        panel_->setFrame(frameAt(pos.x()));
        panel_->insertKeyframe();
    }
}

void TimelineView::contextMenuEvent(QContextMenuEvent* e) {
    const int layer = layerAt(e->pos().y());
    if (layer < 0) return;
    panel_->setCurrentLayer(layer);
    if (e->pos().x() >= kLeft) panel_->setFrame(frameAt(e->pos().x()));
    const TimelineLayer& l = panel_->doc().state.timeline.layers[layer];
    QMenu menu(this);
    menu.addAction(QStringLiteral("Insert Keyframe (F6)"), panel_, &AnimatorPanel::insertKeyframe)->setEnabled(!l.locked);
    menu.addAction(QStringLiteral("Clear Keyframe (Shift+F6)"), panel_, &AnimatorPanel::removeKeyframe)
        ->setEnabled(!l.locked && l.keyAt(panel_->frame()) >= 0);
    const int k = l.keyBefore(panel_->frame());
    const bool tween = k >= 0 && l.keys[k].tween;
    menu.addAction(tween ? QStringLiteral("Remove Motion Tween") : QStringLiteral("Create Motion Tween"), panel_, &AnimatorPanel::toggleTween)
        ->setEnabled(!l.locked && k >= 0);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Rename Layer…"), panel_, [this, layer] { panel_->renameLayer(layer); });
    menu.addAction(QStringLiteral("Delete Layer"), panel_, &AnimatorPanel::deleteLayer);
    menu.exec(e->globalPos());
}

void TimelineView::wheelEvent(QWheelEvent* e) {
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if ((e->modifiers() & Qt::ShiftModifier) || e->angleDelta().x() != 0) {
        if (hbar) hbar->setValue(hbar->value() - d / 4);
    } else if (vbar) {
        vbar->setValue(vbar->value() - d / 120);
    }
    e->accept();
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

AnimatorPanel::AnimatorPanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* grid = new QGridLayout();
    grid->setSpacing(0);
    view_ = new TimelineView(this, this);
    view_->hbar = new QScrollBar(Qt::Horizontal, this);
    view_->vbar = new QScrollBar(Qt::Vertical, this);
    grid->addWidget(view_, 0, 0);
    grid->addWidget(view_->vbar, 0, 1);
    grid->addWidget(view_->hbar, 1, 0);
    lay->addLayout(grid, 1);
    connect(view_->hbar, &QScrollBar::valueChanged, view_, qOverload<>(&QWidget::update));
    connect(view_->vbar, &QScrollBar::valueChanged, view_, qOverload<>(&QWidget::update));

    auto* bar = new QHBoxLayout();
    bar->setSpacing(2);
    auto button = [&](const char* icon, const QString& tip, auto fn, bool checkable = false) {
        auto* b = new QToolButton(this);
        b->setIcon(Icons::get(QString::fromLatin1(icon)));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        b->setObjectName(QString::fromLatin1(icon));
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    button("layer_add", QStringLiteral("New Layer: animate the selected components, lights and cameras"), [this] { addLayersFromSelection(); });
    button("layer_delete", QStringLiteral("Delete Layer"), [this] { deleteLayer(); });
    bar->addSpacing(10);
    button("anim_first", QStringLiteral("First frame"), [this] { stop(); setFrame(1); });
    button("anim_prevkey", QStringLiteral("Previous keyframe"), [this] {
        const TimelineLayer* l = layerPtr();
        int best = 1;
        for (const TimelineLayer& x : doc_->state.timeline.layers)
            if (!l || &x == l)
                for (const TimelineKey& k : x.keys)
                    if (k.frame < frame_) best = std::max(best, k.frame);
        setFrame(best);
    });
    play_ = button("anim_play", QStringLiteral("Play / Pause (Enter)"), [this] { playing_ ? stop() : play(); }, true);
    button("anim_nextkey", QStringLiteral("Next keyframe"), [this] {
        const TimelineLayer* l = layerPtr();
        int best = 0;
        for (const TimelineLayer& x : doc_->state.timeline.layers)
            if (!l || &x == l)
                for (const TimelineKey& k : x.keys)
                    if (k.frame > frame_ && (best == 0 || k.frame < best)) best = k.frame;
        if (best) setFrame(best);
    });
    loopBtn_ = button("anim_loop", QStringLiteral("Loop playback"), [this] { loop_ = loopBtn_->isChecked(); }, true);
    loopBtn_->setChecked(true);
    bar->addSpacing(10);
    button("anim_key", QStringLiteral("Insert Keyframe (F6): remember where the layer's thing is now"), [this] { insertKeyframe(); });
    button("anim_unkey", QStringLiteral("Clear Keyframe (Shift+F6)"), [this] { removeKeyframe(); });
    button("anim_tween", QStringLiteral("Create / Remove Motion Tween from the keyframe before the playhead"), [this] { toggleTween(); });
    autoKeyBtn_ = button("anim_autokey", QStringLiteral("Auto Key: moving or changing an animated thing makes a keyframe at the playhead"),
                         [this] { autoKey_ = autoKeyBtn_->isChecked(); }, true);
    autoKeyBtn_->setChecked(true);
    bar->addStretch(1);
    frameSpin_ = new QSpinBox(this);
    frameSpin_->setRange(1, 100000);
    frameSpin_->setToolTip(QStringLiteral("Current frame"));
    fps_ = new QDoubleSpinBox(this);
    fps_->setRange(1, 240);
    fps_->setDecimals(1);
    fps_->setSuffix(QStringLiteral(" fps"));
    fps_->setToolTip(QStringLiteral("Frame rate"));
    length_ = new QSpinBox(this);
    length_->setRange(1, 100000);
    length_->setPrefix(QStringLiteral("of "));
    length_->setToolTip(QStringLiteral("Length of the timeline in frames"));
    elapsed_ = new QLabel(this);
    elapsed_->setMinimumWidth(56);
    elapsed_->setToolTip(QStringLiteral("Elapsed time"));
    bar->addWidget(frameSpin_);
    bar->addWidget(length_);
    bar->addWidget(fps_);
    bar->addWidget(elapsed_);
    lay->addLayout(bar);
    connect(frameSpin_, &QSpinBox::valueChanged, this, [this](int v) { if (!updating_) setFrame(v); });
    connect(fps_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (updating_ || v == doc_->state.timeline.fps) return;
        doc_->beginOp("Frame Rate");
        doc_->state.timeline.fps = v;
        commitTimelineOp();
    });
    connect(length_, &QSpinBox::valueChanged, this, [this](int v) {
        if (updating_ || v == doc_->state.timeline.length) return;
        doc_->beginOp("Timeline Length");
        doc_->state.timeline.length = v;
        commitTimelineOp();
        refresh();
    });

    timer_ = new QTimer(this);
    timer_->setInterval(15);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });

    // Shortcuts while the panel is in the window.
    auto shortcut = [&](const QKeySequence& k, auto fn) {
        auto* a = new QAction(this);
        a->setShortcut(k);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    shortcut(QKeySequence(Qt::Key_F6), [this] { insertKeyframe(); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6), [this] { removeKeyframe(); });
    refresh();
}

void AnimatorPanel::commitTimelineOp() {
    // An edit of the timeline itself: pose the model as it now says before the
    // change is announced, so Auto Key does not take it for a move.
    applying_ = true;
    applyTimeline(doc_->state, frame_);
    doc_->commitOp();
    applying_ = false;
    vp_->update();
}

TimelineLayer* AnimatorPanel::layerPtr() {
    auto& ls = doc_->state.timeline.layers;
    return layer_ >= 0 && layer_ < (int)ls.size() ? &ls[layer_] : nullptr;
}

int AnimatorPanel::framesShown() const {
    const Timeline& tl = doc_->state.timeline;
    return std::max({tl.length, tl.lastKeyFrame() + 10, frame_});
}

void AnimatorPanel::setCurrentLayer(int i) {
    layer_ = i;
    // Selecting the layer selects its thing, as clicking it would.
    if (const TimelineLayer* l = layerPtr(); l && layerTargetAlive(doc_->state, *l) && doc_->state.editing == kNone) {
        doc_->selection.clear();
        if (l->kind == TimelineLayer::Instance) doc_->selection.instances.insert(l->target);
        else doc_->selection.objects.insert(l->target);
        doc_->notifySelection();
    }
    view_->update();
}

void AnimatorPanel::setFrame(int f) {
    f = std::max(1, f);
    const bool moved = f != frame_;
    frame_ = f;
    applying_ = true;
    if (applyTimeline(doc_->state, f)) doc_->notifyChanged();
    applying_ = false;
    vp_->update();
    syncControls();
    view_->update();
    if (moved) emit frameChanged(f);
}

void AnimatorPanel::reapply() {
    applying_ = true;
    if (applyTimeline(doc_->state, frame_)) doc_->notifyChanged();
    applying_ = false;
    layer_ = std::min<int>(layer_, (int)doc_->state.timeline.layers.size() - 1);
    refresh();
}

void AnimatorPanel::modelReplaced() {
    if (playing_) {
        playing_ = false;
        timer_->stop();
    }
    frame_ = 1;
    layer_ = doc_->state.timeline.layers.empty() ? -1 : 0;
    reapply();
}

void AnimatorPanel::play() {
    if (playing_) return;
    if (frame_ >= std::max(1, doc_->state.timeline.lastKeyFrame())) frame_ = 1;
    playing_ = true;
    playFrom_ = frame_;
    clock_.start();
    timer_->start();
    syncControls();
}

void AnimatorPanel::stop() {
    if (!playing_) return;
    playing_ = false;
    timer_->stop();
    setFrame(frame_);
    syncControls();
}

void AnimatorPanel::tick() {
    const Timeline& tl = doc_->state.timeline;
    const int end = std::max(1, tl.lastKeyFrame() > 0 ? tl.lastKeyFrame() : tl.length);
    double f = playFrom_ + clock_.nsecsElapsed() * 1e-9 * tl.fps;
    if (f > end) {
        if (loop_ && end > 1) {
            f = 1 + std::fmod(f - 1, double(end - 1));
            playFrom_ = f;
            clock_.restart();
        } else {
            f = end;
        }
    }
    frame_ = (int)std::floor(f);
    applying_ = true;
    applyTimeline(doc_->state, f); // smooth: between whole frames too
    applying_ = false;
    vp_->update();
    syncControls();
    view_->update();
    emit frameChanged(frame_);
    if (!loop_ && f >= end) stop();
}

void AnimatorPanel::insertKeyframe() {
    TimelineLayer* l = layerPtr();
    if (!l || l->locked || !layerTargetAlive(doc_->state, *l)) return;
    const int before = l->keyBefore(frame_);
    const bool tween = before >= 0 ? l->keys[before].tween : true;
    doc_->beginOp("Insert Keyframe");
    setKey(*layerPtr(), captureKey(doc_->state, *layerPtr(), frame_, tween));
    commitTimelineOp();
    refresh();
}

void AnimatorPanel::removeKeyframe() {
    TimelineLayer* l = layerPtr();
    if (!l || l->locked) return;
    const int k = l->keyAt(frame_);
    if (k < 0) return;
    doc_->beginOp("Clear Keyframe");
    layerPtr()->keys.erase(layerPtr()->keys.begin() + k);
    commitTimelineOp();
    setFrame(frame_);
    refresh();
}

void AnimatorPanel::toggleTween() {
    TimelineLayer* l = layerPtr();
    if (!l || l->locked) return;
    const int k = l->keyBefore(frame_);
    if (k < 0) return;
    doc_->beginOp(l->keys[k].tween ? "Remove Motion Tween" : "Create Motion Tween");
    layerPtr()->keys[k].tween = !layerPtr()->keys[k].tween;
    commitTimelineOp();
    setFrame(frame_);
    refresh();
}

bool AnimatorPanel::moveKey(int layer, int from, int to) {
    auto& ls = doc_->state.timeline.layers;
    if (layer < 0 || layer >= (int)ls.size() || ls[layer].locked) return false;
    const int k = ls[layer].keyAt(from);
    if (k < 0 || ls[layer].keyAt(to) >= 0) return false;
    doc_->beginOp("Move Keyframe");
    TimelineKey key = ls[layer].keys[k];
    ls[layer].keys.erase(ls[layer].keys.begin() + k);
    key.frame = to;
    setKey(ls[layer], key);
    commitTimelineOp();
    refresh();
    return true;
}

void AnimatorPanel::addLayersFromSelection() {
    ModelState& st = doc_->state;
    if (st.editing != kNone) return;
    auto has = [&](TimelineLayer::Kind kind, Id id) {
        for (const TimelineLayer& l : st.timeline.layers)
            if (l.kind == kind && l.target == id) return true;
        return false;
    };
    std::vector<TimelineLayer> fresh;
    auto add = [&](TimelineLayer::Kind kind, Id id) {
        if (has(kind, id)) return;
        TimelineLayer l;
        l.kind = kind;
        l.target = id;
        l.name = targetName(st, l).toStdString();
        l.color = kLayerColors[(st.timeline.layers.size() + fresh.size()) % (sizeof(kLayerColors) / sizeof(kLayerColors[0]))];
        // The first keyframe holds where it is now.
        setKey(l, captureKey(st, l, frame_));
        fresh.push_back(std::move(l));
    };
    for (Id i : doc_->selection.instances) add(TimelineLayer::Instance, i);
    for (Id o : doc_->selection.objects) add(TimelineLayer::Object, o);
    if (fresh.empty()) return;
    doc_->beginOp("New Layer");
    for (TimelineLayer& l : fresh) st.timeline.layers.push_back(std::move(l));
    commitTimelineOp();
    layer_ = (int)st.timeline.layers.size() - 1;
    refresh();
}

void AnimatorPanel::deleteLayer() {
    if (!layerPtr()) return;
    doc_->beginOp("Delete Layer");
    doc_->state.timeline.layers.erase(doc_->state.timeline.layers.begin() + layer_);
    commitTimelineOp();
    layer_ = std::min<int>(layer_, (int)doc_->state.timeline.layers.size() - 1);
    refresh();
}

void AnimatorPanel::renameLayer(int i) {
    auto& ls = doc_->state.timeline.layers;
    if (i < 0 || i >= (int)ls.size()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Layer"), QStringLiteral("Name:"), QLineEdit::Normal,
                                               QString::fromStdString(ls[i].name), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    doc_->beginOp("Rename Layer");
    doc_->state.timeline.layers[i].name = name.trimmed().toStdString();
    commitTimelineOp();
    refresh();
}

void AnimatorPanel::toggleVisible(int i) {
    auto& ls = doc_->state.timeline.layers;
    if (i < 0 || i >= (int)ls.size() || !layerTargetAlive(doc_->state, ls[i])) return;
    doc_->beginOp("Show / Hide");
    const TimelineLayer& l = doc_->state.timeline.layers[i];
    if (l.kind == TimelineLayer::Instance) doc_->state.instances[l.target].hidden = !doc_->state.instances[l.target].hidden;
    else doc_->state.objects[l.target].hidden = !doc_->state.objects[l.target].hidden;
    commitTimelineOp();
    refresh();
}

void AnimatorPanel::toggleLocked(int i) {
    auto& ls = doc_->state.timeline.layers;
    if (i < 0 || i >= (int)ls.size()) return;
    doc_->beginOp(ls[i].locked ? "Unlock Layer" : "Lock Layer");
    doc_->state.timeline.layers[i].locked = !doc_->state.timeline.layers[i].locked;
    commitTimelineOp();
    refresh();
}

void AnimatorPanel::setAutoKey(bool on) {
    autoKey_ = on;
    autoKeyBtn_->setChecked(on);
}

void AnimatorPanel::documentChanged() {
    if (applying_ || suspended_ || playing_ || !autoKey_ || doc_->inOp() || doc_->state.editing != kNone) {
        if (!applying_) refresh();
        return;
    }
    // Something animated was moved or changed: remember it at the playhead.
    ModelState& st = doc_->state;
    bool changed = false;
    for (TimelineLayer& l : st.timeline.layers) {
        if (l.locked || l.keys.empty() || !layerTargetAlive(st, l)) continue;
        TimelineKey pose;
        if (!evaluateLayer(l, frame_, pose) || !keyDiffers(st, l, pose)) continue;
        // Part of the edit just made: its undo takes the keyframe away too.
        const int before = l.keyBefore(frame_);
        setKey(l, captureKey(st, l, frame_, before >= 0 ? l.keys[before].tween : true));
        changed = true;
    }
    if (changed) doc_->modified = true;
    refresh();
}

void AnimatorPanel::syncControls() {
    updating_ = true;
    const Timeline& tl = doc_->state.timeline;
    frameSpin_->setValue(frame_);
    fps_->setValue(tl.fps);
    length_->setValue(tl.length);
    elapsed_->setText(QStringLiteral("%1s").arg((frame_ - 1) / std::max(1.0, tl.fps), 0, 'f', 1));
    play_->setChecked(playing_);
    play_->setIcon(Icons::get(playing_ ? "anim_pause" : "anim_play"));
    updating_ = false;
}

void AnimatorPanel::refresh() {
    const Timeline& tl = doc_->state.timeline;
    if (layer_ >= (int)tl.layers.size()) layer_ = (int)tl.layers.size() - 1;
    if (layer_ < 0 && !tl.layers.empty()) layer_ = 0;
    const int contentW = framesShown() * TimelineView::kCell + 40;
    const int visibleW = std::max(0, view_->width() - TimelineView::kLeft);
    view_->hbar->setRange(0, std::max(0, contentW - visibleW));
    view_->hbar->setPageStep(std::max(1, visibleW));
    view_->hbar->setSingleStep(TimelineView::kCell);
    const int rowsVisible = std::max(1, (view_->height() - TimelineView::kHeader) / TimelineView::kRow);
    view_->vbar->setRange(0, std::max(0, (int)tl.layers.size() - rowsVisible));
    view_->vbar->setPageStep(rowsVisible);
    syncControls();
    view_->update();
}

} // namespace gs
