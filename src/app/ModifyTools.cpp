// SPDX-License-Identifier: GPL-3.0-or-later
#include "ModifyTools.h"

#include "Viewport.h"
#include "core/Components.h"
#include "core/FollowMe.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>

#include <cmath>
#include <functional>

namespace gs {

namespace {

const Vec3 kAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};

bool parseArray(const QString& textIn, int& n, bool& divide) {
    QString t = textIn.trimmed().toLower();
    divide = false;
    if (t.startsWith('/')) { divide = true; t = t.mid(1); }
    else if (t.endsWith('/')) { divide = true; t.chop(1); }
    else if (t.startsWith('x') || t.startsWith('*')) t = t.mid(1);
    else if (t.endsWith('x') || t.endsWith('*')) t.chop(1);
    else return false;
    bool ok = false;
    n = t.trimmed().toInt(&ok);
    return ok && n >= 1 && n <= 1000;
}

Color axisColorFor(const Vec3& n) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(n.dot(kAxes[i])) > 1 - 1e-6) return Renderer::axisColor(i);
    return Color::rgb8(220, 0, 220);
}

} // namespace

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

void expandCurves(const Mesh& m, std::set<Id>& edges) {
    std::set<Id> curves;
    for (Id e : edges)
        if (m.edges[e].alive && m.edges[e].curve != kNone) curves.insert(m.edges[e].curve);
    if (curves.empty()) return;
    for (Id e = 0; e < (Id)m.edges.size(); ++e)
        if (m.edges[e].alive && curves.count(m.edges[e].curve)) edges.insert(e);
}

std::set<Id> selectionVertices(const Mesh& m, const Selection& s) {
    std::set<Id> vs;
    for (Id f : s.faces)
        if (m.faces[f].alive)
            for (Id v : m.faceVertices(f)) vs.insert(v);
    for (Id e : s.edges)
        if (m.edges[e].alive) {
            vs.insert(m.edges[e].v[0]);
            vs.insert(m.edges[e].v[1]);
        }
    return vs;
}

void selectConnected(const Mesh& m, Selection& s) {
    std::set<Id> verts = selectionVertices(m, s);
    std::vector<Id> stack(verts.begin(), verts.end());
    while (!stack.empty()) {
        Id v = stack.back();
        stack.pop_back();
        for (Id e : m.vertexEdges(v)) {
            if (!m.edges[e].alive) continue;
            s.edges.insert(e);
            for (Id f : m.edgeFaces(e))
                if (m.faces[f].alive) s.faces.insert(f);
            Id o = m.edges[e].other(v);
            if (verts.insert(o).second) stack.push_back(o);
        }
    }
}

// ===========================================================================
// Select
// ===========================================================================

QString SelectTool::status() const {
    if (vp_->doc().editingComponent())
        return QStringLiteral("Editing component: changes apply to every copy. Click outside it or press Esc to close it.");
    return QStringLiteral("Select objects. Ctrl = add, Shift = add/subtract, Ctrl+Shift = subtract. Drag to select multiple; double-click a face to include its edges, triple-click for connected geometry, double-click a component to edit it.");
}

void SelectTool::applyPick(const Selection& picked, Qt::KeyboardModifiers mods) {
    Selection& s = doc().selection;
    bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
    if (ctrl && shift) {
        for (Id f : picked.faces) s.faces.erase(f);
        for (Id e : picked.edges) s.edges.erase(e);
        for (Id i : picked.instances) s.instances.erase(i);
    } else if (ctrl) {
        s.faces.insert(picked.faces.begin(), picked.faces.end());
        s.edges.insert(picked.edges.begin(), picked.edges.end());
        s.instances.insert(picked.instances.begin(), picked.instances.end());
    } else if (shift) {
        for (Id f : picked.faces)
            if (!s.faces.erase(f)) s.faces.insert(f);
        for (Id e : picked.edges)
            if (!s.edges.erase(e)) s.edges.insert(e);
        for (Id i : picked.instances)
            if (!s.instances.erase(i)) s.instances.insert(i);
    } else {
        s = picked;
    }
    doc().notifySelection();
    update();
}

void SelectTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    // Triple click: a press shortly after a double click.
    if (dblTimer_.isValid() && dblTimer_.elapsed() < QApplication::doubleClickInterval() &&
        std::hypot(e.x - dblPos_.x(), e.y - dblPos_.y()) < 6) {
        dblTimer_.invalidate();
        PickResult pr = engine().pick(e.x, e.y);
        if (pr.face != kNone || pr.edge != kNone) {
            Selection s;
            if (pr.face != kNone) s.faces.insert(pr.face);
            if (pr.edge != kNone) s.edges.insert(pr.edge);
            selectConnected(mesh(), s);
            applyPick(s, e.mods);
        }
        return;
    }
    pressed_ = true;
    dragging_ = false;
    pressPos_ = curPos_ = QPointF(e.x, e.y);
}

void SelectTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!pressed_) return;
    curPos_ = QPointF(e.x, e.y);
    if (!dragging_ && (curPos_ - pressPos_).manhattanLength() > 4) dragging_ = true;
    if (dragging_) update();
}

void SelectTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !pressed_) return;
    pressed_ = false;
    if (dragging_) {
        dragging_ = false;
        rectSelect(e.mods);
        update();
        return;
    }
    PickResult pr = engine().pick(e.x, e.y);
    Selection s;
    if (pr.instance != kNone) {
        s.instances.insert(pr.instance);
    } else if (pr.edge != kNone) {
        s.edges.insert(pr.edge);
        expandCurves(mesh(), s.edges);
    } else if (pr.face != kNone) {
        s.faces.insert(pr.face);
    }
    if (s.empty() && (e.mods & (Qt::ControlModifier | Qt::ShiftModifier))) return;
    if (s.empty() && doc().editingComponent()) {
        // Clicking outside the open component closes it.
        doc().closeComponent();
        refreshStatus();
        update();
        return;
    }
    applyPick(s, e.mods);
}

void SelectTool::mouseDoubleClick(const MouseEvent& e) {
    dblTimer_.start();
    dblPos_ = QPointF(e.x, e.y);
    PickResult pr = engine().pick(e.x, e.y);
    if (pr.instance != kNone) {
        // Edit the component in place.
        doc().openComponent(pr.instance);
        dblTimer_.invalidate();
        refreshStatus();
        update();
        return;
    }
    Selection s;
    const Mesh& m = mesh();
    if (pr.edge != kNone) {
        s.edges.insert(pr.edge);
        expandCurves(m, s.edges);
        for (Id ed : std::set<Id>(s.edges))
            for (Id f : m.edgeFaces(ed))
                if (m.faces[f].alive) s.faces.insert(f);
    } else if (pr.face != kNone) {
        s.faces.insert(pr.face);
        for (Id ed : m.faceEdges(pr.face)) s.edges.insert(ed);
    } else {
        return;
    }
    applyPick(s, e.mods);
}

