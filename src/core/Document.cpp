// SPDX-License-Identifier: GPL-3.0-or-later
#include "Document.h"

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
        state.mesh.touch();
        state.mesh.markGeometryMoved();
        ++materialsRevision;
    }
    validateSelection();
    notifyChanged();
}

bool Document::undo() {
    if (undo_.empty() || opDepth_ > 0) return false;
    redo_.emplace_back(undo_.back().first, std::move(state));
    state = std::move(undo_.back().second);
    undo_.pop_back();
    state.mesh.touch();
    state.mesh.markGeometryMoved();
    state.mesh.revision += 1000; // never reuse an older revision number
    ++materialsRevision;
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
    state.mesh.touch();
    state.mesh.markGeometryMoved();
    state.mesh.revision += 1000;
    ++materialsRevision;
    modified = true;
    selection.clear();
    notifySelection();
    notifyChanged();
    return true;
}

void Document::clearHistory() {
    undo_.clear();
    redo_.clear();
    opDepth_ = 0;
}

void Document::reset() {
    uint64_t rev = state.mesh.revision;
    state = ModelState();
    state.mesh.revision = rev + 1000;
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
    const Mesh& m = state.mesh;
    for (auto it = selection.faces.begin(); it != selection.faces.end();)
        it = (*it < 0 || *it >= (Id)m.faces.size() || !m.faces[*it].alive) ? selection.faces.erase(it) : ++it;
    for (auto it = selection.edges.begin(); it != selection.edges.end();)
        it = (*it < 0 || *it >= (Id)m.edges.size() || !m.edges[*it].alive) ? selection.edges.erase(it) : ++it;
    if (!(before == selection)) notifySelection();
}

} // namespace gs
