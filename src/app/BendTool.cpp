// SPDX-License-Identifier: GPL-3.0-or-later
#include "BendTool.h"

#include "Viewport.h"
#include "core/Components.h"

#include <QKeyEvent>

#include <cmath>

namespace gs {

namespace {
const Vec3 kAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
} // namespace

QString BendTool::status() const {
    QString s;
    switch (stage_) {
    case 0: s = QStringLiteral("Click where the bend starts; the selection (or what you click) is bent. Arrow keys choose the axis: Right=red, Left=green, Up=blue."); break;
    case 1: s = QStringLiteral("Click where the bend ends. Beyond it the geometry follows rigidly."); break;
    default: s = QStringLiteral("Move the mouse or type the angle in degrees (snaps every 15°). Type 24s for the number of segments."); break;
    }
    return message_.isEmpty() ? s : message_ + QStringLiteral("  ") + s;
}

void BendTool::reset() {
    stage_ = 0;
    angle_ = 0;
    segments_ = 0;
    message_.clear();
    ghost_.clear();
    refreshStatus();
    update();
}

Vec3 BendTool::axis() const {
    if (forcedAxis_ >= 0) return kAxes[forcedAxis_];
    // Around the vertical, unless the stretch itself runs up: then across the view.
    const Vec3 d = (stage_ == 0 ? Vec3(1, 0, 0) : (stage_ == 1 ? cur_ : end_) - start_).normalized();
    if (std::fabs(d.z) < 0.95) return Vec3(0, 0, 1);
    Vec3 f = const_cast<BendTool*>(this)->cam().forward();
    f.z = 0;
    return f.length() > 1e-6 ? f.normalized() : Vec3(0, 1, 0);
}

bool BendTool::params(double angle, BendParams& out) const {
    return makeBend(start_, end_, axis(), angle, segments_, out);
}

bool BendTool::keyPress(QKeyEvent* e) {
    int a = -1;
    switch (e->key()) {
    case Qt::Key_Right: a = 0; break;
    case Qt::Key_Left: a = 1; break;
    case Qt::Key_Up: a = 2; break;
    default: return Tool::keyPress(e);
    }
    const int before = forcedAxis_;
    forcedAxis_ = forcedAxis_ == a ? -1 : a;
    // The stretch cannot run along the axis.
    BendParams test;
    if (stage_ >= 2 && !params(0, test)) forcedAxis_ = before;
    mouseMove(lastMouse_);
    update();
    return true;
}

void BendTool::holdAxis(int axis) {
    if (axis >= 0 && heldAxis_ < 0) heldSaved_ = forcedAxis_;
    const int before = forcedAxis_;
    forcedAxis_ = axis >= 0 ? axis : heldSaved_;
    BendParams test;
    if (stage_ >= 2 && !params(0, test)) forcedAxis_ = before; // the stretch cannot run along the axis
    heldAxis_ = axis;
    mouseMove(lastMouse_);
    update();
}

bool BendTool::ensureSelection(const MouseEvent& e) {
    Document& d = doc();
    if (!d.selection.empty()) return true;
    const PickResult pr = engine().pick(e.x, e.y);
    if (pr.instance != kNone) {
        d.selection.instances.insert(pr.instance);
    } else {
        // Everything connected to what was clicked.
        const Mesh& m = mesh();
        std::vector<Id> open;
        if (pr.face != kNone) open = m.faceVertices(pr.face);
        else if (pr.edge != kNone) open = {m.edges[pr.edge].v[0], m.edges[pr.edge].v[1]};
        std::set<Id> seen(open.begin(), open.end());
        while (!open.empty()) {
            const Id v = open.back();
            open.pop_back();
            for (Id e2 : m.vertexEdges(v)) {
                const Edge& E = m.edges[e2];
                if (!E.alive) continue;
                d.selection.edges.insert(e2);
                for (Id f : m.edgeFaces(e2))
                    if (m.faces[f].alive && !m.faces[f].hidden) d.selection.faces.insert(f);
                const Id o = E.other(v);
                if (seen.insert(o).second) open.push_back(o);
            }
        }
    }
    d.notifySelection();
    return !d.selection.empty();
}

void BendTool::collectGhost() {
    ghost_.clear();
    const Document& d = doc();
    const Mesh& m = d.mesh();
    std::set<Id> edges = d.selection.edges;
    for (Id f : d.selection.faces)
        for (Id e : m.faceEdges(f)) edges.insert(e);
    for (Id e : edges)
        if (m.edges[e].alive && !m.edges[e].soft && !m.edges[e].hidden) ghost_.emplace_back(m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1]));
    if (d.editingComponent()) return;
    for (Id i : d.selection.instances) {
        const Mat4 xf = instanceWorldXf(d.state, i);
        const Mesh& def = d.state.defs[d.state.instances[i].def].mesh;
        for (const Edge& E : def.edges)
            if (E.alive && !E.soft && !E.hidden) ghost_.emplace_back(xf.transformPoint(def.pos(E.v[0])), xf.transformPoint(def.pos(E.v[1])));
    }
}

void BendTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    message_.clear();
    if (stage_ == 0) {
        InferenceResult r = inferPoint(e, std::nullopt);
        if (!ensureSelection(e)) {
            message_ = QStringLiteral("Nothing to bend here.");
            refreshStatus();
            return;
        }
        start_ = cur_ = r.p;
        collectGhost();
        stage_ = 1;
    } else if (stage_ == 1) {
        InferenceResult r = inferPoint(e, start_);
        end_ = r.p;
        BendParams test;
        if (!params(0, test)) {
            end_ = cur_ = start_;
            message_ = QStringLiteral("The bend needs some length across its axis.");
            refreshStatus();
            return;
        }
        angle_ = 0;
        cur_ = end_;
        stage_ = 2;
    } else {
        commit();
        return;
    }
    refreshStatus();
    update();
}

void BendTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (stage_ == 0) {
        cur_ = inferPoint(e, std::nullopt).p;
    } else if (stage_ == 1) {
        cur_ = inferPoint(e, start_).p;
    } else {
        BendParams flat;
        if (!params(0, flat)) return;
        const InferenceResult r = inferPoint(e, std::nullopt, Plane(flat.axis, end_), nullptr, false);
        cur_ = r.p;
        // The bend's end lies in the direction of the cursor as seen from its
        // start: a chord leaves an arc at half the arc's angle.
        const Vec3 d = cur_ - start_;
        const double raw = 2 * std::atan2(d.dot(flat.axis.cross(flat.dir)), d.dot(flat.dir));
        const double deg = raw * 180 / kPi;
        const double snap = std::round(deg / 15.0) * 15.0;
        snapped_ = std::fabs(deg - snap) < 2.5;
        angle_ = snapped_ ? snap * kPi / 180 : raw;
        setVcb(QString::number(angle_ * 180 / kPi, 'f', 1) + QStringLiteral("°"));
    }
    update();
}

void BendTool::commit() {
    BendParams p;
    if (!params(angle_, p)) return;
    if (std::fabs(angle_) < 1e-6) {
        message_ = QStringLiteral("Bend by some angle first.");
        refreshStatus();
        return;
    }
    Document& d = doc();
    d.beginOp("Bend");
    Selection sel = d.selection;
    std::string err;
    if (!applyBend(d.state, sel, p, &err)) {
        d.cancelOp();
        message_ = QString::fromStdString(err);
        refreshStatus();
        update();
        return;
    }
    d.selection = sel;
    d.commitOp();
    d.notifySelection();
    const int keepAxis = forcedAxis_;
    reset();
    forcedAxis_ = keepAxis;
}

bool BendTool::vcbEnter(const QString& text) {
    QString t = text.trimmed();
    if (t.endsWith(QLatin1Char('s'), Qt::CaseInsensitive)) {
        bool ok = false;
        const int n = t.chopped(1).toInt(&ok);
        if (!ok || n < 1 || n > 1000) return false;
        segments_ = n;
        update();
        return true;
    }
    if (stage_ != 2) return false;
    t.remove(QStringLiteral("°"));
    t.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double deg = t.toDouble(&ok);
    if (!ok) return false;
    angle_ = deg * kPi / 180;
    commit();
    return true;
}

void BendTool::buildPreview(Preview& pv) {
    const Color axisColor = forcedAxis_ >= 0 ? Renderer::axisColor(forcedAxis_) : Renderer::axisColor(2);
    if (stage_ == 0) {
        engine().addPreview(inf_, pv);
        pv.markers.push_back({cur_, Preview::MarkerKind::Plain});
        return;
    }
    if (stage_ == 1) {
        pv.line(start_, cur_, Color::rgb8(220, 120, 0), 2.0f);
        engine().addPreview(inf_, pv);
        return;
    }
    BendParams p;
    if (!params(angle_, p)) return;
    // The geometry as it will be, cut where the bend will cut it.
    const Color ghostColor = Color::rgb8(0, 110, 220);
    const int n = p.segmentCount();
    const double step = p.length / n;
    for (const auto& [a, b] : ghost_) {
        const double sa = (a - p.origin).dot(p.dir), sb = (b - p.origin).dot(p.dir);
        std::vector<double> ts{0.0, 1.0};
        if (std::fabs(sb - sa) > 1e-9)
            for (int k = 0; k <= n; ++k) {
                const double t = (k * step - sa) / (sb - sa);
                if (t > 0 && t < 1) ts.push_back(t);
            }
        std::sort(ts.begin(), ts.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i)
            pv.line(p.apply(lerp(a, b, ts[i])), p.apply(lerp(a, b, ts[i + 1])), ghostColor, 1.2f);
    }
    // The stretch before and after, and the axis at the arc's centre.
    pv.line(start_, end_, Color::rgb8(220, 120, 0), 1.2f, Dash::Dashed);
    for (int k = 0; k < n; ++k)
        pv.line(p.apply(p.origin + p.dir * (k * step)), p.apply(p.origin + p.dir * ((k + 1) * step)), Color::rgb8(220, 120, 0), 2.0f);
    if (std::fabs(p.angle) > 1e-6) {
        const Vec3 centre = p.origin + p.axis.cross(p.dir) * p.radius();
        const double len = cam().pixelSize(centre) * 40;
        pv.line(centre - p.axis * len, centre + p.axis * len, axisColor, 2.0f);
    }
    pv.line(start_, cur_, snapped_ ? Color::rgb8(220, 0, 220) : Color::rgb8(0, 0, 0), 1.0f, Dash::Dotted);
}

} // namespace gs