void SelectTool::rectSelect(Qt::KeyboardModifiers mods) {
    const Mesh& m = mesh();
    QRectF r = QRectF(pressPos_, curPos_).normalized();
    bool crossing = curPos_.x() < pressPos_.x();
    std::vector<QPointF> sp(m.vertices.size());
    std::vector<char> ok(m.vertices.size(), 0);
    for (Id v = 0; v < (Id)m.vertices.size(); ++v) {
        if (!m.vertices[v].alive) continue;
        double x, y;
        if (cam().project(m.vertices[v].p, x, y)) {
            sp[v] = QPointF(x, y);
            ok[v] = 1;
        }
    }
    auto inside = [&](Id v) { return ok[v] && r.contains(sp[v]); };
    auto segHits = [&](Id a, Id b) {
        if (!ok[a] || !ok[b]) return false;
        if (inside(a) || inside(b)) return true;
        QLineF seg(sp[a], sp[b]);
        QLineF sides[4] = {QLineF(r.topLeft(), r.topRight()), QLineF(r.topRight(), r.bottomRight()),
                           QLineF(r.bottomRight(), r.bottomLeft()), QLineF(r.bottomLeft(), r.topLeft())};
        for (const auto& s : sides)
            if (seg.intersects(s, nullptr) == QLineF::BoundedIntersection) return true;
        return false;
    };
    Selection s;
    for (Id e = 0; e < (Id)m.edges.size(); ++e) {
        const Edge& E = m.edges[e];
        if (!E.alive) continue;
        if (crossing ? segHits(E.v[0], E.v[1]) : (inside(E.v[0]) && inside(E.v[1]))) s.edges.insert(e);
    }
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        const Face& F = m.faces[f];
        if (!F.alive || F.hidden) continue;
        bool sel;
        if (!crossing) {
            sel = true;
            for (Id v : F.outer)
                if (!inside(v)) { sel = false; break; }
        } else {
            sel = false;
            for (size_t i = 0; i < F.outer.size() && !sel; ++i)
                sel = segHits(F.outer[i], F.outer[(i + 1) % F.outer.size()]);
            if (!sel) {
                Ray ray = cam().ray(r.center().x(), r.center().y());
                double t;
                sel = m.rayFace(f, ray, t);
            }
        }
        if (sel) s.faces.insert(f);
    }
    const ModelState& st = doc().state;
    for (Id i = 0; i < (Id)st.instances.size() && st.editing == kNone; ++i) {
        const Instance& I = st.instances[i];
        Vec3 k[8];
        if (!I.alive || I.hidden || !instanceBoxCorners(st, i, k)) continue;
        QRectF box;
        bool all = true, ok = true;
        for (const Vec3& p : k) {
            double x, y;
            if (!cam().project(p, x, y)) { ok = false; break; }
            all = all && r.contains(QPointF(x, y));
            box = box.isNull() ? QRectF(QPointF(x, y), QSizeF(1e-6, 1e-6)) : box.united(QRectF(QPointF(x, y), QSizeF(1e-6, 1e-6)));
        }
        if (ok && (crossing ? r.intersects(box) : all)) s.instances.insert(i);
    }
    applyPick(s, mods);
}

void SelectTool::paintOverlay(QPainter& p) {
    if (!dragging_) return;
    QRectF r = QRectF(pressPos_, curPos_).normalized();
    bool crossing = curPos_.x() < pressPos_.x();
    QPen pen(QColor(0, 0, 0), 1, crossing ? Qt::DashLine : Qt::SolidLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

// ===========================================================================
// Eraser
// ===========================================================================

QString EraserTool::status() const {
    return QStringLiteral("Select objects to delete, or drag over them. Shift = hide, Ctrl = soften/smooth, Ctrl+Shift = unsoften.");
}

void EraserTool::markAt(const MouseEvent& e) {
    Id ed = engine().pickEdge(e.x, e.y, 7.0);
    if (ed == kNone && (e.ctrl() && e.shift())) {
        // Allow picking soft edges when unsoftening.
        double best = 7.0;
        const Mesh& m = mesh();
        for (Id i = 0; i < (Id)m.edges.size(); ++i) {
            if (!m.edges[i].alive || !m.edges[i].soft) continue;
            Vec3 a = m.pos(m.edges[i].v[0]), b = m.pos(m.edges[i].v[1]);
            Ray r = cam().ray(e.x, e.y);
            double s, t;
            Vec3 q = closestLineLine(a, b - a, r.origin, r.dir, s, t) ? lerp(a, b, std::clamp(s, 0.0, 1.0)) : a;
            double d = engine().pixelDistance(q, e.x, e.y);
            if (d < best) { best = d; ed = i; }
        }
    }
    if (ed != kNone) marked_.insert(ed);
    else if (Id inst = engine().pickInstance(e.x, e.y); inst != kNone && !(e.ctrl())) markedInstances_.insert(inst);
    else {
        int g = engine().pickGuide(e.x, e.y, 6.0);
        if (g >= 0) markedGuides_.insert(g);
    }
}

void EraserTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    erasing_ = true;
    marked_.clear();
    markedInstances_.clear();
    markedGuides_.clear();
    markAt(e);
    update();
}

void EraserTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (erasing_) markAt(e);
    else hover_ = engine().pickEdge(e.x, e.y, 7.0);
    update();
}

void EraserTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !erasing_) return;
    erasing_ = false;
    if (marked_.empty() && markedGuides_.empty() && markedInstances_.empty()) return;
    Mesh& m = mesh();
    if (e.ctrl() && e.shift()) {
        doc().beginOp("Unsoften");
        for (Id ed : marked_) m.edges[ed].soft = m.edges[ed].smooth = false;
        m.touch();
    } else if (e.ctrl()) {
        doc().beginOp("Soften");
        for (Id ed : marked_) m.edges[ed].soft = m.edges[ed].smooth = true;
        m.touch();
    } else if (e.shift()) {
        doc().beginOp("Hide");
        for (Id ed : marked_) m.edges[ed].hidden = true;
        for (Id i : markedInstances_) doc().state.instances[i].hidden = true;
        m.touch();
    } else {
        doc().beginOp("Erase");
        for (Id ed : marked_) m.eraseEdge(ed, true);
        for (Id i : markedInstances_) doc().state.instances[i].alive = false;
        std::vector<int> gs(markedGuides_.rbegin(), markedGuides_.rend());
        for (int g : gs)
            if (g < (int)doc().state.guides.size()) doc().state.guides.erase(doc().state.guides.begin() + g);
        m.touch();
    }
    doc().commitOp();
    marked_.clear();
    markedInstances_.clear();
    markedGuides_.clear();
    update();
}

void EraserTool::buildPreview(Preview& pv) {
    if (erasing_) {
        pv.highlightEdges = marked_;
        pv.highlightInstances = markedInstances_;
    } else if (hover_ != kNone) {
        pv.highlightEdges.insert(hover_);
    }
    for (int g : markedGuides_) {
        if (g >= (int)doc().state.guides.size()) continue;
        const Guide& gd = doc().state.guides[g];
        if (gd.kind == Guide::Line) pv.line(gd.p - gd.dir * 1e4, gd.p + gd.dir * 1e4, Color::rgb8(0, 0, 255), 1.5f, Dash::Dashed, true);
    }
}

// ===========================================================================
// Paint bucket
// ===========================================================================

QString PaintTool::status() const {
    return QStringLiteral("Select face to paint. Ctrl = fill connected, Shift = replace all matching, Alt = sample material. Drop image files onto faces to apply them as textures.");
}

void PaintTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    hover_ = engine().pickFace(e.x, e.y);
    update();
}

void PaintTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    Mesh& m = mesh();
    double t;
    Id f = engine().pickFace(e.x, e.y, &t);
    if (f == kNone) return;
    Ray r = cam().ray(e.x, e.y);
    bool front = m.faces[f].normal.dot(r.dir) < 0;
    if (e.alt()) {
        doc().currentMaterial = front ? m.faces[f].front.material : m.faces[f].back.material;
        vp_->materialSampled();
        return;
    }
    const Id mat = doc().currentMaterial;
    const Id old = front ? m.faces[f].front.material : m.faces[f].back.material;
    std::vector<std::pair<Id, bool>> targets;
    if (doc().selection.faces.count(f)) {
        for (Id g : doc().selection.faces) {
            bool fr = m.faces[g].normal.dot(cam().eye - m.faceInteriorPoint(g)) > 0 || !cam().perspective;
            if (!cam().perspective) fr = m.faces[g].normal.dot(cam().forward()) < 0;
            targets.push_back({g, fr});
        }
    } else if (e.shift()) {
        for (Id g = 0; g < (Id)m.faces.size(); ++g) {
            if (!m.faces[g].alive) continue;
            if (m.faces[g].front.material == old) targets.push_back({g, true});
            if (m.faces[g].back.material == old) targets.push_back({g, false});
        }
    } else if (e.ctrl()) {
        // Flood fill across edges, same material on the same side.
        std::set<Id> seen = {f};
        std::vector<Id> stack = {f};
        while (!stack.empty()) {
            Id g = stack.back();
            stack.pop_back();
            targets.push_back({g, front});
            for (Id ed : m.faceEdges(g))
                for (Id h : m.edgeFaces(ed)) {
                    if (!m.faces[h].alive || seen.count(h)) continue;
                    Id hm = front ? m.faces[h].front.material : m.faces[h].back.material;
                    if (hm != old) continue;
                    seen.insert(h);
                    stack.push_back(h);
                }
        }
    } else {
        targets.push_back({f, front});
    }
    doc().beginOp("Paint");
    for (auto [g, fr] : targets) {
        FaceSide& s = fr ? m.faces[g].front : m.faces[g].back;
        if (s.material != mat) s.tex.valid = false;
        s.material = mat;
    }
    m.touch();
    doc().commitOp();
    update();
}

