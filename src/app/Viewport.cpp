// SPDX-License-Identifier: GPL-3.0-or-later
#include "Viewport.h"

#include "CameraTools.h"
#include "DrawTools.h"
#include "ModifyTools.h"
#include "core/Components.h"
#include "io/ImageUtil.h"

#include <QApplication>
#include <QDragEnterEvent>
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
    }
    if (dropHoverFace_ >= 0) pv.highlightFaces.insert(dropHoverFace_);
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
    if (tool_) tool_->mouseDoubleClick(toEvent(e));
    update();
}

void Viewport::wheelEvent(QWheelEvent* e) {
    double steps = e->angleDelta().y() / 120.0;
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
    doc_->mesh().viewHint = camera_.eye;
    if (tool_ && tool_->keyPress(e)) {
        update();
        return;
    }
    QOpenGLWidget::keyPressEvent(e);
}

void Viewport::keyReleaseEvent(QKeyEvent* e) {
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
    if (!imagePaths(e->mimeData()).isEmpty()) e->acceptProposedAction();
    else e->ignore();
}

void Viewport::dragMoveEvent(QDragMoveEvent* e) {
    if (imagePaths(e->mimeData()).isEmpty()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    dropHoverFace_ = inference_->pickFace(e->position().x(), e->position().y());
    update();
}

void Viewport::dropEvent(QDropEvent* e) {
    QStringList paths = imagePaths(e->mimeData());
    dropHoverFace_ = -1;
    if (paths.isEmpty()) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    bool tile = e->modifiers() & Qt::ControlModifier;
    if (!dropImage(paths.first(), e->position().x(), e->position().y(), tile))
        addImagePlane(paths.first(), e->position().x(), e->position().y());
    for (int i = 1; i < paths.size(); ++i) emit imageDropped(paths[i]);
    update();
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
