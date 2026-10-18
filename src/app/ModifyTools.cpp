// SPDX-License-Identifier: GPL-3.0-or-later
#include "ModifyTools.h"

#include "Viewport.h"

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
    return QStringLiteral("Select objects. Ctrl = add, Shift = add/subtract, Ctrl+Shift = subtract. Drag to select multiple; double-click a face to include its edges, triple-click for connected geometry.");
}

void SelectTool::applyPick(const Selection& picked, Qt::KeyboardModifiers mods) {
    Selection& s = doc().selection;
    bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier;
    if (ctrl && shift) {
        for (Id f : picked.faces) s.faces.erase(f);
        for (Id e : picked.edges) s.edges.erase(e);
    } else if (ctrl) {
        s.faces.insert(picked.faces.begin(), picked.faces.end());
        s.edges.insert(picked.edges.begin(), picked.edges.end());
    } else if (shift) {
        for (Id f : picked.faces)
            if (!s.faces.erase(f)) s.faces.insert(f);
        for (Id e : picked.edges)
            if (!s.edges.erase(e)) s.edges.insert(e);
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
    if (pr.edge != kNone) {
        s.edges.insert(pr.edge);
        expandCurves(mesh(), s.edges);
    } else if (pr.face != kNone) {
        s.faces.insert(pr.face);
    }
    if (s.empty() && (e.mods & (Qt::ControlModifier | Qt::ShiftModifier))) return;
    applyPick(s, e.mods);
}

void SelectTool::mouseDoubleClick(const MouseEvent& e) {
    dblTimer_.start();
    dblPos_ = QPointF(e.x, e.y);
    PickResult pr = engine().pick(e.x, e.y);
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
    else {
        int g = engine().pickGuide(e.x, e.y, 6.0);
        if (g >= 0) markedGuides_.insert(g);
    }
}

void EraserTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    erasing_ = true;
    marked_.clear();
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
    if (marked_.empty() && markedGuides_.empty()) return;
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
        m.touch();
    } else {
        doc().beginOp("Erase");
        for (Id ed : marked_) m.eraseEdge(ed, true);
        std::vector<int> gs(markedGuides_.rbegin(), markedGuides_.rend());
        for (int g : gs)
            if (g < (int)doc().state.guides.size()) doc().state.guides.erase(doc().state.guides.begin() + g);
        m.touch();
    }
    doc().commitOp();
    marked_.clear();
    markedGuides_.clear();
    update();
}

void EraserTool::buildPreview(Preview& pv) {
    if (erasing_) pv.highlightEdges = marked_;
    else if (hover_ != kNone) pv.highlightEdges.insert(hover_);
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
    if (pr.edge != kNone) {
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
        doc().notifySelection();
    } else {
        verts_ = selectionVertices(m, doc().selection);
    }
    orig_.clear();
    for (Id v : verts_) orig_[v] = m.pos(v);
    affectedFaces_.clear();
    for (Id f = 0; f < (Id)m.faces.size(); ++f) {
        if (!m.faces[f].alive) continue;
        for (Id v : m.faceVertices(f))
            if (verts_.count(v)) { affectedFaces_.push_back(f); break; }
    }
    active_ = !verts_.empty();
    if (!active_) doc().cancelOp();
    return active_;
}

void TransformTool::applyLive(const Mat4& mt) {
    Mesh& m = mesh();
    for (auto& [v, p] : orig_) m.vertices[v].p = mt.transformPoint(p);
    for (Id f : affectedFaces_) m.recomputeNormal(f);
    m.markGeometryMoved();
}

void TransformTool::finish(const Mat4& mt) {
    Mesh& m = mesh();
    for (auto& [v, p] : orig_) m.vertices[v].p = p;
    m.markGeometryMoved();
    m.transformVertices(verts_, mt, true);
    doc().commitOp();
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
    doc().cancelOp();
    doc().selection = origSelection_;
    doc().validateSelection();
    doc().notifySelection();
    active_ = false;
}