void PaintTool::buildPreview(Preview& pv) {
    if (hover_ != kNone) pv.highlightFaces.insert(hover_);
}

// ===========================================================================
// Follow Me
// ===========================================================================

QString FollowMeTool::status() const {
    QString s;
    if (!selectedPath_.empty()) s = QStringLiteral("Click the profile face to sweep along the selected path.");
    else if (profile_ == kNone) s = QStringLiteral("Click the profile face, or select the path edges first.");
    else s = QStringLiteral("Click a path edge to follow its chain. Alt = follow a face's outline. Esc = cancel.");
    return message_.isEmpty() ? s : message_ + QStringLiteral("  ") + s;
}

void FollowMeTool::activate() {
    const Selection& sel = doc().selection;
    selectedPath_ = sel.edges;
    for (Id f : sel.faces)
        for (Id e : mesh().faceEdges(f)) selectedPath_.insert(e);
    refreshStatus();
}

void FollowMeTool::reset() {
    profile_ = hoverFace_ = kNone;
    hoverPath_.clear();
    message_.clear();
    refreshStatus();
    update();
}

std::set<Id> FollowMeTool::pathUnder(const MouseEvent& e) const {
    Mesh& m = const_cast<FollowMeTool*>(this)->mesh();
    InferenceEngine& eng = const_cast<FollowMeTool*>(this)->engine();
    if (e.alt()) {
        Id f = eng.pickFace(e.x, e.y);
        if (f == kNone || f == profile_) return {};
        std::vector<Id> es = m.faceEdges(f);
        return {es.begin(), es.end()};
    }
    Id edge = eng.pickEdge(e.x, e.y, 6.0);
    if (edge == kNone) return {};
    std::vector<Id> chain = edgeChain(m, edge);
    return {chain.begin(), chain.end()};
}

void FollowMeTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (profile_ == kNone) {
        hoverFace_ = engine().pickFace(e.x, e.y);
        hoverPath_.clear();
    } else {
        hoverPath_ = pathUnder(e);
    }
    update();
}

void FollowMeTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (profile_ == kNone) {
        Id f = engine().pickFace(e.x, e.y);
        if (f == kNone) return;
        profile_ = f;
        message_.clear();
        if (!selectedPath_.empty()) {
            commit(selectedPath_);
            return;
        }
        refreshStatus();
        update();
        return;
    }
    std::set<Id> path = pathUnder(e);
    if (!path.empty()) commit(path);
}

void FollowMeTool::commit(const std::set<Id>& path) {
    std::string err;
    doc().beginOp("Follow Me");
    bool ok = followMe(mesh(), profile_, path, &err);
    if (ok) {
        doc().selection.clear();
        doc().commitOp();
        doc().notifySelection();
        selectedPath_.clear();
    } else {
        doc().cancelOp();
    }
    profile_ = hoverFace_ = kNone;
    hoverPath_.clear();
    message_ = ok ? QString() : QString::fromStdString(err);
    refreshStatus();
    update();
}

void FollowMeTool::buildPreview(Preview& pv) {
    const Mesh& m = mesh();
    if (profile_ != kNone && profile_ < (Id)m.faces.size() && m.faces[profile_].alive) pv.highlightFaces.insert(profile_);
    else if (hoverFace_ != kNone) pv.highlightFaces.insert(hoverFace_);
    for (Id e : hoverPath_)
        if (e >= 0 && e < (Id)m.edges.size() && m.edges[e].alive) pv.highlightEdges.insert(e);
}

// ===========================================================================
// Push / Pull
// ===========================================================================

QString PushPullTool::status() const {
    if (!pulling_) return QStringLiteral("Pick a face to push or pull. Ctrl = create new starting face. Double-click repeats the last distance.");
    return QStringLiteral("Move to set the distance, click a point to push/pull to it, or type a distance.");
}

void PushPullTool::reset() {
    pulling_ = dragged_ = false;
    face_ = kNone;
    clearLocks();
    refreshStatus();
    update();
}

void PushPullTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!pulling_) {
        double t;
        Id f = engine().pickFace(e.x, e.y, &t);
        if (f == kNone) return;
        face_ = f;
        start_ = cam().ray(e.x, e.y).at(t);
        normal_ = mesh().faces[f].normal;
        dist_ = 0;
        pulling_ = true;
        dragged_ = false;
        pressX_ = e.x;
        pressY_ = e.y;
        faceVerts_.clear();
        for (Id v : mesh().faceVertices(f)) faceVerts_.insert(v);
        refreshStatus();
        update();
        return;
    }
    commit(dist_, e.ctrl());
}

void PushPullTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!pulling_) {
        hover_ = engine().pickFace(e.x, e.y);
        update();
        return;
    }
    if (std::hypot(e.x - pressX_, e.y - pressY_) > 6) dragged_ = true;
    InferenceEngine::Options o;
    o.ref = start_;
    o.lockDir = normal_;
    o.lockAxis = -1;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(normal_.dot(kAxes[i])) > 1 - 1e-9) o.lockAxis = i;
    o.excludeVerts = &faceVerts_;
    inf_ = engine().infer(e.x, e.y, o);
    dist_ = (inf_.p - start_).dot(normal_);
    setVcb(fmtLen(dist_));
    update();
}

void PushPullTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (pulling_ && dragged_) commit(dist_, e.ctrl());
}

void PushPullTool::mouseDoubleClick(const MouseEvent& e) {
    if (pulling_ || std::fabs(lastDist_) < kTol) return;
    Id f = engine().pickFace(e.x, e.y);
    if (f == kNone) return;
    face_ = f;
    commit(lastDist_, e.ctrl());
}

void PushPullTool::commit(double dist, bool keep) {
    pulling_ = dragged_ = false;
    refreshStatus();
    if (face_ == kNone || std::fabs(dist) < kTol) { update(); return; }
    lastFace_ = face_;
    lastKeep_ = keep;
    doc().beginOp("Push/Pull");
    mesh().pushPull(face_, dist, keep);
    doc().commitOp();
    lastDist_ = dist;
    canAdjust_ = true;
    face_ = kNone;
    setVcb(fmtLen(dist));
    update();
}

bool PushPullTool::vcbEnter(const QString& text) {
    double d;
    if (!parseLen(text, d)) return false;
    // The typed value follows the current drag direction; a negative value reverses it.
    if (pulling_) {
        commit(dist_ < 0 ? -d : d, lastMouse_.ctrl());
        return true;
    }
    if (canAdjust_ && doc().undoName() == "Push/Pull") {
        // Re-do the last push/pull with the typed distance.
        doc().undo();
        face_ = lastFace_;
        if (face_ >= 0 && face_ < (Id)mesh().faces.size() && mesh().faces[face_].alive) {
            commit(lastDist_ < 0 ? -d : d, lastKeep_);
            return true;
        }
    }
    return false;
}

void PushPullTool::buildPreview(Preview& pv) {
    if (!pulling_) {
        if (hover_ != kNone) pv.highlightFaces.insert(hover_);
        return;
    }
    const Mesh& m = mesh();
    if (face_ < 0 || !m.faces[face_].alive) return;
    pv.highlightFaces.insert(face_);
    Vec3 off = normal_ * dist_;
    const Face& F = m.faces[face_];
    std::vector<std::vector<Id>> loops = {F.outer};
    for (const auto& h : F.holes) loops.push_back(h);
    for (const auto& L : loops) {
        std::vector<Vec3> ring;
        for (size_t i = 0; i < L.size(); ++i) {
            Vec3 a = m.pos(L[i]) + off, b = m.pos(L[(i + 1) % L.size()]) + off;
            pv.line(a, b, Color::rgb8(0, 0, 0), 1.4f);
            pv.line(m.pos(L[i]), a, Color::rgb8(0, 0, 0), 1.0f);
            ring.push_back(a);
        }
        if (&L == &loops[0]) pv.polys.push_back({ring, Color::rgb8(90, 90, 200, 60)});
    }
    if (inf_.isSnap() || inf_.type == InferenceResult::OnEdge || inf_.type == InferenceResult::OnFace) engine().addPreview(inf_, pv);
    pv.line(start_, start_ + off, Color::rgb8(220, 0, 220), 1.0f, Dash::Dotted);
}

