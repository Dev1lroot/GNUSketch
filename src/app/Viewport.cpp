// SPDX-License-Identifier: GPL-3.0-or-later
#include "Viewport.h"

#include "BendTool.h"
#include "CameraTools.h"
#include "DrawTools.h"
#include "GameMode.h"
#include "ModifyTools.h"
#include "SceneTools.h"
#include "SolidTools.h"
#include "TerrainTools.h"
#include "TextureTool.h"
#include "core/Components.h"
#include "io/AssetLibrary.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <limits>

namespace gs {

Viewport::Viewport(Document* doc, QWidget* parent) : QOpenGLWidget(parent), doc_(doc) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(200, 150);
    QSurfaceFormat fmt = format();
    fmt.setSamples(4); // multisampled FBO; does not require an MSAA window visual
    setFormat(fmt);
    inference_ = std::make_unique<InferenceEngine>(*doc_, camera_);
    setTool(ToolId::Select);
    animTimer_ = new QTimer(this);
    animTimer_->setInterval(33);
    connect(animTimer_, &QTimer::timeout, this, [this] {
        if (renderer_.animated()) update();
    });
    animTimer_->start();
}

Viewport::~Viewport() {
    makeCurrent();
    renderer_.cleanup();
    doneCurrent();
}

void Viewport::setTool(ToolId id) {
    if (tool_) tool_->deactivate();
    switch (id) {
    case ToolId::Select: tool_ = std::make_unique<SelectTool>(this); break;
    case ToolId::Eraser: tool_ = std::make_unique<EraserTool>(this); break;
    case ToolId::Paint: tool_ = std::make_unique<PaintTool>(this); break;
    case ToolId::Line: tool_ = std::make_unique<LineTool>(this); break;
    case ToolId::Freehand: tool_ = std::make_unique<FreehandTool>(this); break;
    case ToolId::Rectangle: tool_ = std::make_unique<RectangleTool>(this); break;
    case ToolId::Circle: tool_ = std::make_unique<CircleTool>(this, false); break;
    case ToolId::Polygon: tool_ = std::make_unique<CircleTool>(this, true); break;
    case ToolId::Arc: tool_ = std::make_unique<ArcTool>(this); break;
    case ToolId::PushPull: tool_ = std::make_unique<PushPullTool>(this); break;
    case ToolId::Move: tool_ = std::make_unique<MoveTool>(this); break;
    case ToolId::Rotate: tool_ = std::make_unique<RotateTool>(this); break;
    case ToolId::Scale: tool_ = std::make_unique<ScaleTool>(this); break;
    case ToolId::Offset: tool_ = std::make_unique<OffsetTool>(this); break;
    case ToolId::FollowMe: tool_ = std::make_unique<FollowMeTool>(this); break;
    case ToolId::Tape: tool_ = std::make_unique<TapeTool>(this); break;
    case ToolId::Orbit: tool_ = std::make_unique<OrbitTool>(this); break;
    case ToolId::Pan: tool_ = std::make_unique<PanTool>(this); break;
    case ToolId::Zoom: tool_ = std::make_unique<ZoomTool>(this); break;
    case ToolId::ZoomWindow: tool_ = std::make_unique<ZoomWindowTool>(this); break;
    case ToolId::Mountain:
    case ToolId::Pit:
    case ToolId::Flatten:
    case ToolId::TextureBrush: tool_ = std::make_unique<TerrainTool>(this, id); break;
    case ToolId::SolidUnion:
    case ToolId::SolidIntersect:
    case ToolId::SolidSubtract:
    case ToolId::SolidTrim:
    case ToolId::SolidSplit: tool_ = std::make_unique<SolidTool>(this, id); break;
    case ToolId::TexturePosition: tool_ = std::make_unique<TextureTool>(this); break;
    case ToolId::Bend: tool_ = std::make_unique<BendTool>(this); break;
    case ToolId::PlaceCharacter: tool_ = std::make_unique<PlaceCharacterTool>(this); break;
    case ToolId::PlacePointLight:
    case ToolId::PlaceSpotLight:
    case ToolId::PlaceDirectionalLight:
    case ToolId::PlaceCamera: tool_ = std::make_unique<PlaceObjectTool>(this, id); break;
    }
    inference_->resetMemory();
    vcbLabel_ = tool_->vcbLabel();
    emit vcbChanged(vcbLabel_, QString());
    tool_->activate();
    setCursor(tool_->cursor());
    refreshToolStatus();
    emit toolChanged(id);
    update();
}

