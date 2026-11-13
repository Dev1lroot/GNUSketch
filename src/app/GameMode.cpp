// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameMode.h"

#include "Viewport.h"
#include "core/Components.h"
#include "io/NativeFormat.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTimer>

#include <cmath>

namespace gs {

// ---------------------------------------------------------------------------
// Placing the character
// ---------------------------------------------------------------------------

QString PlaceCharacterTool::status() const {
    return QStringLiteral("Click where the character should stand (it is 1.80 m tall). A game window opens: WASD + mouse to walk.");
}

bool PlaceCharacterTool::spotUnder(const MouseEvent& e, Vec3& feet) {
    PickResult pr = engine().pick(e.x, e.y, 3.0);
    if (pr.face != kNone || pr.edge != kNone || pr.instance != kNone) {
        feet = pr.p;
        return true;
    }
    Ray r = cam().ray(e.x, e.y);
    double t;
    if (r.dir.z < -1e-6 && intersectRayPlane(r, Plane(Vec3(0, 0, 1), Vec3()), t)) {
        feet = r.at(t);
        return true;
    }
    return false;
}

void PlaceCharacterTool::mouseMove(const MouseEvent& e) {
    lastMouse_ = e;
    hasSpot_ = spotUnder(e, spot_);
    update();
}

void PlaceCharacterTool::mousePress(const MouseEvent& e) {
    if (e.button != Qt::LeftButton || !spotUnder(e, spot_)) return;
    const Vec3 f = cam().forward();
    const double yaw = std::atan2(f.y, f.x);
    Viewport* vp = vp_;
    const Vec3 feet = spot_;
    // Replaces this tool, so not from inside its own event handler.
    QTimer::singleShot(0, vp, [vp, feet, yaw] {
        vp->setTool(ToolId::Select);
        emit vp->characterPlaced(feet, yaw);
    });
}

void PlaceCharacterTool::buildPreview(Preview& pv) {
    if (!hasSpot_) return;
    // The character's collision capsule.
    const WalkerParams p;
    const Color c = Color::rgb8(20, 150, 60);
    constexpr int kSides = 20;
    auto ring = [&](double z, double radius) {
        for (int i = 0; i < kSides; ++i) {
            const double a0 = 2 * kPi * i / kSides, a1 = 2 * kPi * (i + 1) / kSides;
            pv.line(spot_ + Vec3(std::cos(a0) * radius, std::sin(a0) * radius, z),
                    spot_ + Vec3(std::cos(a1) * radius, std::sin(a1) * radius, z), c, 1.5f);
        }
    };
    ring(p.radius, p.radius);
    ring(p.height - p.radius, p.radius);
    ring(p.radius * 0.25, p.radius * 0.66);
    ring(p.height - p.radius * 0.25, p.radius * 0.66);
    for (int i = 0; i < 4; ++i) {
        const double a = kPi / 2 * i;
        const Vec3 d(std::cos(a) * p.radius, std::sin(a) * p.radius, 0);
        pv.line(spot_ + d + Vec3(0, 0, p.radius), spot_ + d + Vec3(0, 0, p.height - p.radius), c, 1.5f);
    }
    pv.markers.push_back({spot_, Preview::MarkerKind::Plain});
}

// ---------------------------------------------------------------------------
// The game window
// ---------------------------------------------------------------------------

GameWindow::GameWindow(QWidget* parent) : QOpenGLWidget(parent), walker_(world_) {
    setWindowTitle(QStringLiteral("GNUSketch - Game Mode"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    resize(1280, 720);
    QSurfaceFormat fmt = format();
    fmt.setSamples(4);
    setFormat(fmt);
    canWarp_ = !QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
    camera_.fovY = 60;
    camera_.perspective = true;
    // Keeps animating: each finished frame asks for the next.
    connect(this, &QOpenGLWidget::frameSwapped, this, qOverload<>(&QWidget::update));
}

GameWindow::~GameWindow() {
    if (context()) {
        makeCurrent();
        renderer_.cleanup();
        doneCurrent();
    }
}

bool GameWindow::load(const QString& path, const Vec3& feet, double yaw, QString* error) {
    QJsonObject view;
    if (!loadNative(doc_, path, &view, error)) return false;
    if (view.contains("style")) styleFromJson(view["style"].toObject(), doc_.style);
    // Only the world shows: no modelling aids, and a style one can walk around in.
    RenderStyle& s = doc_.style;
    s.showAxes = s.showGuides = s.showHidden = s.showObjects = false;
    s.xray = s.backEdges = false;
    s.edges = false;
    if (s.faceStyle == RenderStyle::Wireframe || s.faceStyle == RenderStyle::HiddenLine) s.faceStyle = RenderStyle::ShadedTextures;
    // Real sun shadows: always on here, sharp near the player with soft,
    // contact-hardening edges (Renderer::setSoftShadows).
    s.shadows = true;
    doc_.selection.clear();

    // Entities first: their parts leave the static collision world.
    entities_ = std::make_unique<EntityWorld>(doc_.state);
    world_.buildFromModel(doc_.state);
    entities_->start(world_);
    spawn_ = feet;
    spawnYaw_ = yaw;
    walker_.spawn(feet);
    walker_.setYaw(yaw);

    Vec3 mn, mx;
    bool any;
    modelBounds(doc_.state, mn, mx, any);
    if (any) camera_.setSceneRadius((mn + mx) * 0.5, (mx - mn).length() * 0.5);
    // The sharp shadow area: the whole of a small model, 40 m around the
    // player in a large one (the rest uses the coarser whole-model map).
    shadowRadius_ = any ? std::clamp((mx - mn).length() * 0.6, 6.0, 40.0) : 40.0;
    renderer_.setSoftShadows(true, shadowRadius_);
    return true;
}

void GameWindow::initializeGL() {
    glOk_ = renderer_.initialize(&glError_);
    renderer_.setSoftShadows(true, shadowRadius_);
    clock_.start();
}

void GameWindow::simulate(double seconds) {
    WalkInput in;
    auto holds = [&](int a) { return held_.count(a) != 0; };
    in.forward = (holds(Forward) ? 1 : 0) - (holds(Back) ? 1 : 0);
    in.strafe = (holds(Right) ? 1 : 0) - (holds(Left) ? 1 : 0);
    in.run = holds(Run);
    in.jump = holds(Jump);
    const double turn = 2.0 * seconds;
    walker_.look(((holds(LookLeft) ? 1 : 0) - (holds(LookRight) ? 1 : 0)) * turn,
                 ((holds(LookUp) ? 1 : 0) - (holds(LookDown) ? 1 : 0)) * turn * 0.7);
    walker_.advance(seconds, in);
    if (entities_) {
        entities_->update(seconds);
        // Which way the character is trying to go, whether or not it gets there.
        const Vec3 facing(std::cos(walker_.yaw()), std::sin(walker_.yaw()), 0);
        const Vec3 side(std::sin(walker_.yaw()), -std::cos(walker_.yaw()), 0);
        Vec3 wish = facing * in.forward + side * in.strafe;
        for (int i = 0; i < entities_->count(); ++i) {
            bool touching = false;
            for (const AnimPart& p : entities_->entity(i).parts)
                touching = touching || (p.body >= 0 && walker_.touches(p.body, 0.08));
            entities_->setTouching(i, touching);
            if (touching && entities_->isPhysical(i) && wish.length2() > 0.01) {
                // A person's shove, about 400 N at waist height: heavy things stay put.
                const Vec3 force = wish.normalized() * (in.run ? 600.0 : 400.0);
                entities_->pushBy(i, force, walker_.feet() + Vec3(0, 0, 1.0) + wish.normalized() * walker_.params().radius, seconds);
            }
        }
    }
}

int GameWindow::aimedEntity() const {
    if (!entities_ || entities_->count() == 0) return -1;
    double t = 0;
    int body = -1;
    if (!world_.raycast(walker_.eye(), walker_.lookDirection(), 2.5, t, body) || body < 0) return -1;
    return entities_->entityOfBody(body);
}

void GameWindow::useAimed() {
    const int e = aimedEntity();
    if (e >= 0) entities_->use(e);
}

void GameWindow::paintGL() {
    // A stall (a window drag, a slow frame) is not simulated as one long step of falling.
    simulate(std::min(clock_.isValid() ? clock_.restart() / 1000.0 : 0.0, 0.25));

    QPainter hud;
    camera_.setViewport(width(), height());
    camera_.eye = walker_.eye();
    // The target's distance sets the near plane: 25 m gives 5 cm, closer than
    // the capsule's radius, with good depth precision for the distance.
    camera_.target = camera_.eye + walker_.lookDirection() * 25.0;
    camera_.up = Vec3(0, 0, 1);
    if (glOk_) renderer_.render(doc_, camera_, Preview(), devicePixelRatioF());

    hud.begin(this);
    hud.setRenderHint(QPainter::Antialiasing, true);
    if (!glOk_) {
        hud.fillRect(rect(), QColor(40, 40, 40));
        hud.setPen(Qt::white);
        hud.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                     QStringLiteral("OpenGL 3.3 core profile is required but could not be initialised.\n\n") + glError_);
        return;
    }
    const QPoint c = rect().center();
    hud.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
    hud.drawEllipse(c, 2, 2);
    const QString text = captured_
        ? QStringLiteral("WASD walk   E press   Shift run   Space jump   R back to start   Tab edges   F5 cinematic lighting %1   F11 full screen   Esc free the mouse").arg(doc_.style.cinematic ? QStringLiteral("on") : QStringLiteral("off"))
        : QStringLiteral("Click to look around with the mouse   Esc again to leave");
    QFont f = hud.font();
    f.setPointSizeF(9.5);
    hud.setFont(f);
    QRect box = hud.fontMetrics().boundingRect(text).adjusted(-8, -4, 8, 4);
    box.moveTopLeft(QPoint(10, 10));
    hud.fillRect(box, QColor(0, 0, 0, 120));
    hud.setPen(Qt::white);
    hud.drawText(box, Qt::AlignCenter, text);
    if (entities_) {
        const int aimed = aimedEntity();
        if (aimed >= 0 && entities_->usable(aimed)) {
            const QString hint = QStringLiteral("E  press");
            QRect hb = hud.fontMetrics().boundingRect(hint).adjusted(-8, -4, 8, 4);
            hb.moveCenter(c + QPoint(0, 34));
            hud.fillRect(hb, QColor(0, 0, 0, 140));
            hud.drawText(hb, Qt::AlignCenter, hint);
        }
        // Script errors stay in sight, a broken door should not be a mystery.
        int y = box.bottom() + 8;
        for (const QString& err : entities_->errors()) {
            QRect eb = hud.fontMetrics().boundingRect(err).adjusted(-8, -3, 8, 3);
            eb.moveTopLeft(QPoint(10, y));
            hud.fillRect(eb, QColor(150, 20, 20, 190));
            hud.drawText(eb, Qt::AlignCenter, err);
            y = eb.bottom() + 3;
        }
    }
}

int GameWindow::actionOf(const QKeyEvent* e) {
    // By physical position first, so WASD also works on non-Latin keyboard layouts
    // (X keycodes, which Qt on Wayland reports the same).
    switch (e->nativeScanCode()) {
    case 25: return Forward; // W
    case 38: return Left;    // A
    case 39: return Back;    // S
    case 40: return Right;   // D
    default: break;
    }
    switch (e->key()) {
    case Qt::Key_W: return Forward;
    case Qt::Key_S: return Back;
    case Qt::Key_A: return Left;
    case Qt::Key_D: return Right;
    case Qt::Key_Shift: return Run;
    case Qt::Key_Space: return Jump;
    case Qt::Key_Left: return LookLeft;
    case Qt::Key_Right: return LookRight;
    case Qt::Key_Up: return LookUp;
    case Qt::Key_Down: return LookDown;
    default: return -1;
    }
}

void GameWindow::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    if (e->nativeScanCode() == 26) { // E by position, for non-Latin layouts
        useAimed();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Escape:
        if (captured_) setCaptured(false);
        else close();
        return;
    case Qt::Key_R:
        walker_.spawn(spawn_);
        walker_.setYaw(spawnYaw_);
        return;
    case Qt::Key_Tab:
        doc_.style.edges = !doc_.style.edges;
        return;
    case Qt::Key_E:
        useAimed();
        return;
    case Qt::Key_F11:
        setWindowState(windowState() ^ Qt::WindowFullScreen);
        return;
    case Qt::Key_F5:
        // Ray traced shadows from the sun and the lights, light rays.
        doc_.style.cinematic = !doc_.style.cinematic;
        update();
        return;
    default: break;
    }
    const int a = actionOf(e);
    if (a >= 0) held_.insert(a);
}

void GameWindow::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    const int a = actionOf(e);
    if (a >= 0) held_.erase(a);
}

