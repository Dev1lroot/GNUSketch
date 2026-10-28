// SPDX-License-Identifier: GPL-3.0-or-later
#include "SolidTools.h"

#include "Viewport.h"

namespace gs {

bool isSolidTool(ToolId id) {
    return id == ToolId::SolidUnion || id == ToolId::SolidIntersect || id == ToolId::SolidSubtract ||
           id == ToolId::SolidTrim || id == ToolId::SolidSplit;
}

SolidOp solidToolOp(ToolId id) {
    switch (id) {
    case ToolId::SolidIntersect: return SolidOp::Intersect;
    case ToolId::SolidSubtract: return SolidOp::Subtract;
    case ToolId::SolidTrim: return SolidOp::Trim;
    case ToolId::SolidSplit: return SolidOp::Split;
    default: return SolidOp::Union;
    }
}

QString SolidTool::status() const {
    QString s;
    if (const_cast<SolidTool*>(this)->doc().editingComponent()) {
        s = QStringLiteral("Close the open component first (Esc), then click the solids.");
    } else if (first_ == kNone) {
        switch (solidToolOp(id_)) {
        case SolidOp::Union: s = QStringLiteral("Click the first solid component to combine."); break;
        case SolidOp::Intersect: s = QStringLiteral("Click the first solid component to intersect."); break;
        case SolidOp::Subtract: s = QStringLiteral("Click the solid component to subtract; it is removed afterwards."); break;
        case SolidOp::Trim: s = QStringLiteral("Click the solid component that cuts; it is kept."); break;
        case SolidOp::Split: s = QStringLiteral("Click the first solid component to split."); break;
        }
    } else {
        switch (solidToolOp(id_)) {
        case SolidOp::Union: s = QStringLiteral("Click the second solid to combine both into one."); break;
        case SolidOp::Intersect: s = QStringLiteral("Click the second solid; only the volume they share stays."); break;
        case SolidOp::Subtract: s = QStringLiteral("Click the solid to cut it out of. Esc = start over."); break;
        case SolidOp::Trim: s = QStringLiteral("Click the solid to trim with it. Esc = start over."); break;
        case SolidOp::Split: s = QStringLiteral("Click the second solid to split both where they overlap."); break;
        }
    }
    return message_.isEmpty() ? s : message_ + QStringLiteral("  ") + s;
}

bool SolidTool::solid(Id inst, std::string* why) {
    const ModelState& st = doc().state;
    if (inst < 0 || inst >= (Id)st.instances.size() || !st.instances[inst].alive) return false;
    const Id def = st.instances[inst].def;
    Cached& c = cache_[def];
    if (c.revision != st.defs[def].mesh.revision) {
        c.revision = st.defs[def].mesh.revision;
        c.why.clear();
        c.solid = isSolid(st.defs[def].mesh, &c.why);
    }
    if (why) *why = c.why;
    return c.solid;
}

void SolidTool::activate() {
    const Selection& sel = doc().selection;
    const SolidOp op = solidToolOp(id_);
    std::vector<Id> picked;
    for (Id i : sel.instances)
        if (solid(i)) picked.push_back(i);
    if ((op == SolidOp::Union || op == SolidOp::Intersect) && picked.size() >= 2) {
        apply(picked);
        return;
    }
    if (sel.instances.size() == 1 && picked.size() == 1) first_ = picked[0];
    refreshStatus();
}

void SolidTool::reset() {
    first_ = hover_ = kNone;
    message_.clear();
    refreshStatus();
    update();
}

void SolidTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    Id h = engine().pickInstance(e.x, e.y);
    if (h != hover_) {
        hover_ = h;
        std::string why;
        if (h != kNone && !solid(h, &why))
            message_ = QStringLiteral("Not a solid: %1.").arg(QString::fromStdString(why));
        else if (message_.startsWith(QStringLiteral("Not a solid")))
            message_.clear();
        refreshStatus();
    }
    update();
}

void SolidTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton) return;
    Id inst = engine().pickInstance(e.x, e.y);
    if (inst == kNone) return;
    std::string why;
    if (!solid(inst, &why)) {
        message_ = QStringLiteral("Not a solid: %1.").arg(QString::fromStdString(why));
        refreshStatus();
        return;
    }
    if (first_ == kNone || first_ == inst || !doc().state.instances[first_].alive) {
        first_ = inst;
        message_.clear();
        doc().selection.clear();
        doc().selection.instances.insert(inst);
        doc().notifySelection();
        refreshStatus();
        update();
        return;
    }
    apply({first_, inst});
}

// Applies the operation to the instances in click order (Union/Intersect fold
// every further instance into the result).
void SolidTool::apply(const std::vector<Id>& order) {
    const SolidOp op = solidToolOp(id_);
    doc().beginOp(solidOpName(op));
    Selection out;
    std::string err;
    bool ok = true;
    Id acc = order[0];
    for (size_t k = 1; k < order.size() && ok; ++k) {
        ok = solidOperation(doc().state, op, acc, order[k], &out, &err);
        acc = order[k];
    }
    if (ok) {
        doc().selection = out;
        doc().commitOp();
        doc().notifySelection();
        message_.clear();
    } else {
        doc().cancelOp();
        message_ = QString::fromStdString(err);
    }
    first_ = hover_ = kNone;
    refreshStatus();
    update();
}

void SolidTool::buildPreview(Preview& pv) {
    if (hover_ != kNone && hover_ != first_ && solid(hover_)) pv.highlightInstances.insert(hover_);
}

} // namespace gs