void Viewport::refreshToolStatus() {
    if (!tool_) return;
    emit statusText(tool_->status());
    QString label = tool_->vcbLabel();
    if (label != vcbLabel_) {
        vcbLabel_ = label;
        emit vcbChanged(vcbLabel_, QString());
    }
}

void Viewport::setVcbValue(const QString& v) { emit vcbChanged(tool_ ? tool_->vcbLabel() : QString(), v); }

void Viewport::vcbEnter(const QString& text) {
    if (!tool_ || text.trimmed().isEmpty()) return;
    doc_->mesh().viewHint = camera_.eye;
    doc_->mesh().groundFacesDown = false;
    if (!tool_->vcbEnter(text.trimmed())) {
        QApplication::beep();
        emit statusText(QStringLiteral("Invalid value: %1").arg(text));
    }
    update();
}

void Viewport::cameraChanged() { update(); }

void Viewport::cancelTool() {
    if (tool_) tool_->reset();
    update();
}

void Viewport::updateSceneBounds() {
    Vec3 mn, mx;
    bool any;
    modelBounds(doc_->state, mn, mx, any);
    if (any) camera_.setSceneRadius((mn + mx) * 0.5, (mx - mn).length() * 0.5);
    else camera_.setSceneRadius(Vec3(), 10);
}

void Viewport::zoomExtents() {
    Vec3 mn, mx;
    bool any;
    modelBounds(doc_->state, mn, mx, any);
    for (const Guide& g : doc_->state.guides)
        if (g.kind == Guide::Point) {
            for (int i = 0; i < 3; ++i) {
                mn[i] = std::min(mn[i], g.p[i]);
                mx[i] = std::max(mx[i], g.p[i]);
            }
            any = true;
        }
    if (!any) {
        camera_ = Camera();
        camera_.setViewport(width(), height());
    } else {
        camera_.zoomExtents(mn, mx);
    }
    update();
}

void Viewport::setStandardView(const Vec3& dir) {
    camera_.setStandardView(dir, Vec3(0, 0, 1));
    zoomExtents();
}

void Viewport::setPerspective(bool on) {
    camera_.perspective = on;
    update();
}

// ---------------------------------------------------------------------------
// GL
// ---------------------------------------------------------------------------

void Viewport::initializeGL() {
    QString log;
    glOk_ = renderer_.initialize(&log);
    if (!glOk_) {
        glError_ = log;
        QTimer::singleShot(0, this, [this, log] { emit glFailed(log); });
    }
}

void Viewport::resizeGL(int, int) { camera_.setViewport(width(), height()); }

void Viewport::paintGL() {
    camera_.setViewport(width(), height());
    doc_->mesh().viewHint = camera_.eye;
    doc_->mesh().groundFacesDown = false;
    updateSceneBounds();
    Preview pv;
    if (tool_ && mouseInside_) tool_->buildPreview(pv);
    else if (tool_) {
        Preview tmp;
        tool_->buildPreview(tmp);
        // Keep geometry previews while the cursor is outside, drop cursor markers.
        pv.lines = tmp.lines;
        pv.polys = tmp.polys;
        pv.highlightFaces = tmp.highlightFaces;
        pv.highlightEdges = tmp.highlightEdges;
        pv.hideFaceSelection = tmp.hideFaceSelection;
    }
    if (dropHoverFace_ >= 0) pv.highlightFaces.insert(dropHoverFace_);
    if (dragPrefab_ && dragPrefabHit_ && !dragPrefab_->defs.empty()) {
        // The box the prefab would take up, standing where it would land.
        Vec3 mn, mx;
        bool any = false;
        definitionBounds(*dragPrefab_, 0, mn, mx, any);
        if (any) {
            const Vec3 off = dragPrefabAt_ - Vec3((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z);
            Vec3 k[8];
            for (int i = 0; i < 8; ++i) k[i] = off + Vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
            static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& ed : E) pv.line(k[ed[0]], k[ed[1]], Color::rgb8(0, 110, 230), 1.5f);
        }
    }
    if (glOk_) renderer_.render(*doc_, camera_, pv, devicePixelRatioF());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!glOk_) {
        p.fillRect(rect(), QColor(230, 230, 230));
        p.setPen(Qt::black);
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("OpenGL 3.3 core profile is required but could not be initialised.\n\n") + glError_);
        return;
    }
    paintOverlay(p, pv);
    if (tool_) tool_->paintOverlay(p);
}