// ===========================================================================
// Transform tools (move / rotate / scale)
// ===========================================================================

void TransformTool::reset() {
    if (active_) cancelLive();
    active_ = false;
    clearLocks();
    refreshStatus();
    update();
}

bool TransformTool::keyPress(QKeyEvent* e) {
    if (e->key() == Qt::Key_Control && !e->isAutoRepeat()) {
        if (active_) toggleCopy();
        else copy_ = !copy_;
        refreshStatus();
        update();
        return true;
    }
    return Tool::keyPress(e);
}

bool TransformTool::keyRelease(QKeyEvent* e) { return Tool::keyRelease(e); }

bool TransformTool::ensureSelection(const MouseEvent& e) {
    if (!doc().selection.empty()) return true;
    PickResult pr = engine().pick(e.x, e.y);
    Selection s;
    if (pr.instance != kNone) {
        s.instances.insert(pr.instance);
    } else if (pr.edge != kNone) {
        s.edges.insert(pr.edge);
        expandCurves(mesh(), s.edges);
    } else if (pr.face != kNone) {
        s.faces.insert(pr.face);
    }
    if (s.empty()) return false;
    doc().selection = s;
    doc().notifySelection();
    return true;
}

bool TransformTool::begin(const char* opName, bool copy) {
    Mesh& m = mesh();
    origSelection_ = doc().selection;
    opName_ = opName;
    copy_ = copy;
    doc().beginOp(copy ? std::string("Copy") : std::string(opName));
    if (copy) {
        std::set<Id> nf, ne, nv;
        m.duplicate(doc().selection.faces, doc().selection.edges, nf, ne, nv);
        doc().selection.faces = nf;
        doc().selection.edges = ne;
        verts_ = nv;
        // Copies of instances share the definition: editing one edits all.
        std::set<Id> ni;
        for (Id i : doc().selection.instances) {
            Id c = cloneInstance(doc().state, i);
            if (c != kNone) ni.insert(c);
        }
        doc().selection.instances = ni;
        doc().notifySelection();
    } else {
        verts_ = selectionVertices(m, doc().selection);
    }
    orig_.clear();
    for (Id v : verts_) orig_[v] = m.pos(v);
    instOrig_.clear();
    for (Id i : doc().selection.instances) instOrig_[i] = doc().state.instances[i].xf;
    engine().ignoredInstances = doc().selection.instances;
    affectedFaces_.clear();
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        if (!m.faces[f].alive) continue;
        for (Id v : m.faceVertices(f))
            if (verts_.count(v)) { affectedFaces_.push_back(f); break; }
    }
    active_ = !verts_.empty() || !instOrig_.empty();
    if (!active_) {
        doc().cancelOp();
        engine().ignoredInstances.clear();
    }
    return active_;
}

void TransformTool::applyLive(const Mat4& mt) {
    Mesh& m = mesh();
    for (auto& [v, p] : orig_) m.vertices[v].p = mt.transformPoint(p);
    for (Id f : affectedFaces_) m.recomputeNormal(f);
    for (auto& [i, xf] : instOrig_) doc().state.instances[i].xf = mt * xf;
    m.markGeometryMoved();
}

void TransformTool::finish(const Mat4& mt) {
    Mesh& m = mesh();
    for (auto& [v, p] : orig_) m.vertices[v].p = p;
    m.markGeometryMoved();
    m.transformVertices(verts_, mt, true);
    for (auto& [i, xf] : instOrig_) doc().state.instances[i].xf = mt * xf;
    engine().ignoredInstances.clear();
    doc().commitOp();
    if (placing_) doc().commitOp(); // the paste itself
    placing_ = false;
    active_ = false;
    lastWasCopy_ = copy_;
    lastSourceSel_ = origSelection_;
    lastTransform_ = mt;
    copy_ = false;
    doc().validateSelection();
    clearLocks();
    refreshStatus();
}

void TransformTool::cancelLive() {
    engine().ignoredInstances.clear();
    doc().cancelOp();
    if (placing_) doc().cancelOp(); // drop the pasted geometry too
    placing_ = false;
    doc().selection = origSelection_;
    doc().validateSelection();
    doc().notifySelection();
    active_ = false;
}

void TransformTool::toggleCopy() {
    if (!active_ || placing_) return;
    Mat4 mt = currentTransform();
    std::string name = opName_;
    bool newCopy = !copy_;
    cancelLive();
    begin(name.c_str(), newCopy);
    applyLive(mt);
}

void TransformTool::makeArray(const Mat4& step, int n) {
    if (!lastWasCopy_ || doc().undoName() != "Copy") return;
    doc().undo();
    doc().selection = lastSourceSel_;
    doc().validateSelection();
    Mesh& m = mesh();
    doc().beginOp("Array");
    Mat4 acc;
    Selection all;
    for (int k = 1; k <= n; ++k) {
        acc = step * acc;
        std::set<Id> nf, ne, nv;
        m.duplicate(lastSourceSel_.faces, lastSourceSel_.edges, nf, ne, nv);
        m.transformVertices(nv, acc, true);
        all.faces.insert(nf.begin(), nf.end());
        all.edges.insert(ne.begin(), ne.end());
        for (Id i : lastSourceSel_.instances) {
            Id c = cloneInstance(doc().state, i);
            if (c == kNone) continue;
            doc().state.instances[c].xf = acc * doc().state.instances[c].xf;
            all.instances.insert(c);
        }
    }
    doc().selection = all;
    doc().commitOp();
    doc().notifySelection();
    lastWasCopy_ = false;
    update();
}

// ---- Move ------------------------------------------------------------------

QString MoveTool::status() const {
    if (active_ && placing_) return QStringLiteral("Click to place the pasted geometry, or type a distance. Esc cancels the paste.");
    if (!active_) return copy_ ? QStringLiteral("Copy: pick the base point. Ctrl toggles copy.")
                               : QStringLiteral("Pick the base point to move. Ctrl = copy. After copying, type e.g. 5x or /5 to make an array.");
    return QStringLiteral("Pick the destination or type a distance. Arrow keys lock axes, Shift locks the inference.");
}

bool MoveTool::startPlacement(const Vec3& base, double x, double y) {
    base_ = base;
    delta_ = Vec3();
    if (!begin("Paste", false)) return false;
    placing_ = true;
    dragged_ = false;
    pressX_ = x;
    pressY_ = y;
    MouseEvent e;
    e.x = x;
    e.y = y;
    lastMouse_ = e;
    mouseMove(e); // jump to the cursor right away
    refreshStatus();
    return true;
}

void MoveTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!active_) {
        InferenceResult r = inferPoint(e, std::nullopt);
        if (!ensureSelection(e)) return;
        base_ = r.p;
        delta_ = Vec3();
        if (!begin("Move", copy_)) return;
        dragged_ = false;
        pressX_ = e.x;
        pressY_ = e.y;
        refreshStatus();
        update();
        return;
    }
    finish(currentTransform());
    update();
}

void MoveTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!active_) {
        inferPoint(e, std::nullopt);
        hoverFace_ = hoverEdge_ = hoverInstance_ = kNone;
        if (doc().selection.empty()) {
            PickResult pr = engine().pick(e.x, e.y);
            hoverInstance_ = pr.instance;
            hoverEdge_ = pr.edge;
            hoverFace_ = pr.edge == kNone ? pr.face : kNone;
        }
        update();
        return;
    }
    if (std::hypot(e.x - pressX_, e.y - pressY_) > 6) dragged_ = true;
    InferenceResult r = inferPoint(e, base_, std::nullopt, &verts_);
    delta_ = r.p - base_;
    applyLive(currentTransform());
    setVcb(fmtLen(delta_.length()));
    update();
}

void MoveTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    if (active_ && dragged_) {
        finish(currentTransform());
        update();
    }
}

bool MoveTool::vcbEnter(const QString& text) {
    int n;
    bool divide;
    if (!active_ && parseArray(text, n, divide)) {
        if (!lastWasCopy_) return false;
        Vec3 d(lastTransform_.at(0, 3), lastTransform_.at(1, 3), lastTransform_.at(2, 3));
        if (divide) makeArray(Mat4::translation(d / n), n);
        else makeArray(Mat4::translation(d), n);
        return true;
    }
    double len;
    if (!parseLen(text, len)) {
        Vec3 c;
        QString t = text.trimmed();
        if (active_ && t.startsWith('<') && t.endsWith('>')) {
            QStringList p = t.mid(1, t.size() - 2).split(QRegularExpression("[;,]"));
            if (p.size() == 3) {
                bool ok = true;
                for (int i = 0; i < 3; ++i) {
                    double v = 0;
                    ok = ok && parseLen(p[i], v);
                    c[i] = v;
                }
                if (ok) {
                    delta_ = c;
                    finish(currentTransform());
                    update();
                    return true;
                }
            }
        }
        return false;
    }
    if (!active_) {
        // Adjust the last move distance.
        if (doc().undoName() != "Move" && doc().undoName() != "Copy") return false;
        Vec3 d(lastTransform_.at(0, 3), lastTransform_.at(1, 3), lastTransform_.at(2, 3));
        if (d.length() < kTol) return false;
        bool wasCopy = lastWasCopy_;
        Selection src = lastSourceSel_;
        doc().undo();
        doc().selection = src;
        doc().validateSelection();
        if (!begin("Move", wasCopy)) return false;
        delta_ = d.normalized() * len;
        finish(currentTransform());
        update();
        return true;
    }
    Vec3 dir = delta_.length() > kTol ? delta_ : (lockDir_ ? *lockDir_ : Vec3(1, 0, 0));
    delta_ = dir.normalized() * len;
    finish(currentTransform());
    update();
    return true;
}

void MoveTool::buildPreview(Preview& pv) {
    if (!active_) {
        if (hoverInstance_ != kNone) pv.highlightInstances.insert(hoverInstance_);
        if (hoverFace_ != kNone) pv.highlightFaces.insert(hoverFace_);
        if (hoverEdge_ != kNone) {
            std::set<Id> es = {hoverEdge_};
            expandCurves(mesh(), es);
            pv.highlightEdges = es;
        }
    } else {
        Vec3 t = base_ + delta_;
        pv.line(base_, t, hasLock() ? lockColor() : lineColor(base_, t), hasLock() ? 2.0f : 1.2f, Dash::Dashed);
    }
    engine().addPreview(inf_, pv);
}

// ---- Rotate ----------------------------------------------------------------

QString RotateTool::status() const {
    switch (stage_) {
    case 0: return QStringLiteral("Pick the rotation center. Arrow keys lock the rotation plane (Right=red, Left=green, Up=blue). Ctrl = copy.");
    case 1: return QStringLiteral("Pick the start of the rotation angle.");
    default: return QStringLiteral("Pick the end angle or type degrees. Snaps every 15°.");
    }
}

void RotateTool::reset() {
    TransformTool::reset();
    stage_ = 0;
    angle_ = 0;
}

bool RotateTool::keyPress(QKeyEvent* e) {
    int axis = -1;
    switch (e->key()) {
    case Qt::Key_Right: axis = 0; break;
    case Qt::Key_Left: axis = 1; break;
    case Qt::Key_Up: axis = 2; break;
    default: return TransformTool::keyPress(e);
    }
    if (stage_ >= 2) return true;
    forcedAxis_ = forcedAxis_ == axis ? -1 : axis;
    if (forcedAxis_ >= 0) normal_ = kAxes[forcedAxis_];
    mouseMove(lastMouse_);
    update();
    return true;
}

Vec3 RotateTool::planeNormalAt(const MouseEvent& e, const InferenceResult& r) {
    if (forcedAxis_ >= 0) return kAxes[forcedAxis_];
    Id f = r.face != kNone ? r.face : engine().pickFace(e.x, e.y);
    if (f != kNone) return mesh().faces[f].normal;
    return Vec3(0, 0, 1);
}

Mat4 RotateTool::currentTransform() const {
    return Mat4::translation(center_) * Mat4::rotation(normal_, angle_) * Mat4::translation(-center_);
}

void RotateTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (stage_ == 0) {
        if (!ensureSelection(e)) return;
        InferenceResult r = inferPoint(e, std::nullopt);
        center_ = r.p;
        normal_ = planeNormalAt(e, r);
        stage_ = 1;
    } else if (stage_ == 1) {
        Vec3 d = cur_ - center_;
        d = d - normal_ * d.dot(normal_);
        if (d.length() < kTol) return;
        refDir_ = d.normalized();
        angle_ = 0;
        if (!begin("Rotate", copy_)) { stage_ = 0; return; }
        stage_ = 2;
    } else {
        finish(currentTransform());
        stage_ = 0;
    }
    refreshStatus();
    update();
}

void RotateTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (stage_ == 0) {
        InferenceResult r = inferPoint(e, std::nullopt);
        normal_ = planeNormalAt(e, r);
        center_ = r.p;
    } else if (stage_ == 1) {
        InferenceResult r = inferPoint(e, center_, Plane(normal_, center_));
        cur_ = r.p;
    } else {
        InferenceResult r = inferPoint(e, center_, Plane(normal_, center_), &verts_, false);
        cur_ = r.p;
        Vec3 d = cur_ - center_;
        d = d - normal_ * d.dot(normal_);
        if (d.length() > kTol) {
            Vec3 v = d.normalized();
            angle_ = std::atan2(normal_.dot(refDir_.cross(v)), refDir_.dot(v));
            double deg = angle_ * 180 / kPi;
            double snap = std::round(deg / 15.0) * 15.0;
            snapped_ = std::fabs(deg - snap) < 2.5 && !r.isSnap();
            if (snapped_) angle_ = snap * kPi / 180;
        }
        applyLive(currentTransform());
        setVcb(QString::number(angle_ * 180 / kPi, 'f', 1) + QStringLiteral("°"));
    }
    update();
}

bool RotateTool::vcbEnter(const QString& text) {
    int n;
    bool divide;
    if (stage_ == 0 && parseArray(text, n, divide)) {
        if (!lastWasCopy_) return false;
        Mat4 step = divide ? Mat4::translation(center_) * Mat4::rotation(normal_, angle_ / n) * Mat4::translation(-center_)
                           : lastTransform_;
        makeArray(step, n);
        return true;
    }
    QString t = text.trimmed();
    t.remove(QStringLiteral("°"));
    double deg;
    if (!parseNumber(t.toStdString(), deg)) return false;
    if (stage_ == 2) {
        angle_ = deg * kPi / 180;
        finish(currentTransform());
        stage_ = 0;
        refreshStatus();
        update();
        return true;
    }
    return false;
}

void RotateTool::buildPreview(Preview& pv) {
    double r = cam().pixelSize(center_) * 55;
    Vec3 a, b;
    planeBasis(normal_, a, b);
    Color c = axisColorFor(normal_);
    for (int i = 0; i < 48; ++i) {
        double t0 = 2 * kPi * i / 48, t1 = 2 * kPi * (i + 1) / 48;
        pv.line(center_ + (a * std::cos(t0) + b * std::sin(t0)) * r, center_ + (a * std::cos(t1) + b * std::sin(t1)) * r, c, 1.4f);
    }
    for (int i = 0; i < 24; ++i) {
        double t = 2 * kPi * i / 24;
        Vec3 d = a * std::cos(t) + b * std::sin(t);
        double inner = (i % 6 == 0) ? 0.8 : 0.9;
        pv.line(center_ + d * (r * inner), center_ + d * r, c, 1.0f);
    }
    if (stage_ >= 1) pv.line(center_, stage_ == 1 ? cur_ : center_ + refDir_ * r * 1.3, Color::rgb8(0, 0, 0), 1.0f, Dash::Dashed);
    if (stage_ == 2) {
        Vec3 d = cur_ - center_;
        d = d - normal_ * d.dot(normal_);
        pv.line(center_, center_ + d.normalized() * std::max(d.length(), r * 1.3), snapped_ ? Color::rgb8(220, 0, 220) : Color::rgb8(0, 0, 0), 1.2f, Dash::Dashed);
    }
    engine().addPreview(inf_, pv);
}

