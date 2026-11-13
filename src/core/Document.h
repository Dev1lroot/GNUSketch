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
    std::set<Id> instances;    // component instances, at the level being edited
    std::set<Id> objects;      // lights and cameras in the world
    bool empty() const { return faces.empty() && edges.empty() && instances.empty() && objects.empty(); }
    size_t count() const { return faces.size() + edges.size() + instances.size() + objects.size(); }
    void clear() { faces.clear(); edges.clear(); instances.clear(); objects.clear(); }
    bool operator==(const Selection&) const = default;
};

// A saved pose of a component's geometry (F5 while it is open): where each
// vertex is, in the definition's own coordinates.
struct AnimState {
    std::string name;
    std::vector<Id> verts;
    std::vector<Vec3> pos;
    // Where the components inside it are: (instance, transform in the definition).
    std::vector<std::pair<Id, Mat4>> children;
};

// One thing an entity fires, some time after it was fired.
struct EntityTarget {
    std::string name;
    double delay = 0; // seconds
    bool operator==(const EntityTarget&) const = default;
};

// What makes an instance a live thing in Game Mode, in the manner of Valve
// Hammer entities: a class, a name others can target, a target of its own.
struct EntityProps {
    std::string classname;  // func_door, func_button, func_actor; empty = plain geometry
    std::string targetname; // what buttons refer to
    std::vector<EntityTarget> targets; // which entities fire() triggers, and when
    std::string startState; // empty = the first state
    std::string script;     // JavaScript; see core/Animation.h and io/EntityScript.h
    // Game Mode: it falls, slides and tumbles as a rigid body.
    bool physics = false;
    double mass = 10;     // kg
    double friction = 0.5;
    double bounce = 0.1;
    bool isEntity() const { return !classname.empty(); }
    // Anything that gets moving parts in Game Mode.
    bool isLive() const { return isEntity() || physics; }
    bool operator==(const EntityProps&) const = default;
};

// A component definition: geometry shared by all of its instances, in the
// definition's own coordinates.
struct ComponentDef {
    std::string name;
    std::string uid; // stable identity (lets pasted copies stay linked)
    Mesh mesh;
    std::vector<AnimState> states;
    std::string prefab; // the prefab it was placed from, for showing that
    // What a file format keeps with it that GNUSketch has no place for (JSON text,
    // saved with the model): an Open Block Model group's tags and cubes, for one.
    std::string extra;
};

// One placement of a component definition.
struct Instance {
    Id def = kNone;
    Mat4 xf; // definition -> world
    bool alive = true;
    bool hidden = false;
    EntityProps entity;
    Id parent = kNone;    // the definition this one is inside; kNone = the model itself
    bool dynamic = false; // Game Mode: collides as a moving body, not as static geometry
};

// A light source or camera placed in the world.
struct SceneObject {
    enum Kind { PointLight, SpotLight, DirectionalLight, ViewCamera };
    Kind kind = PointLight;
    std::string name;
    Vec3 pos;
    Vec3 target;            // where spot lights, directional lights and cameras aim
    bool alive = true;
    bool enabled = true;    // lights: switched on
    bool hidden = false;    // not shown; a hidden light does not shine
    Color color = Color(1.0f, 0.95f, 0.85f);
    double intensity = 3;   // lights; point and spot lights at 1 m
    double range = 15;      // point and spot: meters until the light has faded out
    double coneAngle = 50;  // spot: full angle in degrees
    double softness = 0.3;  // spot: 0 hard edge .. 1 fades from the centre
    double fov = 35;        // camera: vertical field of view in degrees
    bool perspective = true;
    bool isLight() const { return kind != ViewCamera; }
    bool aims() const { return kind != PointLight; }
};
const char* sceneObjectTypeName(SceneObject::Kind k);

// Everything that participates in undo/redo.
struct ModelState {
    Mesh mesh; // top-level loose geometry
    std::vector<Material> materials;
    std::vector<Guide> guides;
    std::vector<ComponentDef> defs;
    std::vector<Instance> instances;
    std::vector<SceneObject> objects;
    // What a file format (a plugin) keeps with the whole model that GNUSketch has
    // no place for: JSON text, saved with it (see ComponentDef::extra).
    std::string extra;
    // Instance open for editing (kNone = top level). While open, its
    // definition mesh and the transforms of the components inside it hold world
    // coordinates (the instance transform is baked in) so every tool edits it
    // in place; see Components.h.
    Id editing = kNone;
    std::vector<Id> outerOpen; // the ones open around it, outermost first

    Id activeDef() const { return editing == kNone ? kNone : instances[editing].def; }

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
    bool showObjects = true;   // light and camera markers
    bool showHidden = false;
    bool sky = true;
    bool ground = true;
    bool shadows = false;      // sun shadows on faces and the ground
    // Cinematic lighting: ray traced soft shadows from the sun and every light,
    // and light rays (volumetric shafts) of the given strength 0..1.
    bool cinematic = false;
    double lightRays = 0.5;
    bool ambientOcclusion = false; // darkening in creases and corners (screen space)
    bool antialiasing = true;  // multisampling
    bool backfaceCulling = false; // the back of a face is not drawn or picked
    bool sun = true;           // sunlight and the sun in the sky
    double sunAzimuth = 207;   // degrees clockwise from +Y (north, green axis)
    double sunAltitude = 48;   // degrees above the horizon
    double sunIntensity = 1.0;
    // Unit vector towards the sun.
    Vec3 sunDirection() const {
        const double az = sunAzimuth * kPi / 180, alt = sunAltitude * kPi / 180;
        return {std::sin(az) * std::cos(alt), std::cos(az) * std::cos(alt), std::sin(alt)};
    }
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
    // Adds a light or camera with a unique name; returns its id.
    Id addObject(SceneObject o);
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