void Viewport::paintOverlay(QPainter& p, const Preview& pv) {
    using MK = Preview::MarkerKind;
    for (const auto& m : pv.markers) {
        double x, y;
        if (!camera_.project(m.p, x, y)) continue;
        QPointF c(x, y);
        p.setPen(QPen(QColor(0, 0, 0, 200), 1));
        switch (m.kind) {
        case MK::Endpoint:
            p.setBrush(QColor(0, 200, 0));
            p.drawEllipse(c, 5, 5);
            break;
        case MK::Midpoint:
            p.setBrush(QColor(0, 210, 210));
            p.drawEllipse(c, 5, 5);
            break;
        case MK::OnEdge:
            p.setBrush(QColor(230, 0, 0));
            p.drawRect(QRectF(x - 4, y - 4, 8, 8));
            break;
        case MK::OnFace: {
            p.setBrush(QColor(40, 40, 255));
            QPolygonF d;
            d << QPointF(x, y - 6) << QPointF(x + 6, y) << QPointF(x, y + 6) << QPointF(x - 6, y);
            p.drawPolygon(d);
            break;
        }
        case MK::Origin:
            p.setBrush(QColor(255, 220, 0));
            p.drawEllipse(c, 5, 5);
            break;
        case MK::Guide:
        case MK::Intersection:
            p.setPen(QPen(Qt::black, 1.5));
            p.drawLine(QPointF(x - 5, y - 5), QPointF(x + 5, y + 5));
            p.drawLine(QPointF(x - 5, y + 5), QPointF(x + 5, y - 5));
            break;
        case MK::Grip:
        case MK::GripHot:
            p.setBrush(m.kind == MK::GripHot ? QColor(230, 30, 30) : QColor(0, 170, 0));
            p.drawRect(QRectF(x - 3.5, y - 3.5, 7, 7));
            break;
        case MK::Center:
        case MK::Plain:
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 4, 4);
            break;
        }
    }
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.95);
    p.setFont(f);
    QFontMetrics fm(f);
    for (const auto& l : pv.labels) {
        double x, y;
        if (!camera_.project(l.p, x, y)) continue;
        QString text = QString::fromStdString(l.text);
        QRectF r = fm.boundingRect(text);
        r.adjust(-4, -2, 4, 2);
        r.moveTopLeft(QPointF(x + 14, y + 14));
        p.setPen(QPen(QColor(90, 90, 90), 1));
        p.setBrush(QColor(255, 255, 225));
        p.drawRect(r);
        p.setPen(Qt::black);
        p.drawText(r, Qt::AlignCenter, text);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Vec3 Viewport::orbitPivotAt(double x, double y) {
    if (orbitPivot_ == OrbitPivot::CameraTarget) return camera_.target;
    double t = std::numeric_limits<double>::infinity();
    inference_->pickFace(x, y, &t); // also hits components
    if (std::isfinite(t)) return camera_.ray(x, y).at(t);
    const Mesh& m = doc_->mesh();
    const Selection& sel = doc_->selection;
    Vec3 mn, mx;
    bool any = false;
    auto grow = [&](Id v) {
        const Vec3& p = m.pos(v);
        if (!any) mn = mx = p;
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], p[i]);
            mx[i] = std::max(mx[i], p[i]);
        }
        any = true;
    };
    for (Id f : sel.faces)
        if (f >= 0 && f < (Id)m.faces.size() && m.faces[f].alive)
            for (Id v : m.faceVertices(f)) grow(v);
    for (Id e : sel.edges)
        if (e >= 0 && e < (Id)m.edges.size() && m.edges[e].alive)
            for (Id v : m.edges[e].v) grow(v);
    for (Id i : sel.instances) {
        Vec3 a, b;
        if (!instanceBounds(doc_->state, i, a, b)) continue;
        if (!any) mn = a, mx = b;
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], a[k]);
            mx[k] = std::max(mx[k], b[k]);
        }
        any = true;
    }
    if (!any) modelBounds(doc_->state, mn, mx, any);
    return any ? (mn + mx) * 0.5 : Vec3();
}

