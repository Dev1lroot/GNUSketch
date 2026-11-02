// SPDX-License-Identifier: GPL-3.0-or-later
#include "EntityScript.h"

#include "AssetLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValueList>
#include <QRegularExpression>
#include <QSaveFile>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace gs {

namespace {

// The animator, in JavaScript: the part scripts talk to.
const char* kPrelude = R"JS(
var __E = [], __timers = [], __time = 0, __nextTimer = 1;
var game = { get time() { return __time; } };
var __ease = {
    linear: function (t) { return t; },
    inQuad: function (t) { return t * t; },
    outQuad: function (t) { return t * (2 - t); },
    inOutQuad: function (t) { return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); },
    inCubic: function (t) { return t * t * t; },
    outCubic: function (t) { var u = 1 - t; return 1 - u * u * u; },
    inOutCubic: function (t) { var u = 1 - t; return t < 0.5 ? 4 * t * t * t : 1 - 4 * u * u * u; },
    outBack: function (t) { var u = t - 1; return 1 + 2.70158 * u * u * u + 1.70158 * u * u; },
    outBounce: function (t) {
        if (t < 1 / 2.75) return 7.5625 * t * t;
        if (t < 2 / 2.75) { t -= 1.5 / 2.75; return 7.5625 * t * t + 0.75; }
        if (t < 2.5 / 2.75) { t -= 2.25 / 2.75; return 7.5625 * t * t + 0.9375; }
        t -= 2.625 / 2.75; return 7.5625 * t * t + 0.984375;
    },
    outElastic: function (t) {
        if (t <= 0 || t >= 1) return t;
        return Math.pow(2, -10 * t) * Math.sin((t * 10 - 0.75) * 2 * Math.PI / 3) + 1;
    }
};
function __Entity(i, info) {
    this._i = i;
    this.name = info.name;
    this.classname = info.classname;
    this.targets = info.targets;
    this.target = info.targets.length ? info.targets[0].name : "";
    this.states = info.states;
    this.linked = info.linked;
    this.state = info.start;
    this._h = {};
    this._a = null;
    this._seq = null;
}
__Entity.prototype = {
    get moving() { return this._a !== null || this._seq !== null; },
    _index: function (name) {
        var k = this.states.indexOf(name);
        if (k < 0) throw new Error(this.classname + " " + (this.name || "(unnamed)") + " has no state \"" + name + "\"; it has: " + this.states.join(", "));
        return k;
    },
    set: function (name) {
        var k = this._index(name);
        this._a = null;
        this._seq = null;
        this.state = name;
        __native.setPose(this._i, k, k, 0);
        return this;
    },
    tween: function (to, opts, done) {
        this._seq = null;
        this._tween(to, opts, done);
        return this;
    },
    _tween: function (to, opts, done) {
        if (typeof opts === "number") opts = { duration: opts };
        if (typeof opts === "function") { done = opts; opts = {}; }
        opts = opts || {};
        var b = this._index(to), dur = opts.duration === undefined ? 1 : opts.duration;
        var ease = __ease[opts.easing || "inOutQuad"];
        if (!ease) throw new Error("no easing called \"" + opts.easing + "\"");
        var cur = this._a, t = 0, a;
        if (opts.from !== undefined) a = this._index(opts.from);
        else if (cur && cur.a === b && cur.dur > 0) {
            // Going back mid-way: from where it is now.
            a = cur.b;
            t = (1 - cur.t / cur.dur) * dur;
        } else a = this._index(this.state);
        this._a = { a: a, b: b, t: t, dur: dur, ease: ease, done: done, to: to };
        if (dur <= 0) this._step(0);
    },
    _step: function (dt) {
        var A = this._a;
        A.t += dt;
        if (A.t >= A.dur) {
            this._a = null;
            this.state = A.to;
            __native.setPose(this._i, A.b, A.b, 0);
            if (A.done) A.done.call(this);
        } else {
            __native.setPose(this._i, A.a, A.b, A.ease(A.t / A.dur));
        }
    },
    play: function (list, opts, done) {
        if (typeof opts === "number") opts = { duration: opts };
        if (typeof opts === "function") { done = opts; opts = {}; }
        opts = opts || {};
        for (var k = 0; k < list.length; ++k) this._index(list[k]);
        var self = this, n = 0;
        var seq = {};
        this._seq = seq;
        function next() {
            if (self._seq !== seq) return;
            if (n >= list.length) {
                if (opts.loop && list.length > 1) n = 0;
                else {
                    self._seq = null;
                    if (done) done.call(self);
                    return;
                }
            }
            self._tween(list[n++], opts, next);
        }
        next();
        return this;
    },
    stop: function () { this._a = null; this._seq = null; return this; },
    send: function (event, arg) {
        var h = this._h[event];
        if (h) for (var k = 0; k < h.length; ++k) h[k].call(this, arg);
        return this;
    },
    fire: function (arg) {
        this.targets.forEach(function (t) {
            function go() { find(t.name).forEach(function (e) { e.send("trigger", arg); }); }
            if (t.delay > 0) after(t.delay, go);
            else go();
        });
        return this;
    }
};
function find(name) {
    if (!name) return [];
    return __E.filter(function (e) { return e.name === name; });
}
function log() { __native.print(Array.prototype.join.call(arguments, " ")); }
function after(seconds, fn) {
    var id = __nextTimer++;
    __timers.push({ id: id, at: __time + seconds, fn: fn });
    return id;
}
function every(seconds, fn) {
    var id = __nextTimer++;
    __timers.push({ id: id, at: __time + seconds, fn: fn, repeat: Math.max(seconds, 0.001) });
    return id;
}
function cancel(id) { __timers = __timers.filter(function (t) { return t.id !== id; }); }
function __guard(fn) {
    try { fn(); } catch (e) { __native.error(String(e) + (e.lineNumber ? " (line " + e.lineNumber + ")" : "")); }
}
function __add(i, info) { __E[i] = new __Entity(i, info); }
function __load(i, body) {
    var self = __E[i];
    function on(event, fn) { (self._h[event] = self._h[event] || []).push(fn); }
    __guard(function () { body(self, on, after, every, cancel, find, log, game); });
}
function __event(i, event) { __guard(function () { __E[i].send(event); }); }
function __update(dt) {
    __time += dt;
    var due = __timers.filter(function (t) { return t.at <= __time; });
    due.forEach(function (t) {
        if (t.repeat) t.at += t.repeat;
        else cancel(t.id);
        __guard(t.fn);
    });
    __E.forEach(function (e) {
        __guard(function () {
            if (e._a) e._step(dt);
            if (e._h.tick) e.send("tick", dt);
        });
    });
}
)JS";

