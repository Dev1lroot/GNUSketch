// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - document: geometry, materials, guides, selection and undo history.
#pragma once

#include "Material.h"
#include "Mesh.h"
#include "Units.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace gs {

struct Guide {
    enum Kind { Line, Point };
    Kind kind = Line;
    Vec3 p;   // point on the line / the point
    Vec3 dir; // unit direction (lines)
};

struct Selection {
    std::set<Id> faces, edges; // in the active mesh
    std::set<Id> instances;    // component instances (top level only)
    bool empty() const { return faces.empty() && edges.empty() && instances.empty(); }
    size_t count() const { return faces.size() + edges.size() + instances.size(); }
    void clear() { faces.clear(); edges.clear(); instances.clear(); }
    bool operator==(const Selection&) const = default;
};

// A component definition: geometry shared by all of its instances, in the
// definition's own coordinates.
struct ComponentDef {
    std::string name;
    std::string uid; // stable identity (lets pasted copies stay linked)
    Mesh mesh;
};

// One placement of a component definition.
struct Instance {
    Id def = kNone;
    Mat4 xf; // definition -> world
    bool alive = true;
    bool hidden = false;
};

// Everything that participates in undo/redo.
struct ModelState {
    Mesh mesh; // top-level loose geometry
    std::vector<Material> materials;
    std::vector<Guide> guides;
    std::vector<ComponentDef> defs;
    std::vector<Instance> instances;
    // Instance open for editing (kNone = top level). While open, its
    // definition mesh holds world coordinates (the instance transform is
    // baked in) so every tool edits it in place; see Components.h.
    Id editing = kNone;

    Mesh& activeMesh() { return editing == kNone ? mesh : defs[instances[editing].def].mesh; }
    const Mesh& activeMesh() const { return editing == kNone ? mesh : defs[instances[editing].def].mesh; }
};

struct RenderStyle {
    enum FaceStyle { Wireframe, HiddenLine, Shaded, ShadedTextures, Monochrome };
    FaceStyle faceStyle = ShadedTextures;
    bool xray = false;
    bool edges = true;
    bool backEdges = false;
    bool profiles = true;
    float profileWidth = 2.5f;
    bool showAxes = true;
    bool showGuides = true;
    bool showHidden = false;
    bool sky = true;
    bool ground = true;
    Color background = Color::rgb8(255, 255, 255);
    Color skyColor = Color::rgb8(166, 205, 236);
    Color horizonColor = Color::rgb8(222, 234, 244);
    Color groundColor = Color::rgb8(206, 202, 190);
    Color edgeColor = Color::rgb8(0, 0, 0);
    Color frontColor = defaultFrontColor();
    Color backColor = defaultBackColor();
};

class Document {
public:
    Document();

    ModelState state;
    Selection selection;
    RenderStyle style;
    Units units;
    std::string filePath;
    bool modified = false;
    uint64_t materialsRevision = 1;
    Id currentMaterial = kNone; // paint bucket material (kNone = default)

    // The mesh being edited: the top-level geometry, or the definition of the
    // component that is open for editing.
    Mesh& mesh() { return state.activeMesh(); }
    const Mesh& mesh() const { return state.activeMesh(); }
    bool editingComponent() const { return state.editing != kNone; }
    // Opens a component instance for editing in place / returns to the top level.
    void openComponent(Id instance);
    void closeComponent();
    std::vector<Material>& materials() { return state.materials; }
    const Material* material(Id m) const {
        return (m >= 0 && m < (Id)state.materials.size()) ? &state.materials[m] : nullptr;
    }

    // Undo protocol: beginOp() snapshots the state; commitOp() keeps the change;
    // cancelOp() restores the snapshot. Nested begins are ignored.
    void beginOp(const std::string& name);
    void commitOp();
    void cancelOp();
    bool inOp() const { return opDepth_ > 0; }
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoName() const { return undo_.empty() ? std::string() : undo_.back().first; }
    std::string redoName() const { return redo_.empty() ? std::string() : redo_.back().first; }
    void clearHistory();
    void reset(); // new empty document

    Id addMaterial(const Material& m);
    std::string uniqueMaterialName(const std::string& base) const;
    void validateSelection();

    // Change notification hooks (set by the UI).
    std::function<void()> onChanged;
    std::function<void()> onSelectionChanged;
    void notifyChanged() { if (onChanged) onChanged(); }
    void notifySelection() { if (onSelectionChanged) onSelectionChanged(); }

private:
    void restored(); // after the state was replaced by an undo snapshot
    std::vector<std::pair<std::string, ModelState>> undo_, redo_;
    int opDepth_ = 0;
    static constexpr size_t kMaxUndo = 200;
};

} // namespace gs