MouseEvent Viewport::toEvent(QMouseEvent* e) const {
    MouseEvent m;
    m.x = e->position().x();
    m.y = e->position().y();
    m.button = e->button();
    m.buttons = e->buttons();
    m.mods = e->modifiers();
    return m;
}

void Viewport::mousePressEvent(QMouseEvent* e) {
    setFocus();
    doc_->mesh().viewHint = camera_.eye;
    doc_->mesh().groundFacesDown = false;
    if (e->button() == Qt::MiddleButton) {
        nav_ = (e->modifiers() & Qt::ShiftModifier) ? Nav::Pan : Nav::Orbit;
        navLast_ = e->position();
        navGrab_ = focusPointAt(*inference_, camera_, e->position().x(), e->position().y());
        navPivot_ = orbitPivotAt(e->position().x(), e->position().y());
        navMoved_ = false;
        setCursor(nav_ == Nav::Pan ? Qt::ClosedHandCursor : Qt::SizeAllCursor);
        return;
    }
    if (e->button() == Qt::RightButton) return;
    if (tool_) tool_->mousePress(toEvent(e));
    update();
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
    mousePos_ = e->position();
    mouseInside_ = true;
    if (nav_ != Nav::None) {
        QPointF d = e->position() - navLast_;
        navLast_ = e->position();
        if (d.manhattanLength() > 0) navMoved_ = true;
        bool pan = nav_ == Nav::Pan || (e->modifiers() & Qt::ShiftModifier);
        if (pan) camera_.panTo(navGrab_, e->position().x(), e->position().y());
        else camera_.orbitAround(navPivot_, d.x(), d.y());
        if (tool_) tool_->mouseMove(toEvent(e));
        update();
        return;
    }
    if (tool_) tool_->mouseMove(toEvent(e));
    update();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        nav_ = Nav::None;
        if (tool_) setCursor(tool_->cursor());
        return;
    }
    if (e->button() == Qt::RightButton) {
        if (tool_ && tool_->busy()) {
            tool_->reset();
            update();
            return;
        }
        PickResult pr = inference_->pick(e->position().x(), e->position().y());
        pr.object = inference_->pickObject(e->position().x(), e->position().y());
        emit contextMenuAt(e->globalPosition().toPoint(), pr);
        return;
    }
    if (tool_) tool_->mouseRelease(toEvent(e));
    update();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        // Re-center the view on the point under the cursor.
        Vec3 p = focusPointAt(*inference_, camera_, e->position().x(), e->position().y());
        Vec3 off = p - camera_.target;
        camera_.target += off;
        camera_.eye += off;
        update();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    doc_->mesh().viewHint = camera_.eye;
    doc_->mesh().groundFacesDown = false;
    if (tool_) tool_->mouseDoubleClick(toEvent(e));
    update();
}

void Viewport::wheelEvent(QWheelEvent* e) {
    // Alt+wheel arrives as horizontal scrolling on some platforms.
    const QPoint ad = e->angleDelta();
    const double toolSteps = (ad.y() != 0 ? ad.y() : ad.x()) / 120.0;
    if (tool_ && std::fabs(toolSteps) > 1e-6 && tool_->wheel(toolSteps, e->modifiers())) {
        update();
        return;
    }
    double steps = ad.y() / 120.0;
    if (std::fabs(steps) < 1e-6) return;
    Vec3 focus = focusPointAt(*inference_, camera_, e->position().x(), e->position().y());
    camera_.zoom(std::pow(0.8, steps), &focus);
    if (tool_) {
        MouseEvent m;
        m.x = e->position().x();
        m.y = e->position().y();
        m.mods = e->modifiers();
        m.buttons = e->buttons();
        tool_->mouseMove(m);
    }
    update();
}

