// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - JavaScript behind entities in Game Mode.
//
// Every entity (func_door, func_button, func_actor) runs its own script. The
// script has no access to files or the system. It sees:
//
//   self            this entity
//     .name .classname .targets (array of { name, delay }) .target (the first's name)
//     .states (array of names) .state (the current one)
//     .moving .linked (another entity targets this one)
//     .set(state)                       jump there
//     .tween(state, opts, done)         animate there; opts: a duration in seconds, or
//                                       { duration, easing, from }; done is optional
//     .play([states], opts, done)       one after another; opts also { loop: true }
//     .stop()                           stay where it is now
//     .fire(arg)                        send "trigger" to all its targets, each after its delay
//     .find(name)                       as find() below
//     .send(event, arg)                 send an event to this entity
//   on(event, fn)   "start", "use" (the player presses it), "touch" (walks into it),
//                   "trigger" (fired by another entity), "tick" (fn receives dt),
//                   or any name used with send()
//   after(seconds, fn), every(seconds, fn), cancel(id)
//   find(name)      the entities a name means from here: those in the innermost
//                   component around this one that has any by that name, or else any
//                   in the model. "front/door" only counts those inside a component
//                   named front. Targets are looked up the same way.
//   log(...)        prints to the terminal
//   game.time       seconds since the start
//   Easing names: linear, inQuad, outQuad, inOutQuad, inCubic, outCubic, inOutCubic,
//   outBack, outBounce, outElastic
//
// As in Valve Hammer: a door or actor that another entity targets takes no "use"
// or "touch" from the player, only "trigger".
#pragma once

#include "core/Animation.h"
#include "core/Physics.h"
#include "core/Walker.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>
#include <string>
#include <vector>

class QJSEngine;

namespace gs {

struct ScriptPreset {
    QString name;
    QString classname; // empty = any
    QString code;
    bool builtIn = true;
    QString path;
};
// The ready-made presets, then the user's from the "scripts" asset directory.
std::vector<ScriptPreset> scriptPresets();
// Writes a user preset; returns its file path or empty.
QString saveScriptPreset(const QString& name, const QString& classname, const QString& code);
QString defaultScript(const std::string& classname);
const char* const* entityClasses(int* count);

// Checks the syntax; false with a message with its line number.
bool checkEntityScript(const QString& code, QString* error);

class EntityWorld;

// The native side of the scripts' animator.
class ScriptBridge : public QObject {
    Q_OBJECT
public:
    explicit ScriptBridge(EntityWorld* w) : world_(w) {}
    Q_INVOKABLE void setPose(int i, int a, int b, double t);
    Q_INVOKABLE QVariantList resolve(int from, const QString& name);
    Q_INVOKABLE void print(const QString& text);
    Q_INVOKABLE void error(const QString& text);

private:
    EntityWorld* world_;
};

class EntityWorld {
public:
    // Turns the entities of this model (a throw-away copy) into moving parts.
    // Build the static collision world after this, then call start().
    explicit EntityWorld(ModelState& state);
    ~EntityWorld();

    // Adds the moving bodies to a built collision world and starts the scripts.
    void start(CollisionWorld& world);
    void update(double dt);
    int count() const { return (int)entities_.size(); }
    const AnimEntity& entity(int i) const { return entities_[i]; }
    // The entity a collision body belongs to, or -1.
    int entityOfBody(int body) const;
    // Whether the player can press it directly.
    bool usable(int i) const;
    bool linked(int i) const { return linked_[i]; }
    void use(int i);
    // Call each step with whether the walker touches the entity now.
    void setTouching(int i, bool touching);
    QStringList errors() const { return errors_; }
    void setPose(int i, int a, int b, double t);
    std::vector<int> resolve(int from, const std::string& name) const;
    // Rigid bodies.
    const PhysicsWorld& physics() const { return physics_; }
    bool isPhysical(int i) const;
    // Pushes it for that long with a force in N at a world point. It stops
    // gaining once it moves at a fifth of a m/s per 40 N: a walker pushes, no more.
    void pushBy(int i, const Vec3& force, const Vec3& at, double seconds);
    void addError(const QString& text);

private:
    bool call(const QString& function, int entity, const QString& event);
    void startPhysics();
    void stepPhysics(double dt);
    PhysicsWorld physics_;
    bool inPhysics_ = false;
    int lastA_ = -1;
    ModelState& state_;
    CollisionWorld* world_ = nullptr;
    std::vector<AnimEntity> entities_;
    std::vector<char> linked_, touching_;
    std::unique_ptr<QJSEngine> engine_;
    std::unique_ptr<ScriptBridge> bridge_;
    QStringList errors_;
    struct Watchdog;
    std::unique_ptr<Watchdog> watchdog_;
};

} // namespace gs