void setTargetsProperty(QJSEngine* engine, QJSValue& info, const std::vector<EntityTarget>& targets) {
    QJSValue list = engine->newArray((uint)targets.size());
    for (size_t k = 0; k < targets.size(); ++k) {
        QJSValue t = engine->newObject();
        t.setProperty(QStringLiteral("name"), QString::fromStdString(targets[k].name));
        t.setProperty(QStringLiteral("delay"), targets[k].delay);
        list.setProperty((quint32)k, t);
    }
    info.setProperty(QStringLiteral("targets"), list);
}

QString wrap(const QString& code) {
    // On one line with the script's first: messages count the script's own lines.
    return QStringLiteral("(function (self, on, after, every, cancel, find, log, game) {") + code + QStringLiteral("\n})");
}

const char* const kClasses[] = {"func_door", "func_button", "func_actor"};

std::vector<ScriptPreset> builtInPresets() {
    return {
        {QStringLiteral("Door: open, close by itself"), QStringLiteral("func_door"), QStringLiteral(R"JS(// Opens when pressed, walked into or triggered, and closes again.
var closed = self.states[0], open = self.states[1];
var openTime = 1.0, staysOpen = 3.0;
if (self.states.length < 2) { log(self.name, "needs two states: open the component and press F5 twice"); return; }

var timer = 0;
function openDoor() {
    cancel(timer);
    self.tween(open, { duration: openTime }, function () {
        timer = after(staysOpen, function () { self.tween(closed, { duration: openTime }); });
    });
}
on("use", openDoor);
on("touch", openDoor);
on("trigger", openDoor);
)JS"), true, QString()},
        {QStringLiteral("Door: toggle"), QStringLiteral("func_door"), QStringLiteral(R"JS(// Each press or trigger opens it or closes it; stays as it is left.
var closed = self.states[0], open = self.states[1];
var openTime = 1.0;
if (self.states.length < 2) { log(self.name, "needs two states: open the component and press F5 twice"); return; }

var isOpen = false;
function toggle() {
    isOpen = !isOpen;
    self.tween(isOpen ? open : closed, { duration: openTime });
}
on("use", toggle);
on("trigger", toggle);
)JS"), true, QString()},
        {QStringLiteral("Button: press and return"), QStringLiteral("func_button"), QStringLiteral(R"JS(// Goes in, fires its target, comes back out.
var up = self.states[0], down = self.states[1];
if (self.states.length < 2) { log(self.name, "needs two states: open the component and press F5 twice"); return; }

on("use", function () {
    if (self.moving) return;
    self.tween(down, { duration: 0.12 }, function () {
        self.fire();
        after(0.4, function () { self.tween(up, { duration: 0.2 }); });
    });
});
)JS"), true, QString()},
        {QStringLiteral("Button: latching switch"), QStringLiteral("func_button"), QStringLiteral(R"JS(// Stays in until pressed again; fires both ways.
var up = self.states[0], down = self.states[1];
if (self.states.length < 2) { log(self.name, "needs two states: open the component and press F5 twice"); return; }

var pressed = false;
on("use", function () {
    if (self.moving) return;
    pressed = !pressed;
    self.tween(pressed ? down : up, { duration: 0.15 }, function () { self.fire(pressed); });
});
)JS"), true, QString()},
        {QStringLiteral("Actor: loop through states"), QStringLiteral("func_actor"), QStringLiteral(R"JS(// Plays every state as a keyframe, forever, from the start.
var secondsPerState = 1.0;
on("start", function () {
    self.play(self.states, { duration: secondsPerState, easing: "linear", loop: true });
});
)JS"), true, QString()},
        {QStringLiteral("Actor: back and forth"), QStringLiteral("func_actor"), QStringLiteral(R"JS(// Between the first and the last state and back, with soft ends.
var first = self.states[0], last = self.states[self.states.length - 1];
on("start", function () {
    self.play([last, first], { duration: 2.0, easing: "inOutCubic", loop: true });
});
)JS"), true, QString()},
        {QStringLiteral("Actor: play once when triggered"), QStringLiteral("func_actor"), QStringLiteral(R"JS(// Runs through its states each time it is pressed or triggered.
function run() {
    if (self.moving) return;
    self.set(self.states[0]);
    self.play(self.states.slice(1), { duration: 1.0 });
}
on("use", run);
on("trigger", run);
)JS"), true, QString()},
    };
}

} // namespace

const char* const* entityClasses(int* count) {
    *count = 3;
    return kClasses;
}

std::vector<ScriptPreset> scriptPresets() {
    std::vector<ScriptPreset> out = builtInPresets();
    const QDir dir(assetDir(QStringLiteral("scripts")));
    for (const QString& f : dir.entryList({QStringLiteral("*.js")}, QDir::Files, QDir::Name)) {
        QFile file(dir.filePath(f));
        if (!file.open(QIODevice::ReadOnly)) continue;
        ScriptPreset p;
        p.builtIn = false;
        p.path = file.fileName();
        p.name = QFileInfo(f).completeBaseName();
        p.code = QString::fromUtf8(file.readAll());
        // "// class: func_door" on the first line limits it to that class.
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(^//\s*class:\s*(\w+))")).match(p.code);
        if (m.hasMatch()) p.classname = m.captured(1);
        out.push_back(std::move(p));
    }
    return out;
}

QString saveScriptPreset(const QString& name, const QString& classname, const QString& code) {
    QString safe = name.trimmed();
    safe.replace(QRegularExpression(QStringLiteral(R"([/\\:*?"<>|\x00-\x1f])")), QStringLiteral("_"));
    while (safe.startsWith('.')) safe.remove(0, 1);
    if (safe.isEmpty()) return {};
    const QString path = QDir(assetDir(QStringLiteral("scripts"))).filePath(safe + QStringLiteral(".js"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    QString text = code;
    text.remove(QRegularExpression(QStringLiteral(R"(^//\s*class:[^\n]*\n)")));
    if (!classname.isEmpty()) text = QStringLiteral("// class: %1\n").arg(classname) + text;
    f.write(text.toUtf8());
    return f.commit() ? path : QString();
}

QString defaultScript(const std::string& classname) {
    const QString c = QString::fromStdString(classname);
    for (const ScriptPreset& p : builtInPresets())
        if (p.classname == c) return p.code;
    return {};
}

bool checkEntityScript(const QString& code, QString* error) {
    QJSEngine engine;
    const QJSValue v = engine.evaluate(wrap(code), QStringLiteral("script"));
    if (!v.isError()) return true;
    if (error) *error = QStringLiteral("Line %1: %2").arg(v.property(QStringLiteral("lineNumber")).toInt()).arg(v.toString());
    return false;
}

void ScriptBridge::setPose(int i, int a, int b, double t) { world_->setPose(i, a, b, t); }
void ScriptBridge::print(const QString& text) {
    std::printf("script: %s\n", qPrintable(text));
    std::fflush(stdout);
}
void ScriptBridge::error(const QString& text) { world_->addError(text); }

// A script stuck in a loop must not freeze the game: it gets interrupted.
struct EntityWorld::Watchdog {
    QJSEngine* engine;
    std::atomic<bool> quit{false};
    std::atomic<int64_t> startedMs{0};
    std::thread thread;
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    explicit Watchdog(QJSEngine* e) : engine(e) {
        thread = std::thread([this] {
            while (!quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const int64_t s = startedMs;
                if (s != 0 && now() - s > 2000) engine->setInterrupted(true);
            }
        });
    }
    ~Watchdog() {
        quit = true;
        thread.join();
    }
    struct Scope {
        Watchdog& w;
        explicit Scope(Watchdog& d) : w(d) { w.startedMs = now(); }
        ~Scope() { w.startedMs = 0; }
    };
};

EntityWorld::EntityWorld(ModelState& state) : state_(state) {
    entities_ = prepareEntities(state_);
    linked_.assign(entities_.size(), 0);
    touching_.assign(entities_.size(), 0);
    for (size_t i = 0; i < entities_.size(); ++i)
        for (size_t j = 0; j < entities_.size(); ++j)
            for (const EntityTarget& t : entities_[j].props.targets)
                if (i != j && !t.name.empty() && t.name == entities_[i].props.targetname) linked_[i] = 1;
}

EntityWorld::~EntityWorld() {
    watchdog_.reset();
    engine_.reset(); // before the bridge it holds
}

void EntityWorld::addError(const QString& text) {
    if (errors_.size() < 20 && !errors_.contains(text)) errors_ << text;
    std::fprintf(stderr, "script error: %s\n", qPrintable(text));
}

void EntityWorld::setPose(int i, int a, int b, double t) {
    if (i < 0 || i >= (int)entities_.size()) return;
    AnimEntity& e = entities_[i];
    e.poseA = a;
    e.poseB = b;
    e.poseT = t;
    const bool shape = poseEntity(state_, e, a, b, t);
    auto syncBodies = [this](const AnimEntity& en, bool changedShape) {
        if (!world_) return;
        for (const AnimPart& p : en.parts) {
            if (p.body < 0) continue;
            const Mat4& xf = state_.instances[p.instance].xf;
            if (changedShape && p.deforms) world_->setBodyMesh(p.body, state_.defs[p.def].mesh, xf);
            else world_->setBodyTransform(p.body, xf);
        }
    };
    syncBodies(e, shape);
    // What is inside it rides along.
    poseMounted(state_, entities_, i, [&](int j) { syncBodies(entities_[j], false); });
}

int EntityWorld::entityOfBody(int body) const {
    for (int i = 0; i < (int)entities_.size(); ++i)
        for (const AnimPart& p : entities_[i].parts)
            if (p.body == body) return i;
    return -1;
}

bool EntityWorld::usable(int i) const {
    if (i < 0 || i >= count()) return false;
    return !linked_[i] || entities_[i].props.classname == "func_button";
}

bool EntityWorld::call(const QString& function, int entity, const QString& event) {
    if (!engine_) return false;
    Watchdog::Scope scope(*watchdog_);
    QJSValue fn = engine_->globalObject().property(function);
    const QJSValue r = fn.call({entity, event});
    engine_->setInterrupted(false);
    if (r.isError()) addError(r.toString());
    return !r.isError();
}

void EntityWorld::start(CollisionWorld& world) {
    world_ = &world;
    for (AnimEntity& e : entities_)
        for (AnimPart& p : e.parts) p.body = world.addBody(state_.defs[p.def].mesh, state_.instances[p.instance].xf);
    engine_ = std::make_unique<QJSEngine>();
    watchdog_ = std::make_unique<Watchdog>(engine_.get());
    bridge_ = std::make_unique<ScriptBridge>(this);
    QJSEngine::setObjectOwnership(bridge_.get(), QJSEngine::CppOwnership);
    engine_->globalObject().setProperty(QStringLiteral("__native"), engine_->newQObject(bridge_.get()));
    {
        const QJSValue v = engine_->evaluate(QString::fromUtf8(kPrelude), QStringLiteral("prelude"));
        if (v.isError()) addError(QStringLiteral("prelude: %1").arg(v.toString()));
    }
    QJSValue add = engine_->globalObject().property(QStringLiteral("__add"));
    QJSValue load = engine_->globalObject().property(QStringLiteral("__load"));
    for (int i = 0; i < count(); ++i) {
        const AnimEntity& e = entities_[i];
        // The starting pose.
        int start = -1;
        for (int k = 0; k < (int)e.states.size(); ++k)
            if (e.states[k] == e.props.startState) start = k;
        if (start < 0 && !e.states.empty()) start = 0;
        setPose(i, start, start, 0); // parents come first, and carry the entities inside
        QJSValue info = engine_->newObject();
        info.setProperty(QStringLiteral("name"), QString::fromStdString(e.props.targetname));
        info.setProperty(QStringLiteral("classname"), QString::fromStdString(e.props.classname));
        setTargetsProperty(engine_.get(), info, e.props.targets);
        info.setProperty(QStringLiteral("linked"), (bool)linked_[i]);
        info.setProperty(QStringLiteral("start"), start >= 0 ? QString::fromStdString(e.states[start]) : QString());
        QJSValue names = engine_->newArray((uint)e.states.size());
        for (int k = 0; k < (int)e.states.size(); ++k) names.setProperty((quint32)k, QString::fromStdString(e.states[k]));
        info.setProperty(QStringLiteral("states"), names);
        add.call({i, info});
    }
    // All entities exist before any script runs: find() sees them all.
    for (int i = 0; i < count(); ++i) {
        const AnimEntity& e = entities_[i];
        const QString label = QStringLiteral("%1 %2").arg(QString::fromStdString(e.props.classname), QString::fromStdString(e.props.targetname)).trimmed();
        Watchdog::Scope scope(*watchdog_);
        const QString code = e.props.script.empty() ? defaultScript(e.props.classname) : QString::fromStdString(e.props.script);
        const QJSValue body = engine_->evaluate(wrap(code), label);
        if (body.isError()) {
            addError(QStringLiteral("%1, line %2: %3").arg(label).arg(body.property(QStringLiteral("lineNumber")).toInt()).arg(body.toString()));
            continue;
        }
        const QJSValue r = load.call({i, body});
        engine_->setInterrupted(false);
        if (r.isError()) addError(QStringLiteral("%1: %2").arg(label, r.toString()));
    }
    for (int i = 0; i < count(); ++i) call(QStringLiteral("__event"), i, QStringLiteral("start"));
}

void EntityWorld::update(double dt) {
    if (!engine_) return;
    Watchdog::Scope scope(*watchdog_);
    QJSValue fn = engine_->globalObject().property(QStringLiteral("__update"));
    const QJSValue r = fn.call({dt});
    engine_->setInterrupted(false);
    if (r.isError()) addError(r.toString());
}

void EntityWorld::use(int i) {
    if (usable(i)) call(QStringLiteral("__event"), i, QStringLiteral("use"));
}

void EntityWorld::setTouching(int i, bool touching) {
    if (i < 0 || i >= count() || (bool)touching_[i] == touching) return;
    touching_[i] = touching;
    if (touching && !linked_[i]) call(QStringLiteral("__event"), i, QStringLiteral("touch"));
}

} // namespace gs