static bool startsVcb(const QString& t) {
    if (t.isEmpty()) return false;
    QChar c = t[0];
    return c.isDigit() || c == '.' || c == '-' || c == ',' || c == '<' || c == '[' || c == '/' || c == '*' || c == '\'' || c == '"';
}

bool Viewport::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(e);
        bool typing = !vcbTyped_.isEmpty();
        QString t = ke->text();
        if (!(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !t.isEmpty() && t[0].isPrint() &&
            (typing || startsVcb(t))) {
            e->accept();
            return true;
        }
        if (typing && (ke->key() == Qt::Key_Backspace || ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ||
                       ke->key() == Qt::Key_Escape)) {
            e->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}

void Viewport::keyPressEvent(QKeyEvent* e) {
    QString typed = vcbTyped_;
    const bool typing = !vcbTyped_.isEmpty();
    auto setTyped = [&](const QString& s) {
        vcbTyped_ = s;
        emit vcbChanged(tool_ ? tool_->vcbLabel() : QString(), s);
    };
    if (e->key() == Qt::Key_Escape) {
        if (typing) setTyped(QString());
        else if (tool_ && !tool_->busy() && doc_->editingComponent()) doc_->closeComponent();
        else cancelTool();
        return;
    }
    if (typing && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        setTyped(QString());
        vcbEnter(typed);
        return;
    }
    if (typing && e->key() == Qt::Key_Backspace) {
        typed.chop(1);
        setTyped(typed);
        return;
    }
    QString t = e->text();
    bool printable = !t.isEmpty() && t[0].isPrint() && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
    if (printable && (typing || startsVcb(t))) {
        setTyped(typed + t);
        return;
    }
    // Holding X, Y or Z keeps the tool on the red, green or blue axis (what
    // the arrow keys lock in each tool) until the key is let go.
    if (const int arrow = axisArrowFor(e); arrow && tool_ && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        if (!e->isAutoRepeat() && heldAxisArrow_ != arrow) {
            heldAxisArrow_ = arrow;
            heldAxisTool_ = tool_.get();
            tool_->holdAxis(arrow == Qt::Key_Right ? 0 : arrow == Qt::Key_Left ? 1 : 2);
            refreshToolStatus();
        }
        return;
    }
    doc_->mesh().viewHint = camera_.eye;
    doc_->mesh().groundFacesDown = false;
    if (tool_ && tool_->keyPress(e)) {
        update();
        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

int Viewport::axisArrowForKey(int key) {
    switch (key) {
    case Qt::Key_X: return Qt::Key_Right; // red
    case Qt::Key_Y: return Qt::Key_Left;  // green
    case Qt::Key_Z: return Qt::Key_Up;    // blue
    default: return 0;
    }
}

int Viewport::axisArrowFor(const QKeyEvent* e) {
    if (int a = axisArrowForKey(e->key())) return a;
    // Other keyboard layouts: the key in the X/Y/Z position.
    switch (e->nativeVirtualKey()) {
    case 'x': case 'X': return Qt::Key_Right;
    case 'y': case 'Y': return Qt::Key_Left;
    case 'z': case 'Z': return Qt::Key_Up;
    default: return 0;
    }
}


void Viewport::keyReleaseEvent(QKeyEvent* e) {
    if (const int arrow = axisArrowFor(e); arrow && !e->isAutoRepeat() && arrow == heldAxisArrow_) {
        if (tool_ && heldAxisTool_ == tool_.get()) {
            tool_->holdAxis(-1);
            refreshToolStatus();
        }
        heldAxisArrow_ = 0;
        heldAxisTool_ = nullptr;
        return;
    }
    if (tool_ && tool_->keyRelease(e)) {
        update();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(e);
}

void Viewport::leaveEvent(QEvent* e) {
    mouseInside_ = false;
    update();
    QOpenGLWidget::leaveEvent(e);
}

// ---------------------------------------------------------------------------
// Drag & drop textures
// ---------------------------------------------------------------------------

static QStringList imagePaths(const QMimeData* md) {
    QStringList r;
    if (!md || !md->hasUrls()) return r;
    for (const QUrl& u : md->urls())
        if (u.isLocalFile() && isImageFile(u.toLocalFile())) r << u.toLocalFile();
    return r;
}

void Viewport::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(prefabMime())) {
        const QString path = QString::fromUtf8(md->data(prefabMime()));
        auto prefab = std::make_unique<ModelState>();
        QString err;
        if (!loadPrefab(path, *prefab, nullptr, nullptr, &err)) {
            emit statusText(err);
            e->ignore();
            return;
        }
        dragPrefabPath_ = path;
        dragPrefab_ = std::move(prefab);
        e->acceptProposedAction();
        return;
    }
    if (md->hasFormat(materialMime()) || !imagePaths(md).isEmpty()) e->acceptProposedAction();
    else e->ignore();
}

void Viewport::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(prefabMime())) {
        e->acceptProposedAction();
        dragPrefabHit_ = surfacePointAt(e->position().x(), e->position().y(), dragPrefabAt_);
        update();
        return;
    }
    if (!md->hasFormat(materialMime()) && imagePaths(md).isEmpty()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    dropHoverFace_ = inference_->pickFace(e->position().x(), e->position().y());
    update();
}

void Viewport::dragLeaveEvent(QDragLeaveEvent* e) {
    dropHoverFace_ = -1;
    dragPrefab_.reset();
    dragPrefabHit_ = false;
    update();
    QOpenGLWidget::dragLeaveEvent(e);
}

void Viewport::dropEvent(QDropEvent* e) {
    const QMimeData* md = e->mimeData();
    const double x = e->position().x(), y = e->position().y();
    if (md->hasFormat(prefabMime())) {
        ModelState prefab;
        const QString path = QString::fromUtf8(md->data(prefabMime()));
        QString err;
        Vec3 at;
        if (dragPrefab_ && dragPrefabPath_ == path) prefab = std::move(*dragPrefab_);
        else if (!loadPrefab(path, prefab, nullptr, nullptr, &err)) emit statusText(err);
        if (!prefab.defs.empty()) prefab.defs[0].prefab = QFileInfo(path).completeBaseName().toStdString();
        dragPrefab_.reset();
        dragPrefabHit_ = false;
        if (!prefab.defs.empty() && surfacePointAt(x, y, at) && placePrefab(prefab, at) != kNone) e->acceptProposedAction();
        else e->ignore();
        setFocus();
        update();
        return;
    }
    if (md->hasFormat(materialMime())) {
        dropHoverFace_ = -1;
        const QJsonDocument d = QJsonDocument::fromJson(md->data(materialMime()));
        if (d.isObject() && dropMaterial(materialFromJson(d.object()), x, y, e->modifiers())) e->acceptProposedAction();
        else e->ignore();
        update();
        return;
    }
    QStringList paths = imagePaths(md);
    dropHoverFace_ = -1;
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    bool tile = e->modifiers() & Qt::ControlModifier;
    if (!dropImage(paths.first(), x, y, tile)) addImagePlane(paths.first(), x, y);
    for (int i = 1; i < paths.size(); ++i) emit imageDropped(paths[i]);
    update();
}

bool Viewport::surfacePointAt(double x, double y, Vec3& out) {
    double t = std::numeric_limits<double>::infinity();
    inference_->pickFace(x, y, &t); // also hits components
    const Ray r = camera_.ray(x, y);
    if (std::isfinite(t)) {
        out = r.at(t);
        return true;
    }
    if (intersectRayPlane(r, Plane(Vec3(0, 0, 1), Vec3()), t) && t > 0) {
        out = r.at(t);
        return true;
    }
    return false;
}

Id Viewport::placePrefab(const ModelState& prefab, const Vec3& at) {
    if (prefab.defs.empty()) return kNone;
    if (tool_) tool_->reset();
    doc_->beginOp("Place Prefab");
    const Id def = importDefinition(*doc_, prefab, 0);
    if (!canPlaceInActive(doc_->state, def)) {
        doc_->cancelOp();
        emit statusText(QStringLiteral("A component cannot go inside itself."));
        return kNone;
    }
    Vec3 mn, mx;
    bool any = false;
    definitionBounds(doc_->state, def, mn, mx, any);
    Instance in;
    in.def = def;
    in.parent = doc_->state.activeDef();
    in.xf = Mat4::translation(at - (any ? Vec3((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z) : Vec3()));
    if (!prefab.instances.empty()) in.entity = prefab.instances[0].entity;
    doc_->state.instances.push_back(in);
    const Id inst = (Id)doc_->state.instances.size() - 1;
    doc_->selection.clear();
    doc_->selection.instances.insert(inst);
    doc_->commitOp();
    doc_->notifySelection();
    emit statusText(QStringLiteral("Placed prefab \"%1\". Everything placed from it shares its geometry.")
                        .arg(QString::fromStdString(doc_->state.defs[def].name)));
    return inst;
}

bool Viewport::dropMaterial(const Material& m, double x, double y, Qt::KeyboardModifiers mods) {
    if (inference_->pickFace(x, y) == kNone) {
        emit statusText(inference_->pickInstance(x, y) != kNone
                            ? QStringLiteral("To paint a component, open it first (double-click), then drop the material on its face.")
                            : QStringLiteral("Drop the material on a face."));
        return false;
    }
    if (tool_) tool_->reset();
    // Adding the material and painting with it are one undo step.
    doc_->beginOp("Paint");
    Id id = kNone;
    for (Id i = 0; i < (Id)doc_->materials().size() && id == kNone; ++i)
        if (sameMaterial(doc_->materials()[i], m)) id = i;
    if (id == kNone) id = doc_->addMaterial(m);
    doc_->currentMaterial = id;
    MouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.button = Qt::LeftButton;
    ev.buttons = Qt::LeftButton;
    ev.mods = mods & ~Qt::KeyboardModifiers(Qt::AltModifier); // Alt would sample
    PaintTool(this).mousePress(ev); // the active tool stays what it is
    doc_->commitOp();
    emit materialPicked();
    update();
    return true;
}

void Viewport::lookThrough(Id object) {
    if (object < 0 || object >= (Id)doc_->state.objects.size()) return;
    const SceneObject& o = doc_->state.objects[object];
    if (o.kind != SceneObject::ViewCamera || !o.alive) return;
    camera_.eye = o.pos;
    camera_.target = o.target;
    camera_.up = Vec3(0, 0, 1);
    camera_.fovY = o.fov;
    camera_.perspective = o.perspective;
    emit statusText(QStringLiteral("Looking through \"%1\".").arg(QString::fromStdString(o.name)));
    update();
}

SceneObject Viewport::objectFromView(SceneObject::Kind kind) const {
    SceneObject o;
    o.kind = kind;
    o.pos = camera_.eye;
    o.target = camera_.target;
    o.fov = camera_.fovY;
    o.perspective = camera_.perspective;
    return o;
}

bool Viewport::addImagePlane(const QString& path, double x, double y) {
    Texture tex;
    QString err;
    if (!loadTextureFile(path, tex, &err)) {
        emit statusText(err);
        return false;
    }
    return addImagePlane(tex, QFileInfo(path).completeBaseName(), x, y);
}

bool Viewport::addImagePlane(Texture tex, const QString& name, double x, double y) {
    QImage img = decodeTexture(tex);
    int pw = tex.pixelWidth > 0 ? tex.pixelWidth : img.width(), ph = tex.pixelHeight > 0 ? tex.pixelHeight : img.height();
    if (pw <= 0 || ph <= 0) return false;
    const double W = pw * 0.001, H = ph * 0.001;

    // Axis plane facing the camera most; ground plane at z = 0, vertical planes
    // through the camera target.
    const Vec3 f = camera_.forward();
    int ax = 0;
    for (int i = 1; i < 3; ++i)
        if (std::fabs(f[i]) > std::fabs(f[ax])) ax = i;
    Vec3 n;
    n[ax] = f[ax] > 0 ? -1 : 1;
    Plane pl = ax == 2 ? Plane(Vec3(0, 0, 1), Vec3()) : Plane(n, camera_.target);
    Ray r = camera_.ray(x, y);
    double t;
    Vec3 anchor = intersectRayPlane(r, pl, t) ? r.at(t) : pl.project(camera_.target);
    // Image "up": world up on vertical planes, the in-plane axis closest to the
    // screen's up on the ground. right x up = n, so the image faces the camera.
    Vec3 up(0, 0, 1);
    if (ax == 2) {
        Vec3 su = camera_.trueUp();
        up = std::fabs(su.x) > std::fabs(su.y) ? Vec3(su.x > 0 ? 1 : -1, 0, 0) : Vec3(0, su.y > 0 ? 1 : -1, 0);
    }
    const Vec3 right = up.cross(n);
    Vec3 o = anchor - right * (W / 2) - up * (H / 2);
    if (ax != 2 && o.z < 0) o.z = 0; // stand on the ground

    Material mat;
    mat.name = name.toStdString();
    tex.worldWidth = W;
    tex.worldHeight = H;
    mat.texture = tex;
    mat.color = averageColor(img);

    doc_->beginOp("Add Image");
    Mesh& m = doc_->mesh();
    Id mid = doc_->addMaterial(mat);
    m.drawPolyline({o, o + right * W, o + right * W + up * H, o + up * H}, true);
    TexMap map;
    map.valid = true;
    map.origin = o;
    map.u = right / W;
    map.v = up / H;
    const Plane face(n, o);
    doc_->selection.clear();
    for (Id g = 0; g < (Id)m.faces.size(); ++g) {
        Face& F = m.faces[g];
        if (!F.alive || std::fabs(F.normal.dot(n)) < 1 - 1e-6 || !face.contains(m.faceInteriorPoint(g))) continue;
        Vec3 d = m.faceInteriorPoint(g) - o;
        double u = d.dot(map.u), v = d.dot(map.v);
        if (u < 0 || u > 1 || v < 0 || v > 1) continue;
        for (FaceSide* s : {&F.front, &F.back}) {
            s->material = mid;
            s->tex = map;
        }
        doc_->selection.faces.insert(g);
    }
    doc_->currentMaterial = mid;
    m.touch();
    doc_->commitOp();
    doc_->notifySelection();
    emit materialPicked();
    emit statusText(QStringLiteral("Image placed at 1 px = 1 mm (%1 × %2). Scale it to size; Tools → Trace Image outlines its shape.")
                        .arg(QString::fromStdString(doc_->units.format(W)), QString::fromStdString(doc_->units.format(H))));
    update();
    return true;
}

bool Viewport::dropImage(const QString& path, double x, double y, bool tile) {
    double t;
    Id f = inference_->pickFace(x, y, &t);
    if (f == kNone) return false;
    Texture tex;
    QString err;
    if (!loadTextureFile(path, tex, &err)) {
        emit statusText(err);
        return true;
    }
    Mesh& m = doc_->mesh();
    Ray r = camera_.ray(x, y);
    Material mat;
    mat.name = QFileInfo(path).completeBaseName().toStdString();
    mat.texture = tex;
    mat.color = averageColor(decodeTexture(tex));

    std::vector<Id> targets;
    if (doc_->selection.faces.count(f)) targets.assign(doc_->selection.faces.begin(), doc_->selection.faces.end());
    else targets.push_back(f);

    doc_->beginOp("Apply Texture");
    Id mid = doc_->addMaterial(mat);
    for (Id g : targets) {
        bool front;
        if (g == f) front = m.faces[g].normal.dot(r.dir) < 0;
        else front = camera_.perspective ? m.faces[g].normal.dot(camera_.eye - m.faceInteriorPoint(g)) > 0
                                         : m.faces[g].normal.dot(camera_.forward()) < 0;
        FaceSide& s = front ? m.faces[g].front : m.faces[g].back;
        s.material = mid;
        if (tile) s.tex.valid = false;
        else s.tex = fitTextureToFace(m, g, front);
    }
    doc_->currentMaterial = mid;
    m.touch();
    doc_->commitOp();
    emit materialPicked();
    emit statusText(tile ? QStringLiteral("Texture applied (tiled at %1 per tile). Use the Materials panel to change the tile size.")
                               .arg(QString::fromStdString(doc_->units.format(tex.worldWidth)))
                         : QStringLiteral("Texture fitted to face. Hold Ctrl while dropping to tile it at real-world size instead."));
    return true;
}

} // namespace gs
