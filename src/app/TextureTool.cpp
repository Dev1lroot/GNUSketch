// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextureTool.h"

#include "Viewport.h"
#include "io/ImageUtil.h"

#include <QPainter>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

constexpr double kHandleRadius = 7;
constexpr double kMinScale = 1e-3;

int indexOf(TextureTool::Handle h, TextureTool::Handle first) { return (int)h - (int)first; }
bool isCorner(TextureTool::Handle h) { return h >= TextureTool::Handle::Corner0 && h <= TextureTool::Handle::Corner3; }
bool isEdge(TextureTool::Handle h) { return h >= TextureTool::Handle::Edge0 && h <= TextureTool::Handle::Edge3; }
bool isSkew(TextureTool::Handle h) { return h >= TextureTool::Handle::Skew0 && h <= TextureTool::Handle::Skew3; }

Vec2 cornerUv(const Vec2& lo, int i) {
    static const int X[4] = {0, 1, 1, 0}, Y[4] = {0, 0, 1, 1};
    return {lo.x + X[i & 3], lo.y + Y[i & 3]};
}
Vec2 edgeUv(const Vec2& lo, int i) {
    static const double X[4] = {0.5, 1, 0.5, 0}, Y[4] = {0, 0.5, 1, 0.5};
    return {lo.x + X[i & 3], lo.y + Y[i & 3]};
}

// Keeps a scale from collapsing the texture; sign is kept, which mirrors it.
double usableScale(double s) { return std::fabs(s) < kMinScale ? (s < 0 ? -kMinScale : kMinScale) : s; }

} // namespace

QString TextureTool::status() const {
    QString s;
    if (drag_ != Handle::None) {
        if (drag_ == Handle::Move) s = QStringLiteral("Moving the texture. Type distances along it: 0.5,0.2.");
        else if (drag_ == Handle::Rotate) s = QStringLiteral("Rotating (snaps every 15°). Type an angle in degrees.");
        else if (isCorner(drag_)) s = QStringLiteral("Resizing: Shift = free proportions, Ctrl = about the centre. Drag across to mirror. Type a factor: 2 or 2,1.");
        else if (isEdge(drag_)) s = QStringLiteral("Stretching: Ctrl = about the centre. Type a factor.");
        else s = QStringLiteral("Skewing. Type an angle in degrees.");
    } else {
        s = QStringLiteral("Drag inside the outline to move, corners to resize, side squares to stretch, diamonds to skew, the circle to rotate. Click a handle to type a value. Click another face to select it (Shift: add).");
    }
    return message_.isEmpty() ? s : message_ + QStringLiteral("  ") + s;
}

QString TextureTool::vcbLabel() const { return QStringLiteral("Texture"); }

void TextureTool::activate() {
    refreshStatus();
    update();
}

void TextureTool::reset() {
    cancelDrag();
    message_.clear();
    refreshStatus();
    update();
}

FaceSide& TextureTool::sideOf(const Target& t) {
    Face& F = mesh().faces[t.face];
    return t.front ? F.front : F.back;
}

std::vector<TextureTool::Target> TextureTool::currentTargets() {
    std::vector<Target> out;
    const Mesh& m = mesh();
    const Document& d = doc();
    for (Id f : d.selection.faces) {
        if (f < 0 || f >= (Id)m.faces.size() || !m.faces[f].alive) continue;
        const Face& F = m.faces[f];
        const Camera& c = const_cast<TextureTool*>(this)->cam();
        const bool facing = c.perspective ? F.normal.dot(c.eye - m.faceInteriorPoint(f)) > 0 : F.normal.dot(c.forward()) < 0;
        // The side in view; if only the other one has a texture, that one.
        auto textured = [&](bool front) {
            const Material* mat = d.material((front ? F.front : F.back).material);
            return mat && mat->hasTexture();
        };
        bool front = facing;
        if (!textured(front)) front = !front;
        if (!textured(front)) continue;
        out.push_back({f, front, explicitTexMap(F, front, d.material((front ? F.front : F.back).material))});
    }
    return out;
}

TextureTool::Gizmo TextureTool::gizmo() {
    Gizmo g;
    const std::vector<Target> targets = currentTargets();
    if (targets.empty()) return g;
    const Mesh& m = mesh();
    // Keeps to the face last clicked; else the largest, which shows the most.
    const Target* ref = nullptr;
    for (const Target& t : targets)
        if (t.face == reference_) ref = &t;
    if (!ref) {
        double best = -1;
        for (const Target& t : targets) {
            const double a = m.faceArea(t.face);
            if (a > best) { best = a; ref = &t; }
        }
    }
    const Vec3 centre = m.faceInteriorPoint(ref->face);
    g.facePlane = m.facePlane(ref->face);
    if (!g.plane.set(ref->start, g.facePlane.n, centre)) return g;
    const Vec2 uv = g.plane.uvAt(centre);
    g.corner = Vec2(std::floor(uv.x), std::floor(uv.y));
    g.ok = true;
    return g;
}