QPoint GameWindow::centre() const { return rect().center(); }

void GameWindow::setCaptured(bool on) {
    if (on == captured_) return;
    captured_ = on;
    if (on) {
        setCursor(Qt::BlankCursor);
        grabMouse();
        setFocus();
        skipMotion_ = true;
        if (canWarp_) QCursor::setPos(mapToGlobal(centre()));
    } else {
        releaseMouse();
        unsetCursor();
    }
}

void GameWindow::mousePressEvent(QMouseEvent*) {
    if (captured_) useAimed();
    setCaptured(true);
}

void GameWindow::mouseMoveEvent(QMouseEvent* e) {
    if (!captured_) return;
    const QPoint now = e->position().toPoint();
    if (skipMotion_) {
        // The first report may still predate the warp to the centre.
        skipMotion_ = false;
        lastPos_ = now;
        if (canWarp_) QCursor::setPos(mapToGlobal(centre()));
        return;
    }
    const QPoint delta = now - (canWarp_ ? centre() : lastPos_);
    lastPos_ = now;
    if (delta.isNull()) return;
    constexpr double kSensitivity = 0.0025; // radians per pixel
    walker_.look(-delta.x() * kSensitivity, -delta.y() * kSensitivity);
    if (canWarp_) QCursor::setPos(mapToGlobal(centre()));
}

void GameWindow::focusOutEvent(QFocusEvent* e) {
    held_.clear();
    setCaptured(false);
    QOpenGLWidget::focusOutEvent(e);
}

int runGameWindow(const QString& path, const Vec3& feet, double yaw) {
    GameWindow w;
    QString error;
    if (!w.load(path, feet, yaw, &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Game Mode"), QStringLiteral("Could not load the model: %1").arg(error));
        return 1;
    }
    w.show();
    return QApplication::exec();
}

} // namespace gs