// ---- Scale -----------------------------------------------------------------

QString ScaleTool::status() const {
    if (!active_) {
        if (vp_->doc().selection.empty()) return QStringLiteral("Select what to scale (or click it), then drag one of the grips.");
        return QStringLiteral("Drag a grip: corners scale uniformly, edge grips two axes, face grips one axis. Shift toggles uniform, Ctrl scales about the center.");
    }
    return QStringLiteral("Move to scale, click to finish, or type a factor (2, or 1,2 / 1,2,1 per axis). Shift = uniform, Ctrl = about center.");
}

void ScaleTool::reset() {
    TransformTool::reset();
    hover_ = -1;
    factor_ = Vec3(1, 1, 1);
}

ScaleTool::Box ScaleTool::selectionBox() const {
    const Document& d = vp_->doc();
    const Selection& sel = d.selection;
    const Mesh& m = d.mesh();
    Box b;
    if (sel.empty()) return b;
    if (sel.instances.size() == 1 && sel.faces.empty() && sel.edges.empty()) {
        // One component: its own axes.
        Id i = *sel.instances.begin();
        Vec3 c[8];
        if (instanceBoxCorners(d.state, i, c)) {
            Mat4 xf = instanceWorldXf(d.state, i);
            Vec3 e[3] = {c[1] - c[0], c[2] - c[0], c[4] - c[0]};
            Vec3 col[3] = {xf.transformDir(Vec3(1, 0, 0)), xf.transformDir(Vec3(0, 1, 0)), xf.transformDir(Vec3(0, 0, 1))};
            for (int k = 0; k < 3; ++k) {
                b.len[k] = e[k].length();
                b.ax[k] = (b.len[k] > kTol ? e[k] : col[k]).normalized();
            }
            // Keep the frame orthonormal even if the instance is sheared.
            b.ax[1] = (b.ax[1] - b.ax[0] * b.ax[1].dot(b.ax[0])).normalized();
            Vec3 z = b.ax[0].cross(b.ax[1]);
            b.ax[2] = z.dot(b.ax[2]) < 0 ? -z : z;
            b.o = c[0];
            b.valid = true;
            return b;
        }
    }
    Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
    bool any = false;
    auto grow = [&](const Vec3& p) {
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], p[k]);
            mx[k] = std::max(mx[k], p[k]);
        }
        any = true;
    };
    for (Id v : selectionVertices(m, sel)) grow(m.pos(v));
    for (Id i : sel.instances) {
        Vec3 a, c;
        if (!instanceBounds(d.state, i, a, c)) continue;
        grow(a);
        grow(c);
    }
    if (!any) return b;
    b.o = mn;
    for (int k = 0; k < 3; ++k) {
        b.ax[k] = kAxes[k];
        b.len[k] = mx[k] - mn[k];
    }
    b.valid = true;
    return b;
}

std::vector<ScaleTool::Grip> ScaleTool::grips() const {
    std::vector<Grip> out;
    if (!box_.valid) return out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) {
                Grip g = {i, j, k};
                if (g == Grip{1, 1, 1}) continue;
                bool ok = true;
                for (int a = 0; a < 3; ++a)
                    if (g[a] != 1 && box_.len[a] < kTol) ok = false; // flat axis: nothing to scale
                if (ok) out.push_back(g);
            }
    return out;
}

int ScaleTool::gripAt(double x, double y) const {
    auto gs = grips();
    int best = -1;
    double bestD = 9.0;
    for (int i = 0; i < (int)gs.size(); ++i) {
        double d = vp_->inference().pixelDistance(box_.at(gs[i]), x, y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

bool ScaleTool::uniformMode() const {
    int n = 0;
    for (int a = 0; a < 3; ++a) n += grip_[a] != 1;
    bool shift = mods_ & Qt::ShiftModifier;
    return (n == 3) != shift;
}

Vec3 ScaleTool::anchor() const {
    if (mods_ & Qt::ControlModifier) return box_.at({1, 1, 1});
    return box_.at({2 - grip_[0], 2 - grip_[1], 2 - grip_[2]});
}

Mat4 ScaleTool::currentTransform() const {
    Mat4 r; // box axes as columns
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) r.at(i, k) = box_.ax[k][i];
    Mat4 rt;
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) rt.at(k, i) = box_.ax[k][i];
    Vec3 a = anchor();
    return Mat4::translation(a) * r * Mat4::scale(factor_) * rt * Mat4::translation(-a);
}

QString ScaleTool::gripName(const Grip& g) const {
    static const char* names[3] = {"Red", "Green", "Blue"};
    int n = 0;
    QStringList parts;
    for (int a = 0; a < 3; ++a)
        if (g[a] != 1) {
            ++n;
            parts << QString::fromLatin1(names[a]);
        }
    bool uniform = (n == 3) != bool(mods_ & Qt::ShiftModifier);
    QString what = uniform ? QStringLiteral("Uniform") : parts.join('/');
    return what + ((mods_ & Qt::ControlModifier) ? QStringLiteral(" Scale about Center") : QStringLiteral(" Scale about Opposite Point"));
}

void ScaleTool::applyFactors(const Vec3& fIn) {
    Vec3 f = fIn;
    for (int k = 0; k < 3; ++k)
        if (std::fabs(f[k]) < 1e-3) f[k] = f[k] < 0 ? -1e-3 : 1e-3;
    factor_ = f;
}

void ScaleTool::updateScale(const MouseEvent& e) {
    Vec3 g = box_.at(grip_), a = anchor();
    std::vector<int> axes;
    for (int k = 0; k < 3; ++k)
        if (grip_[k] != 1) axes.push_back(k);
    InferenceResult inf = inferPoint(e, std::nullopt, std::nullopt, &verts_, false);
    const bool snap = inf.isSnap();
    Ray r = cam().ray(e.x, e.y);
    Vec3 f(1, 1, 1);
    if (uniformMode() || axes.size() == 1) {
        // Along the line from the anchor through the grip.
        Vec3 d = g - a;
        double s = 1, t;
        if (snap) s = (inf.p - a).dot(d) / d.length2();
        else if (!closestLineLine(a, d, r.origin, r.dir, s, t)) return;
        cur_ = a + d * s;
        if (uniformMode()) f = Vec3(s, s, s);
        else f[axes[0]] = s;
    } else {
        // In the plane of the two grip axes (or facing the camera for a corner).
        Vec3 n = axes.size() == 2 ? box_.ax[axes[0]].cross(box_.ax[axes[1]]) : cam().forward();
        Plane P(n, g);
        double t;
        Vec3 p;
        if (snap) p = inf.p;
        else if (intersectRayPlane(r, P, t)) p = r.at(t);
        else return;
        cur_ = p;
        for (int k : axes) {
            double den = (g - a).dot(box_.ax[k]);
            if (std::fabs(den) > 1e-12) f[k] = (p - a).dot(box_.ax[k]) / den;
        }
    }
    applyFactors(f);
    applyLive(currentTransform());
    QStringList vals;
    if (uniformMode()) vals << QString::number(factor_.x, 'g', 4);
    else
        for (int k : axes) vals << QString::number(factor_[k], 'g', 4);
    setVcb(vals.join(QStringLiteral(", ")));
}

bool ScaleTool::keyPress(QKeyEvent* e) {
    if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift) {
        mods_ |= e->key() == Qt::Key_Control ? Qt::ControlModifier : Qt::ShiftModifier;
        if (active_) updateScale(lastMouse_);
        refreshStatus();
        update();
        return true;
    }
    return Tool::keyPress(e);
}

