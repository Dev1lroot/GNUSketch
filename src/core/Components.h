// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - components: shared definitions placed as transformed instances.
//
// Editing a definition changes every instance of it. To let all tools edit a
// component in place without knowing about transforms, opening an instance
// bakes its transform into the definition mesh (which then holds world
// coordinates) and closing it bakes the inverse back. While open, every other
// instance of that definition is drawn relative to the open one.
//
// Components can hold components: an instance whose `parent` is a definition is
// inside it, and shows up in every placement of that definition. Opening one
// inside another stacks: the components inside the open one have their
// transforms baked as well, so at the level being edited an instance's transform
// always is world coordinates.
#pragma once

#include "Document.h"

#include <map>
#include <string>

namespace gs {

// Transform an instance is drawn and picked with (accounts for an open component).
Mat4 instanceWorldXf(const ModelState& st, Id inst);
// World-space bounding box of an instance; false if its definition is empty.
bool instanceBounds(const ModelState& st, Id inst, Vec3& mn, Vec3& mx);
// The 8 corners of an instance's box in its definition's own axes, in world space.
bool instanceBoxCorners(const ModelState& st, Id inst, Vec3 corners[8]);
// One placement of a definition, anywhere in the hierarchy.
struct PlacedInstance {
    Id inst = kNone, def = kNone;
    Mat4 xf;            // definition mesh -> world
    bool active = true; // inside what is being edited (not the faded surroundings)
    Id root = kNone;    // the instance at the level being edited that it belongs to
};
// Every visible placement, parents before children, the open instance included.
std::vector<PlacedInstance> placedInstances(const ModelState& st);
// Bounds of the definition's geometry and all inside it, in its own coordinates.
void definitionBounds(const ModelState& st, Id def, Vec3& mn, Vec3& mx, bool& any);
// The definitions directly inside this one, or anything further down, reach `to`.
bool definitionContains(const ModelState& st, Id def, Id to);
// Could an instance of def go into what is being edited? Not into itself.
bool canPlaceInActive(const ModelState& st, Id def);
// The instances that are inside a definition.
std::vector<Id> childrenOf(const ModelState& st, Id def);
// Bounds of the top-level geometry and every visible instance.
void modelBounds(const ModelState& st, Vec3& mn, Vec3& mx, bool& any);
int instanceCount(const ModelState& st, Id def);

// Copies faces/edges of src (every alive one when both sets are null; faces
// bring their edges) into dst, transformed by xf. weld merges the new vertices
// with coincident existing ones. New entity ids are added to `created`.
void appendMesh(Mesh& dst, const Mesh& src, const Mat4& xf, const std::set<Id>* faces = nullptr,
                const std::set<Id>* edges = nullptr, bool weld = false, Selection* created = nullptr);

std::string uniqueComponentName(const ModelState& st, const std::string& base);
std::string newComponentUid();

// Moves the selected faces, edges and components of the level being edited into a
// new definition and puts an instance in their place. Returns the instance id
// (kNone if nothing selected).
Id makeComponent(ModelState& st, const Selection& sel, const std::string& name);
// Adds another instance of the same definition with the same transform.
Id cloneInstance(ModelState& st, Id inst);
// Replaces the instance by loose top-level geometry; new ids go to `out`.
void explodeInstance(ModelState& st, Id inst, Selection* out = nullptr);
// Gives the instances their own copy of their definition (one new definition
// per original definition, shared by the given instances). Instances that
// already are the only users of their definition are left alone.
void makeUnique(ModelState& st, const std::set<Id>& insts);

// A model holding only this definition (with the materials it uses, renumbered)
// and one instance of it at the origin: what a prefab file contains.
// What is inside it comes along. The definition is number 0, its instance number 0.
ModelState extractDefinition(const ModelState& st, Id def);
// Copies an instance with all inside it into another state, as a top-level one
// (materials keep their numbers). defMap avoids copying a definition twice.
Id copyInstanceInto(ModelState& dst, const ModelState& src, Id inst, std::map<Id, Id>& defMap);
// Brings a definition of another model into the document, merging equal
// materials. A definition with the same uid already there is reused, so
// everything placed from one prefab stays linked. Returns the definition id.
Id importDefinition(Document& doc, const ModelState& src, Id def);

void openComponent(ModelState& st, Id inst);
// Closes the innermost open one; what is around it stays open.
void closeComponent(ModelState& st);
void closeAllComponents(ModelState& st);
// A copy of the state with any open component closed (for saving/exporting).
ModelState closedState(const ModelState& st);
// Top-level geometry plus the geometry of every visible instance, in world
// coordinates (for exporters that have no notion of instancing).
Mesh flattenModel(const ModelState& st);

} // namespace gs