void TransformTool::toggleCopy() {
    if (!active_) return;
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
    }
    doc().selection = all;
    doc().commitOp();
    doc().notifySelection();
    lastWasCopy_ = false;
    update();
}

// ---- Move ------------------------------------------------------------------

QString MoveTool::status() const {
    if (!active_) return copy_ ? QStringLiteral("Copy: pick the base point. Ctrl toggles copy.")
                               : QStringLiteral("Pick the base point to move. Ctrl = copy. After copying, type e.g. 5x or /5 to make an array.");
    return QStringLiteral("Pick the destination or type a distance. Arrow keys lock axes, Shift locks the inference.");
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
        hoverFace_ = hoverEdge_ = kNone;
        if (doc().selection.empty()) {
            PickResult pr = engine().pick(e.x, e.y);
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
    if (!active_) return QStringLiteral("Pick a point to start scaling the selection about its center. Type e.g. 2 or 1,2,1 for exact factors.");
    return QStringLiteral("Move to scale, click to finish, or type a scale factor.");
}

Mat4 ScaleTool::currentTransform() const {
    return Mat4::translation(origin_) * Mat4::scale(factor_) * Mat4::translation(-origin_);
}

void ScaleTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    lastMouse_ = e;
    if (!active_) {
        if (!ensureSelection(e)) return;
        InferenceResult r = inferPoint(e, std::nullopt);
        std::set<Id> vs = selectionVertices(mesh(), doc().selection);
        if (vs.empty()) return;
        bbMin_ = Vec3(1e300, 1e300, 1e300);
        bbMax_ = Vec3(-1e300, -1e300, -1e300);
        for (Id v : vs)
            for (int i = 0; i < 3; ++i) {
                bbMin_[i] = std::min(bbMin_[i], mesh().pos(v)[i]);
                bbMax_[i] = std::max(bbMax_[i], mesh().pos(v)[i]);
            }
        origin_ = (bbMin_ + bbMax_) * 0.5;
        base_ = r.p;
        if ((base_ - origin_).length() < kTol) base_ = bbMax_;
        factor_ = Vec3(1, 1, 1);
        if (!begin("Scale", false)) return;
        refreshStatus();
        return;
    }
    finish(currentTransform());
    update();
}

void ScaleTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    if (!active_) {
        inferPoint(e, std::nullopt);
        update();
        return;
    }
    InferenceResult r = inferPoint(e, std::nullopt, std::nullopt, &verts_, false);
    cur_ = r.p;
    Vec3 axis = base_ - origin_;
    double d0 = axis.length();
    double f = (cur_ - origin_).dot(axis / d0) / d0;
    if (std::fabs(f) < 1e-3) f = f < 0 ? -1e-3 : 1e-3;
    factor_ = Vec3(f, f, f);
    applyLive(currentTransform());
    setVcb(QString::number(f, 'g', 4));
    update();
}

bool ScaleTool::vcbEnter(const QString& text) {
    if (!active_) return false;
    QStringList parts = text.split(QRegularExpression("[;,]"));
    Vec3 f;
    if (parts.size() == 1) {
        double v;
        if (!parseNumber(parts[0].toStdString(), v) || std::fabs(v) < 1e-9) return false;
        f = Vec3(v, v, v);
    } else if (parts.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            double v;
            if (!parseNumber(parts[i].toStdString(), v) || std::fabs(v) < 1e-9) return false;
            f[i] = v;
        }
    } else {
        return false;
    }
    factor_ = f;
    finish(currentTransform());
    update();
    return true;
}

void ScaleTool::buildPreview(Preview& pv) {
    if (active_) {
        Vec3 mn = currentTransform().transformPoint(bbMin_), mx = currentTransform().transformPoint(bbMax_);
        Vec3 c[8];
        for (int i = 0; i < 8; ++i) c[i] = Vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& ed : E) pv.line(c[ed[0]], c[ed[1]], Color::rgb8(220, 120, 0), 1.0f, Dash::Dashed);
        pv.line(origin_, cur_, Color::rgb8(0, 0, 0), 1.0f, Dash::Dotted);
    }
    engine().addPreview(inf_, pv);
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