bool TextureTool::screen(const Vec3& p, QPointF& out) const {
    double x, y;
    if (!const_cast<TextureTool*>(this)->cam().project(p, x, y)) return false;
    out = QPointF(x, y);
    return true;
}

bool TextureTool::cursorOnPlane(const MouseEvent& e, const Gizmo& g, Vec3& p) const {
    const Ray r = const_cast<TextureTool*>(this)->cam().ray(e.x, e.y);
    double t;
    if (!intersectRayPlane(r, g.facePlane, t)) return false;
    p = r.at(t);
    return true;
}

std::optional<QPointF> TextureTool::handleAt(Handle h) {
    const Gizmo g = drag_ != Handle::None ? dragGizmo_ : gizmo();
    if (!g.ok) return std::nullopt;
    QPointF c[4], centre;
    for (int i = 0; i < 4; ++i)
        if (!screen(g.plane.pointAt(cornerUv(g.corner, i)), c[i])) return std::nullopt;
    if (!screen(g.plane.pointAt(g.corner + Vec2(0.5, 0.5)), centre)) return std::nullopt;
    auto outward = [&](QPointF from, double px) {
        QPointF d = from - centre;
        const double len = std::hypot(d.x(), d.y());
        return len > 1e-6 ? from + d / len * px : from + QPointF(px, 0);
    };
    if (h == Handle::Move) return centre;
    if (h == Handle::Rotate) return outward(c[2], 30);
    if (isCorner(h)) return c[indexOf(h, Handle::Corner0)];
    if (isEdge(h) || isSkew(h)) {
        const int i = isEdge(h) ? indexOf(h, Handle::Edge0) : indexOf(h, Handle::Skew0);
        QPointF mid;
        if (!screen(g.plane.pointAt(edgeUv(g.corner, i)), mid)) return std::nullopt;
        return isEdge(h) ? mid : outward(mid, 24);
    }
    return std::nullopt;
}

TextureTool::Handle TextureTool::handleUnder(const MouseEvent& e) {
    const Gizmo g = gizmo();
    if (!g.ok) return Handle::None;
    const QPointF at(e.x, e.y);
    auto near = [&](Handle h) {
        const auto p = handleAt(h);
        return p && std::hypot(p->x() - at.x(), p->y() - at.y()) <= kHandleRadius + 2;
    };
    if (near(Handle::Rotate)) return Handle::Rotate;
    for (int i = 0; i < 4; ++i)
        if (near(Handle(int(Handle::Corner0) + i))) return Handle(int(Handle::Corner0) + i);
    for (int i = 0; i < 4; ++i)
        if (near(Handle(int(Handle::Edge0) + i))) return Handle(int(Handle::Edge0) + i);
    for (int i = 0; i < 4; ++i)
        if (near(Handle(int(Handle::Skew0) + i))) return Handle(int(Handle::Skew0) + i);
    // Anywhere on the outline's inside, or on a face being adjusted (they may
    // show other repeats of the texture): moves it.
    QPolygonF quad;
    for (int i = 0; i < 4; ++i) {
        QPointF c;
        if (!screen(g.plane.pointAt(cornerUv(g.corner, i)), c)) return Handle::None;
        quad << c;
    }
    if (quad.containsPoint(at, Qt::OddEvenFill)) return Handle::Move;
    const Id f = engine().pickFace(e.x, e.y);
    if (f != kNone)
        for (const Target& t : currentTargets())
            if (t.face == f) return Handle::Move;
    return Handle::None;
}

void TextureTool::selectUnder(const MouseEvent& e) {
    const Id f = engine().pickFace(e.x, e.y);
    Document& d = doc();
    if (f == kNone) {
        message_ = engine().pickInstance(e.x, e.y) != kNone
                       ? QStringLiteral("Double-click a component with the Select tool to open it, then position its textures.")
                       : QString();
        refreshStatus();
        return;
    }
    if (!e.shift()) d.selection.clear();
    d.selection.faces.insert(f);
    reference_ = f;
    d.notifySelection();
    message_ = currentTargets().empty() ? QStringLiteral("This face has no texture: paint one on it first.") : QString();
    refreshStatus();
    update();
}

void TextureTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (sticky_) {
        // Second click of click-move-click.
        commitDrag();
        return;
    }
    const Handle h = handleUnder(e);
    const Id clicked = engine().pickFace(e.x, e.y);
    clickedOther_ = h == Handle::Move && clicked != kNone && !doc().selection.faces.count(clicked);
    if (h == Handle::None) {
        selectUnder(e);
        return;
    }
    const Gizmo g = gizmo();
    Vec3 p;
    if (!g.ok || !cursorOnPlane(e, g, p)) return;
    dragGizmo_ = g;
    dragTargets_ = currentTargets();
    dragSelection_ = doc().selection;
    doc().beginOp("Position Texture");
    drag_ = h;
    grabbed_ = p;
    grabbedUv_ = g.plane.uvAt(p);
    grabbedAngle_ = g.plane.angleOf(g.corner + Vec2(0.5, 0.5), p);
    pressX_ = e.x;
    pressY_ = e.y;
    movedMouse_ = false;
    moved_ = UvAffine();
    refreshStatus();
    update();
}

void TextureTool::applyMoved(const UvAffine& moved) {
    moved_ = moved;
    for (const Target& t : dragTargets_) {
        TexMap map = t.start;
        if (moveTexMap(map, moved)) sideOf(t).tex = map;
    }
    // Only the texture changed: geometry caches stay, the renderer rebuilds.
    mesh().revision = nextMeshRevision();
    update();
}

void TextureTool::updateDrag(const MouseEvent& e) {
    Vec3 p;
    if (!cursorOnPlane(e, dragGizmo_, p)) return;
    const Gizmo& g = dragGizmo_;
    const Vec2 lo = g.corner, centre = lo + Vec2(0.5, 0.5);

    if (isCorner(drag_)) {
        // Lands exactly on the corners of the faces, for exact fits.
        QPointF at(e.x, e.y);
        double best = 10;
        for (const Target& t : dragTargets_)
            for (Id v : mesh().faceVertices(t.face)) {
                QPointF s;
                if (!screen(mesh().pos(v), s)) continue;
                const double dist = std::hypot(s.x() - at.x(), s.y() - at.y());
                if (dist < best && g.facePlane.contains(mesh().pos(v), 1e-4)) {
                    best = dist;
                    p = mesh().pos(v);
                }
            }
    }
    const Vec2 q = g.plane.uvAt(p);

    if (drag_ == Handle::Move) {
        applyMoved(UvAffine::translate(q.x - grabbedUv_.x, q.y - grabbedUv_.y));
    } else if (drag_ == Handle::Rotate) {
        double a = g.plane.angleOf(centre, p) - grabbedAngle_;
        const double deg = a * 180 / kPi, snap = std::round(deg / 15) * 15;
        if (std::fabs(deg - snap) < 2.5) a = snap * kPi / 180;
        setVcb(QString::number(a * 180 / kPi, 'f', 1) + QStringLiteral("°"));
        applyMoved(g.plane.rotationAbout(centre, a));
    } else if (isCorner(drag_)) {
        const int i = indexOf(drag_, Handle::Corner0);
        const Vec2 anchor = e.ctrl() ? centre : cornerUv(lo, i + 2);
        const Vec2 from = cornerUv(lo, i) - anchor, to = q - anchor;
        double sx = to.x / from.x, sy = to.y / from.y;
        if (!e.shift()) sx = sy = (to.x * from.x + to.y * from.y) / (from.x * from.x + from.y * from.y);
        sx = usableScale(sx);
        sy = usableScale(sy);
        setVcb(e.shift() ? QStringLiteral("%1,%2").arg(sx, 0, 'f', 2).arg(sy, 0, 'f', 2) : QString::number(sx, 'f', 2));
        applyMoved(UvAffine::scaleAbout(anchor, sx, sy));
    } else if (isEdge(drag_)) {
        const int i = indexOf(drag_, Handle::Edge0);
        const Vec2 anchor = e.ctrl() ? centre : edgeUv(lo, i + 2);
        const Vec2 from = edgeUv(lo, i) - anchor, to = q - anchor;
        const bool horizontal = i % 2 == 1; // left and right squares stretch along u
        const double s = usableScale(horizontal ? to.x / from.x : to.y / from.y);
        setVcb(QString::number(s, 'f', 2));
        applyMoved(UvAffine::scaleAbout(anchor, horizontal ? s : 1, horizontal ? 1 : s));
    } else if (isSkew(drag_)) {
        const int i = indexOf(drag_, Handle::Skew0);
        const Vec2 anchor = edgeUv(lo, i + 2), mid = edgeUv(lo, i);
        const bool alongU = i % 2 == 0; // bottom and top edges slide along u
        const double k = alongU ? (q.x - grabbedUv_.x) / (mid.y - anchor.y) : (q.y - grabbedUv_.y) / (mid.x - anchor.x);
        applyMoved(UvAffine::shearAbout(anchor, k, alongU));
    }
}

void TextureTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (drag_ != Handle::None) {
        if (std::hypot(e.x - pressX_, e.y - pressY_) > 3) movedMouse_ = true;
        if (movedMouse_ || sticky_) updateDrag(e);
        return;
    }
    const Handle h = handleUnder(e);
    if (h != hover_) {
        hover_ = h;
        if (h == Handle::Move) vp_->setCursor(Qt::SizeAllCursor);
        else if (h == Handle::None) vp_->setCursor(cursor());
        else if (h == Handle::Rotate) vp_->setCursor(Qt::CrossCursor);
        else vp_->setCursor(Qt::PointingHandCursor);
        update();
    }
}

void TextureTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || drag_ == Handle::None || sticky_) return;
    if (movedMouse_) {
        commitDrag();
    } else if (drag_ == Handle::Move) {
        // A click, not a drag: on a face not yet selected it selects that.
        const bool other = clickedOther_;
        cancelDrag();
        if (other) selectUnder(e);
    } else {
        // Clicked a handle: it follows the mouse until the next click, and takes typed values.
        sticky_ = true;
        refreshStatus();
    }
}

void TextureTool::commitDrag() {
    if (drag_ == Handle::None) return;
    const bool identity = moved_.a == 1 && moved_.b == 0 && moved_.c == 0 && moved_.d == 1 && moved_.e == 0 && moved_.f == 0;
    drag_ = Handle::None;
    sticky_ = false;
    if (identity) {
        doc().cancelOp();
        doc().selection = dragSelection_;
    } else {
        doc().commitOp();
    }
    setVcb(QString());
    refreshStatus();
    update();
}

void TextureTool::cancelDrag() {
    if (drag_ == Handle::None) return;
    drag_ = Handle::None;
    sticky_ = false;
    doc().cancelOp();
    doc().selection = dragSelection_;
    setVcb(QString());
    update();
}

bool TextureTool::vcbEnter(const QString& text) {
    if (drag_ == Handle::None) {
        message_ = QStringLiteral("Click a handle first, then type its value.");
        refreshStatus();
        return false;
    }
    const Gizmo& g = dragGizmo_;
    const Vec2 lo = g.corner, centre = lo + Vec2(0.5, 0.5);
    QString t = text.trimmed();
    t.remove(QStringLiteral("°"));
    auto number = [](const QString& s, double& out) { return parseNumber(s.trimmed().toStdString(), out); };

    if (drag_ == Handle::Move) {
        std::optional<double> a, b;
        if (!parseLen2(t, a, b)) return false;
        applyMoved(UvAffine::translate(a.value_or(0) / g.plane.lengthU(), b.value_or(0) / g.plane.lengthV()));
    } else if (drag_ == Handle::Rotate) {
        double deg;
        if (!number(t, deg)) return false;
        applyMoved(g.plane.rotationAbout(centre, deg * kPi / 180));
    } else if (isCorner(drag_)) {
        const QStringList parts = t.split(QLatin1Char(','));
        double sx, sy;
        if (parts.size() > 2 || !number(parts[0], sx)) return false;
        sy = sx;
        if (parts.size() == 2 && !number(parts[1], sy)) return false;
        const Vec2 anchor = cornerUv(lo, indexOf(drag_, Handle::Corner0) + 2);
        if (sx == 0 || sy == 0) return false;
        applyMoved(UvAffine::scaleAbout(anchor, sx, sy));
    } else if (isEdge(drag_)) {
        double s;
        if (!number(t, s) || s == 0) return false;
        const int i = indexOf(drag_, Handle::Edge0);
        applyMoved(UvAffine::scaleAbout(edgeUv(lo, i + 2), i % 2 == 1 ? s : 1, i % 2 == 1 ? 1 : s));
    } else if (isSkew(drag_)) {
        double deg;
        if (!number(t, deg) || std::fabs(deg) >= 89) return false;
        const int i = indexOf(drag_, Handle::Skew0);
        const bool alongU = i % 2 == 0;
        const Vec2 anchor = edgeUv(lo, i + 2), mid = edgeUv(lo, i);
        // Angle in the world, for this tile's proportions.
        const double ratio = alongU ? g.plane.lengthV() / g.plane.lengthU() : g.plane.lengthU() / g.plane.lengthV();
        const double k = std::tan(deg * kPi / 180) * ratio * (alongU ? (mid.y - anchor.y > 0 ? 1 : -1) * 1.0 : (mid.x - anchor.x > 0 ? 1 : -1) * 1.0);
        applyMoved(UvAffine::shearAbout(anchor, k, alongU));
    }
    commitDrag();
    return true;
}

