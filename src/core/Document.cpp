// SPDX-License-Identifier: GPL-3.0-or-later
#include "Document.h"

#include "Components.h"

namespace gs {

Document::Document() = default;

void Document::beginOp(const std::string& name) {
    if (opDepth_++ > 0) return;
    undo_.emplace_back(name, state);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
}

void Document::commitOp() {
    if (opDepth_ == 0) return;
    if (--opDepth_ > 0) return;
    redo_.clear();
    modified = true;
    validateSelection();
    notifyChanged();
}

void Document::cancelOp() {
    if (opDepth_ == 0) return;
    if (--opDepth_ > 0) return;
    if (!undo_.empty()) {
        state = std::move(undo_.back().second);
        undo_.pop_back();
        restored();
    }
    validateSelection();
    notifyChanged();
}

bool Document::undo() {
    if (undo_.empty() || opDepth_ > 0) return false;
    redo_.emplace_back(undo_.back().first, std::move(state));
    state = std::move(undo_.back().second);
    undo_.pop_back();
    restored();
    modified = true;
    selection.clear();
    notifySelection();
    notifyChanged();
    return true;
}

bool Document::redo() {
    if (redo_.empty() || opDepth_ > 0) return false;
    undo_.emplace_back(redo_.back().first, std::move(state));
    state = std::move(redo_.back().second);
    redo_.pop_back();
    restored();
    modified = true;
    selection.clear();
    notifySelection();
    notifyChanged();
    return true;
}

void Document::restored() {
    auto refresh = [](Mesh& m) {
        m.touch();
        m.markGeometryMoved();
    };
    refresh(state.mesh);
    for (ComponentDef& d : state.defs) refresh(d.mesh);
    ++materialsRevision;
}

void Document::openComponent(Id instance) {
    if (opDepth_ > 0 || instance < 0 || instance >= (Id)state.instances.size() || !state.instances[instance].alive) return;
    gs::openComponent(state, instance);
    selection.clear();
    notifySelection();
    notifyChanged();
}

void Document::closeComponent() {
    if (opDepth_ > 0 || state.editing == kNone) return;
    Id inst = state.editing;
    gs::closeComponent(state);
    selection.clear();
    selection.instances.insert(inst);
    notifySelection();
    notifyChanged();
}

void Document::clearHistory() {
    undo_.clear();
    redo_.clear();
    opDepth_ = 0;
}

void Document::reset() {
    state = ModelState();
    selection.clear();
    filePath.clear();
    modified = false;
    currentMaterial = kNone;
    ++materialsRevision;
    clearHistory();
    notifySelection();
    notifyChanged();
}

std::string Document::uniqueMaterialName(const std::string& base) const {
    auto taken = [&](const std::string& n) {
        for (const auto& m : state.materials)
            if (m.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 1;; ++i) {
        std::string n = base + "_" + std::to_string(i);
        if (!taken(n)) return n;
    }
}

Id Document::addMaterial(const Material& m) {
    Material c = m;
    c.name = uniqueMaterialName(m.name.empty() ? "Material" : m.name);
    state.materials.push_back(c);
    ++materialsRevision;
    return (Id)state.materials.size() - 1;
}

void Document::validateSelection() {
    Selection before = selection;
    const Mesh& m = state.activeMesh();
    for (auto it = selection.faces.begin(); it != selection.faces.end();)
        it = (*it < 0 || *it >= (Id)m.faces.size() || !m.faces[*it].alive) ? selection.faces.erase(it) : ++it;
    for (auto it = selection.edges.begin(); it != selection.edges.end();)
        it = (*it < 0 || *it >= (Id)m.edges.size() || !m.edges[*it].alive) ? selection.edges.erase(it) : ++it;
    const auto& insts = state.instances;
    for (auto it = selection.instances.begin(); it != selection.instances.end();)
        it = (state.editing != kNone || *it < 0 || *it >= (Id)insts.size() || !insts[*it].alive) ? selection.instances.erase(it) : ++it;
    if (!(before == selection)) notifySelection();
}

} // namespace gs
