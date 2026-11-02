// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - animation states of components and how they play back.
//
// A state remembers where the vertices of a component are. To play, each
// connected piece of the component becomes its own moving part: between two
// states it follows a screw motion (a turn about one fixed axis plus a slide
// along it), so a door swings on its hinge instead of shrinking through a
// straight line, and its textures stay on it. A piece whose shape really
// changes (it was scaled, say) blends its vertices instead.
#pragma once

#include "Document.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace gs {

// Saves the open component's geometry as a state (replacing one of that name).
bool recordState(ModelState& st, const std::string& name);
// Moves the definition's geometry to a state.
bool applyState(ModelState& st, Id def, const std::string& name);
int findState(const ComponentDef& def, const std::string& name);

// The rigid motion taking a[i] to b[i]; residual = largest error left.
Mat4 fitRigid(const std::vector<Vec3>& a, const std::vector<Vec3>& b, double* residual = nullptr);
// The pose t of the way from one rigid pose to another along a screw motion.
Mat4 interpolateRigid(const Mat4& from, const Mat4& to, double t);
// Vertex sets of the connected pieces of a mesh (loose vertices are left out).
std::vector<std::vector<Id>> meshIslands(const Mesh& m);

struct AnimPart {
    Id instance = kNone, def = kNone; // private to this part
    Mat4 base;                        // the original instance's transform
    std::vector<Mat4> fit;            // per state: rest pose -> state pose
    std::vector<char> rigid;          // per state: fit reproduces it
    std::vector<std::vector<Vec3>> shape; // per state: all vertices (only used when not rigid)
    std::vector<Vec3> rest;           // all vertices as they were
    bool deforms = false;             // some state is not rigid
    int body = -1;                    // collision body of the game
};

struct AnimEntity {
    Id source = kNone;
    EntityProps props;
    std::vector<std::string> states;
    std::vector<AnimPart> parts;
    // Inside another entity: it rides along with that one's part.
    int owner = -1, ownerPart = -1;
    Mat4 mount; // this entity's transform in the owner part's coordinates
    int poseA = -1, poseB = -1;
    double poseT = 0;
};

// Splits every entity of a model into moving parts with their own definitions,
// and hides the originals. Meant for a throw-away copy of the model: Game Mode.
std::vector<AnimEntity> prepareEntities(ModelState& st);
// Places an entity t of the way from state a to state b.
// Returns true if a mesh changed shape, not only its transform.
bool poseEntity(ModelState& st, const AnimEntity& e, int a, int b, double t);
// After entity i moved: re-places the entities inside it, and those inside them.
// moved(j) is called for each of them.
void poseMounted(ModelState& st, std::vector<AnimEntity>& all, int i, const std::function<void(int)>& moved);

} // namespace gs