void TextureTool::buildPreview(Preview& pv) {
    pv.hideFaceSelection = true; // the texture is what is being looked at
    const Gizmo g = drag_ != Handle::None ? dragGizmo_ : gizmo();
    if (!g.ok) return;
    // Where the tile lies now: the outline follows what was done during a drag.
    Vec3 c[4];
    for (int i = 0; i < 4; ++i) {
        Vec2 uv = cornerUv(g.corner, i);
        if (drag_ != Handle::None) uv = moved_.apply(uv);
        c[i] = g.plane.pointAt(uv);
    }
    const Color orange = Color::rgb8(240, 120, 0);
    for (int i = 0; i < 4; ++i) pv.line(c[i], c[(i + 1) % 4], orange, 1.8f);
}

void TextureTool::paintOverlay(QPainter& p) {
    const bool dragging = drag_ != Handle::None;
    const Gizmo g = dragging ? dragGizmo_ : gizmo();
    if (!g.ok) return;
    p.setRenderHint(QPainter::Antialiasing, true);

    QPointF c[4];
    for (int i = 0; i < 4; ++i) {
        Vec2 uv = cornerUv(g.corner, i);
        if (dragging) uv = moved_.apply(uv);
        if (!screen(g.plane.pointAt(uv), c[i])) return;
    }
    if (dragging && !dragTargets_.empty()) {
        // The whole image, faint: also where it reaches past the faces.
        const Face& F = mesh().faces[dragTargets_.front().face];
        const Material* mat = doc().material((dragTargets_.front().front ? F.front : F.back).material);
        if (mat && mat->hasTexture()) {
            if (imageRevision_ != mat->texture.revision || image_.isNull()) {
                image_ = decodeTexture(mat->texture);
                imageRevision_ = mat->texture.revision;
            }
            if (!image_.isNull()) {
                const double w = image_.width(), h = image_.height();
                QPolygonF from, to;
                from << QPointF(0, h) << QPointF(w, h) << QPointF(w, 0) << QPointF(0, 0); // v runs up the image
                to << c[0] << c[1] << c[2] << c[3];
                QTransform xf;
                if (QTransform::quadToQuad(from, to, xf)) {
                    p.save();
                    p.setOpacity(0.35);
                    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    p.setTransform(xf, true);
                    p.drawImage(QPointF(0, 0), image_);
                    p.restore();
                }
            }
        }
        return;
    }
    if (dragging) return;

    auto color = [&](Handle h) { return hover_ == h ? QColor(255, 210, 0) : QColor(255, 255, 255); };
    const QPen pen(QColor(190, 90, 0), 1.3);
    p.setPen(pen);
    for (int i = 0; i < 4; ++i) {
        const Handle h = Handle(int(Handle::Skew0) + i);
        const auto at = handleAt(h);
        if (!at) continue;
        const double r = kHandleRadius - 1;
        QPolygonF diamond;
        diamond << *at + QPointF(0, -r) << *at + QPointF(r, 0) << *at + QPointF(0, r) << *at + QPointF(-r, 0);
        p.setBrush(color(h));
        p.drawPolygon(diamond);
    }
    for (int i = 0; i < 4; ++i) {
        const Handle h = Handle(int(Handle::Edge0) + i);
        const auto at = handleAt(h);
        if (!at) continue;
        p.setBrush(color(h));
        p.drawRect(QRectF(*at - QPointF(3.5, 3.5), QSizeF(7, 7)));
    }
    for (int i = 0; i < 4; ++i) {
        const Handle h = Handle(int(Handle::Corner0) + i);
        const auto at = handleAt(h);
        if (!at) continue;
        p.setBrush(color(h));
        p.drawRect(QRectF(*at - QPointF(5, 5), QSizeF(10, 10)));
    }
    if (const auto at = handleAt(Handle::Rotate)) {
        p.drawLine(c[2], *at);
        p.setBrush(color(Handle::Rotate));
        p.drawEllipse(*at, kHandleRadius, kHandleRadius);
    }
    if (const auto at = handleAt(Handle::Move)) {
        p.drawLine(*at + QPointF(-6, 0), *at + QPointF(6, 0));
        p.drawLine(*at + QPointF(0, -6), *at + QPointF(0, 6));
    }
}

} // namespace gs
