// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - components: shared definitions placed as transformed instances.
//
// Editing a definition changes every instance of it. To let all tools edit a
// component in place without knowing about transforms, opening an instance
// bakes its transform into the definition mesh (which then holds world
// coordinates) and closing it bakes the inverse back. While open, every other
// instance of that definition is drawn relative to the open one.
#pragma once

#include "Document.h"

#include <string>

namespace gs {

// Transform an instance is drawn and picked with (accounts for an open component).
Mat4 instanceWorldXf(const ModelState& st, Id inst);
// World-space bounding box of an instance; false if its definition is empty.
bool instanceBounds(const ModelState& st, Id inst, Vec3& mn, Vec3& mx);
// The 8 corners of an instance's box in its definition's own axes, in world space.
bool instanceBoxCorners(const ModelState& st, Id inst, Vec3 corners[8]);
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

// Moves the selected top-level faces/edges into a new definition and puts an
// instance in their place. Returns the instance id (kNone if nothing selected).
Id makeComponent(ModelState& st, const Selection& sel, const std::string& name);
// Adds another instance of the same definition with the same transform.
Id cloneInstance(ModelState& st, Id inst);
// Replaces the instance by loose top-level geometry; new ids go to `out`.
void explodeInstance(ModelState& st, Id inst, Selection* out = nullptr);
// Gives the instances their own copy of their definition (one new definition
// per original definition, shared by the given instances). Instances that
// already are the only users of their definition are left alone.
void makeUnique(ModelState& st, const std::set<Id>& insts);

void openComponent(ModelState& st, Id inst);
void closeComponent(ModelState& st);
// A copy of the state with any open component closed (for saving/exporting).
ModelState closedState(const ModelState& st);
// Top-level geometry plus the geometry of every visible instance, in world
// coordinates (for exporters that have no notion of instancing).
Mesh flattenModel(const ModelState& st);

} // namespace gs
