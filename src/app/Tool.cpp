// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tool.h"

#include "Icons.h"
#include "Viewport.h"

#include <QKeyEvent>

namespace gs {

Document& Tool::doc() { return vp_->doc(); }
Mesh& Tool::mesh() { return vp_->doc().mesh(); }
Camera& Tool::cam() { return vp_->camera(); }
InferenceEngine& Tool::engine() { return vp_->inference(); }
void Tool::update() { vp_->update(); }
void Tool::refreshStatus() { vp_->refreshToolStatus(); }
void Tool::setVcb(const QString& value) { vp_->setVcbValue(value); }
QCursor Tool::cursor() const { return Icons::toolCursor(id()); }

QString Tool::fmtLen(double m) const { return QString::fromStdString(vp_->doc().units.format(m)); }

bool Tool::parseLen(const QString& text, double& meters) const {
    return vp_->doc().units.parseLength(text.toStdString(), meters);
}

bool Tool::parseLen2(const QString& text, std::optional<double>& a, std::optional<double>& b) const {
    QString t = text;
    t.replace(';', '|');
    // "1,5m" is ambiguous with a decimal comma; a single comma between two
    // numbers is treated as a separator when both halves parse.
    QStringList parts = t.contains('|') ? t.split('|') : t.split(',');
    if (parts.size() == 1) {
        double v;
        if (!parseLen(parts[0], v)) return false;
        a = v;
        return true;
    }
    if (parts.size() != 2) return false;
    double v;
    if (!parts[0].trimmed().isEmpty()) {
        if (!parseLen(parts[0], v)) return false;
        a = v;
    }
    if (!parts[1].trimmed().isEmpty()) {
        if (!parseLen(parts[1], v)) return false;
        b = v;
    }
    return true;
}

void Tool::clearLocks() {
    lockDir_.reset();
    lockAxis_ = -1;
    shiftLock_ = false;
}

Color Tool::lockColor() const {
    if (lockAxis_ >= 0) return Renderer::axisColor(lockAxis_);
    return Color::rgb8(220, 0, 220);
}

Color Tool::lineColor(const Vec3& a, const Vec3& b) const {
    Vec3 d = (b - a).normalized();
    for (int i = 0; i < 3; ++i)
        if (std::fabs(d[i]) > 1 - 1e-9) return Renderer::axisColor(i);
    if (lockDir_ && lockAxis_ < 0) return Color::rgb8(220, 0, 220);
    return Color::rgb8(0, 0, 0);
}

InferenceResult Tool::inferPoint(const MouseEvent& e, std::optional<Vec3> ref, std::optional<Plane> plane,
                                 const std::set<Id>* exclude, bool axes) {
    InferenceEngine::Options o;
    o.ref = ref;
    if (ref && lockDir_) {
        o.lockDir = lockDir_;
        o.lockAxis = lockAxis_;
    }
    o.plane = plane;
    o.excludeVerts = exclude;
    o.axes = axes;
    inf_ = engine().infer(e.x, e.y, o);
    return inf_;
}

bool Tool::keyPress(QKeyEvent* e) {
    static const Vec3 axes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    auto toggleAxis = [&](int a) {
        if (lockAxis_ == a && !shiftLock_) {
            clearLocks();
        } else {
            lockDir_ = axes[a];
            lockAxis_ = a;
            shiftLock_ = false;
        }
        mouseMove(lastMouse_);
        update();
        return true;
    };
    switch (e->key()) {
    case Qt::Key_Right: return toggleAxis(0);
    case Qt::Key_Left: return toggleAxis(1);
    case Qt::Key_Up: return toggleAxis(2);
    case Qt::Key_Down:
        // Lock to the last inferred non-axis direction (parallel to an edge).
        if (lockDir_ && lockAxis_ < 0) clearLocks();
        else if (inf_.hasDir) {
            lockDir_ = inf_.dir;
            lockAxis_ = -1;
            for (int i = 0; i < 3; ++i)
                if (std::fabs(inf_.dir.dot(axes[i])) > 1 - 1e-9) lockAxis_ = i;
        }
        mouseMove(lastMouse_);
        update();
        return true;
    case Qt::Key_Shift:
        if (!e->isAutoRepeat() && !lockDir_ && inf_.hasDir) {
            lockDir_ = inf_.dir;
            lockAxis_ = -1;
            for (int i = 0; i < 3; ++i)
                if (std::fabs(inf_.dir.dot(axes[i])) > 1 - 1e-9) lockAxis_ = i;
            shiftLock_ = true;
            update();
        }
        return false;
    default: break;
    }
    return false;
}

bool Tool::keyRelease(QKeyEvent* e) {
    if (e->key() == Qt::Key_Shift && !e->isAutoRepeat() && shiftLock_) {
        clearLocks();
        mouseMove(lastMouse_);
        update();
    }
    return false;
}

} // namespace gs