bool ScaleTool::keyRelease(QKeyEvent* e) {
    if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift) {
        mods_ &= ~(e->key() == Qt::Key_Control ? Qt::ControlModifier : Qt::ShiftModifier);
        if (active_) updateScale(lastMouse_);
        update();
        return true;
    }
    return Tool::keyRelease(e);
}

void ScaleTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    mods_ = e.mods & (Qt::ControlModifier | Qt::ShiftModifier);
    if (active_) {
        lastCentered_ = mods_ & Qt::ControlModifier;
        finish(currentTransform());
        canAdjust_ = true;
        box_ = selectionBox();
        refreshStatus();
        update();
        return;
    }
    box_ = selectionBox();
    int gi = gripAt(e.x, e.y);
    if (gi < 0) {
        // Not on a grip: scale what was clicked instead (unless it is already selected).
        PickResult pr = engine().pick(e.x, e.y);
        const Selection& cur = doc().selection;
        bool inSel = (pr.instance != kNone && cur.instances.count(pr.instance)) ||
                     (pr.instance == kNone && pr.edge != kNone && cur.edges.count(pr.edge)) ||
                     (pr.instance == kNone && pr.edge == kNone && pr.face != kNone && cur.faces.count(pr.face));
        if (!inSel && (pr.instance != kNone || pr.edge != kNone || pr.face != kNone)) {
            doc().selection.clear();
            ensureSelection(e);
            box_ = selectionBox();
        }
        refreshStatus();
        update();
        return;
    }
    grip_ = grips()[gi];
    factor_ = Vec3(1, 1, 1);
    if (!begin("Scale", false)) return;
    lastBox_ = box_;
    lastGrip_ = grip_;
    dragged_ = false;
    pressX_ = e.x;
    pressY_ = e.y;
    cur_ = box_.at(grip_);
    refreshStatus();
    update();
}

void ScaleTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    mods_ = e.mods & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!active_) {
        box_ = selectionBox();
        hover_ = gripAt(e.x, e.y);
        inf_ = InferenceResult();
        update();
        return;
    }
    if (std::hypot(e.x - pressX_, e.y - pressY_) > 6) dragged_ = true;
    updateScale(e);
    update();
}

void ScaleTool::mouseRelease(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !active_ || !dragged_) return;
    lastCentered_ = mods_ & Qt::ControlModifier;
    finish(currentTransform());
    canAdjust_ = true;
    box_ = selectionBox();
    refreshStatus();
    update();
}

bool ScaleTool::vcbEnter(const QString& text) {
    QStringList parts = text.split(QRegularExpression("[;,]"));
    std::vector<double> v;
    for (const QString& p : parts) {
        double x;
        if (!parseNumber(p.trimmed().toStdString(), x) || std::fabs(x) < 1e-9) return false;
        v.push_back(x);
    }
    const Grip g = active_ ? grip_ : lastGrip_;
    std::vector<int> axes;
    for (int k = 0; k < 3; ++k)
        if (g[k] != 1) axes.push_back(k);
    if (v.size() != 1 && v.size() != 3 && v.size() != axes.size()) return false;
    if (!active_) {
        // Adjust the scale just made.
        if (!canAdjust_ || doc().undoName() != "Scale") return false;
        Selection src = lastSourceSel_;
        doc().undo();
        doc().selection = src;
        doc().validateSelection();
        doc().notifySelection();
        box_ = lastBox_;
        grip_ = lastGrip_;
        mods_ = lastCentered_ ? Qt::ControlModifier : Qt::NoModifier;
        if (!begin("Scale", false)) return false;
    }
    Vec3 f(1, 1, 1);
    if (v.size() == 3) f = Vec3(v[0], v[1], v[2]);
    else if (v.size() == 1 && uniformMode()) f = Vec3(v[0], v[0], v[0]);
    else if (v.size() == 1) for (int k : axes) f[k] = v[0];
    else for (size_t i = 0; i < axes.size(); ++i) f[axes[i]] = v[i];
    applyFactors(f);
    lastCentered_ = mods_ & Qt::ControlModifier;
    finish(currentTransform());
    canAdjust_ = true;
    box_ = selectionBox();
    refreshStatus();
    update();
    return true;
}

void ScaleTool::buildPreview(Preview& pv) {
    if (!box_.valid) return;
    using MK = Preview::MarkerKind;
    // Box (transformed live while scaling), with grips.
    Mat4 mt = active_ ? currentTransform() : Mat4();
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) c[i] = mt.transformPoint(box_.at({(i & 1) ? 2 : 0, (i & 2) ? 2 : 0, (i & 4) ? 2 : 0}));
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& ed : E) pv.line(c[ed[0]], c[ed[1]], Color::rgb8(220, 160, 0), 1.2f, Dash::None);
    auto gs = grips();
    for (int i = 0; i < (int)gs.size(); ++i) {
        bool hot = active_ ? gs[i] == grip_ : i == hover_;
        pv.markers.push_back({mt.transformPoint(box_.at(gs[i])), hot ? MK::GripHot : MK::Grip});
    }
    if (active_) {
        Vec3 a = anchor();
        pv.markers.push_back({a, MK::Plain});
        pv.line(a, cur_, Color::rgb8(0, 0, 0), 1.0f, Dash::Dotted);
        pv.labels.push_back({mt.transformPoint(box_.at(grip_)), gripName(grip_).toStdString()});
        if (inf_.isSnap()) engine().addPreview(inf_, pv);
    } else if (hover_ >= 0 && hover_ < (int)gs.size()) {
        pv.labels.push_back({box_.at(gs[hover_]), gripName(gs[hover_]).toStdString()});
    }
}

// ===========================================================================
// Offset
// ===========================================================================

QString OffsetTool::status() const {
    if (!active_) return QStringLiteral("Pick a face to offset its edges. Double-click repeats the last offset.");
    return QStringLiteral("Move to set the offset distance, click to finish, or type a distance.");
}

void OffsetTool::reset() {
    active_ = false;
    face_ = kNone;
    refreshStatus();
    update();
}

double OffsetTool::signedDist(Id f, const Vec3& pIn) const {
    const Mesh& m = vp_->doc().mesh();
    Plane P = m.facePlane(f);
    Vec3 p = P.project(pIn);
    double dmin = 1e300;
    for (Id e : m.faceEdges(f)) dmin = std::min(dmin, distancePointSegment(m.pos(m.edges[e].v[0]), m.pos(m.edges[e].v[1]), p));
    return m.pointInFace(f, p, false) ? dmin : -dmin;
}

std::vector<std::vector<Vec3>> OffsetTool::offsetLoops(Id f, double d) const {
    const Mesh& m = vp_->doc().mesh();
    const Face& F = m.faces[f];
    Vec3 u, v;
    planeBasis(F.normal, u, v);
    Plane P = m.facePlane(f);
    std::vector<std::vector<Id>> loops = {F.outer};
    for (const auto& h : F.holes) loops.push_back(h);
    std::vector<std::vector<Vec3>> out;
    for (const auto& L : loops) {
        size_t n = L.size();
        std::vector<Vec2> p2;
        for (Id id : L) p2.emplace_back(m.pos(id).dot(u), m.pos(id).dot(v));
        double a0 = signedArea2D((int)n, [&](int i) { return p2[i]; });
        std::vector<Vec2> q(n);
        for (size_t i = 0; i < n; ++i) {
            Vec2 pp = p2[(i + n - 1) % n], pc = p2[i], pn = p2[(i + 1) % n];
            Vec2 d1 = pc - pp, d2 = pn - pc;
            double l1 = d1.length(), l2 = d2.length();
            if (l1 < 1e-12 || l2 < 1e-12) { q[i] = pc; continue; }
            d1 = d1 * (1 / l1);
            d2 = d2 * (1 / l2);
            Vec2 n1(-d1.y, d1.x), n2(-d2.y, d2.x);
            Vec2 a1 = pc + n1 * d, a2 = pc + n2 * d;
            double cr = d1.cross(d2);
            Vec2 r;
            if (std::fabs(cr) < 1e-9) r = a1;
            else r = a1 + d1 * ((a2 - a1).cross(d2) / cr);
            if ((r - pc).length() > std::fabs(d) * 6) r = pc + (n1 + n2) * (d / std::max(1e-9, (n1 + n2).length()) * 1.0);
            q[i] = r;
        }
        double a1 = signedArea2D((int)n, [&](int i) { return q[i]; });
        if (a1 * a0 <= 0 || std::fabs(a1) < 1e-12) continue;
        std::vector<Vec3> l3;
        Vec3 base = P.n * P.d;
        for (const Vec2& p : q) l3.push_back(base + u * p.x + v * p.y);
        out.push_back(std::move(l3));
    }
    return out;
}

void OffsetTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!active_) {
        double t;
        Id f = engine().pickFace(e.x, e.y, &t);
        if (f == kNone) return;
        face_ = f;
        start_ = cam().ray(e.x, e.y).at(t);
        dist_ = 0;
        active_ = true;
        refreshStatus();
        return;
    }
    commit(dist_);
}

void OffsetTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!active_) {
        hover_ = engine().pickFace(e.x, e.y);
        update();
        return;
    }
    if (face_ < 0 || !mesh().faces[face_].alive) { reset(); return; }
    InferenceResult r = inferPoint(e, std::nullopt, mesh().facePlane(face_), nullptr, false);
    dist_ = signedDist(face_, r.p) - signedDist(face_, start_);
    setVcb(fmtLen(dist_));
    update();
}

void OffsetTool::mouseDoubleClick(const MouseEvent& e) {
    if (active_ || std::fabs(lastDist_) < kTol) return;
    Id f = engine().pickFace(e.x, e.y);
    if (f == kNone) return;
    face_ = f;
    commit(lastDist_);
}

void OffsetTool::commit(double d) {
    active_ = false;
    refreshStatus();
    if (face_ == kNone || std::fabs(d) < kTol) { update(); return; }
    auto loops = offsetLoops(face_, d);
    if (loops.empty()) { update(); return; }
    doc().beginOp("Offset");
    for (const auto& l : loops) mesh().drawPolyline(l, true);
    doc().commitOp();
    lastDist_ = d;
    face_ = kNone;
    update();
}

bool OffsetTool::vcbEnter(const QString& text) {
    double d;
    if (!active_ || !parseLen(text, d)) return false;
    commit(dist_ < 0 ? -std::fabs(d) : std::fabs(d));
    return true;
}

void OffsetTool::buildPreview(Preview& pv) {
    if (!active_) {
        if (hover_ != kNone) pv.highlightFaces.insert(hover_);
        return;
    }
    pv.highlightFaces.insert(face_);
    for (const auto& l : offsetLoops(face_, dist_))
        for (size_t i = 0; i < l.size(); ++i) pv.line(l[i], l[(i + 1) % l.size()], Color::rgb8(0, 0, 0), 1.5f);
}

// ===========================================================================
// Tape measure
// ===========================================================================

QString TapeTool::status() const {
    if (stage_ == 0)
        return QStringLiteral("Pick a point to measure from. Start on an edge to create a parallel guide line; double-click an edge for an infinite guide. Ctrl = measure without guides.");
    return QStringLiteral("Pick the end point. Type a length to place a guide. After measuring, type a value to resize the model.");
}

void TapeTool::reset() {
    stage_ = 0;
    clearLocks();
    refreshStatus();
    update();
}

void TapeTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (stage_ == 0) {
        InferenceResult r = inferPoint(e, std::nullopt);
        start_ = cur_ = r.p;
        startInf_ = r;
        stage_ = 1;
        refreshStatus();
        return;
    }
    InferenceResult r = inferPoint(e, start_);
    cur_ = r.p;
    double len = (cur_ - start_).length();
    stage_ = 0;
    refreshStatus();
    if (len < kTol) return;
    lastMeasured_ = len;
    setVcb(fmtLen(len));
    if (e.ctrl()) return;
    Guide g;
    if (startInf_.type == InferenceResult::OnEdge && startInf_.edge != kNone) {
        const Edge& E = mesh().edges[startInf_.edge];
        g.kind = Guide::Line;
        g.dir = (mesh().pos(E.v[1]) - mesh().pos(E.v[0])).normalized();
        g.p = cur_;
    } else {
        g.kind = Guide::Point;
        g.p = cur_;
    }
    doc().beginOp("Guide");
    doc().state.guides.push_back(g);
    mesh().touch();
    doc().commitOp();
    update();
}

void TapeTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (stage_ == 0) inferPoint(e, std::nullopt);
    else {
        InferenceResult r = inferPoint(e, start_);
        cur_ = r.p;
        setVcb(fmtLen((cur_ - start_).length()));
    }
    update();
}

void TapeTool::mouseDoubleClick(const MouseEvent& e) {
    Id ed = engine().pickEdge(e.x, e.y, 7.0);
    stage_ = 0;
    refreshStatus();
    if (ed == kNone) return;
    const Edge& E = mesh().edges[ed];
    Guide g;
    g.kind = Guide::Line;
    g.p = mesh().pos(E.v[0]);
    g.dir = (mesh().pos(E.v[1]) - mesh().pos(E.v[0])).normalized();
    doc().beginOp("Guide");
    doc().state.guides.push_back(g);
    mesh().touch();
    doc().commitOp();
    update();
}

bool TapeTool::vcbEnter(const QString& text) {
    double len;
    if (!parseLen(text, len) || len <= kTol) return false;
    if (stage_ == 1) {
        Vec3 d = cur_ - start_;
        if (d.length() < kTol) return false;
        Guide g;
        g.p = start_ + d.normalized() * len;
        if (startInf_.type == InferenceResult::OnEdge && startInf_.edge != kNone) {
            const Edge& E = mesh().edges[startInf_.edge];
            g.kind = Guide::Line;
            g.dir = (mesh().pos(E.v[1]) - mesh().pos(E.v[0])).normalized();
        } else {
            g.kind = Guide::Point;
        }
        doc().beginOp("Guide");
        doc().state.guides.push_back(g);
        mesh().touch();
        doc().commitOp();
        stage_ = 0;
        lastMeasured_ = len;
        refreshStatus();
        update();
        return true;
    }
    if (lastMeasured_ <= kTol) return false;
    double f = len / lastMeasured_;
    if (QMessageBox::question(vp_, QStringLiteral("Resize model"),
                              QStringLiteral("Do you want to resize the model by a factor of %1?").arg(f, 0, 'g', 5)) !=
        QMessageBox::Yes)
        return true;
    doc().beginOp("Resize Model");
    std::set<Id> all;
    for (Id v = 0; v < (Id)mesh().vertices.size(); ++v)
        if (mesh().vertices[v].alive) all.insert(v);
    Mat4 s = Mat4::scale(Vec3(f, f, f));
    mesh().transformVertices(all, s, false);
    for (Guide& g : doc().state.guides) g.p = s.transformPoint(g.p);
    for (Material& m : doc().state.materials) {
        m.texture.worldWidth *= f;
        m.texture.worldHeight *= f;
    }
    ++doc().materialsRevision;
    doc().commitOp();
    lastMeasured_ = len;
    vp_->zoomExtents();
    update();
    return true;
}

void TapeTool::buildPreview(Preview& pv) {
    if (stage_ == 1) {
        pv.line(start_, cur_, hasLock() ? lockColor() : lineColor(start_, cur_), 1.2f, Dash::Dashed);
        if (startInf_.type == InferenceResult::OnEdge && startInf_.edge != kNone) {
            const Edge& E = mesh().edges[startInf_.edge];
            Vec3 d = (mesh().pos(E.v[1]) - mesh().pos(E.v[0])).normalized();
            pv.line(cur_ - d * 1e4, cur_ + d * 1e4, Color::rgb8(60, 60, 60), 1.0f, Dash::Dashed, true);
        }
        pv.labels.push_back({(start_ + cur_) * 0.5, fmtLen((cur_ - start_).length()).toStdString()});
    }
    engine().addPreview(inf_, pv);
}

} // namespace gs
