// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - Game Mode: place a character on the model, then walk around in a
// separate window (WASD + mouse).
//
// The game window is its own process working on a snapshot of the model. On
// Wayland that process runs through XWayland, because a Wayland client cannot
// warp the pointer: mouse look needs an endless relative motion, which XWayland
// provides for a hidden cursor on every common compositor.
#pragma once

#include "Tool.h"
#include "core/Walker.h"
#include "render/Camera.h"
#include "render/Renderer.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPoint>

#include <set>

namespace gs {

class PlaceCharacterTool : public Tool {
public:
    explicit PlaceCharacterTool(Viewport* vp) : Tool(vp) {}
    ToolId id() const override { return ToolId::PlaceCharacter; }
    QString status() const override;
    void mouseMove(const MouseEvent& e) override;
    void mousePress(const MouseEvent& e) override;
    void buildPreview(Preview& pv) override;

private:
    bool spotUnder(const MouseEvent& e, Vec3& feet);
    bool hasSpot_ = false;
    Vec3 spot_;
};

class GameWindow : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit GameWindow(QWidget* parent = nullptr);
    ~GameWindow() override;

    // Loads a saved model and places the character.
    bool load(const QString& path, const Vec3& feet, double yaw, QString* error = nullptr);
    Walker& walker() { return walker_; }
    const CollisionWorld& world() const { return world_; }
    bool renderOk() const { return glOk_; }
    bool mouseCaptured() const { return captured_; }
    // Runs the simulation with the keys currently held.
    void simulate(double seconds);

protected:
    void initializeGL() override;
    void paintGL() override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    enum Action { Forward, Back, Left, Right, Run, Jump, LookLeft, LookRight, LookUp, LookDown };
    static int actionOf(const QKeyEvent* e);
    void setCaptured(bool on);
    QPoint centre() const;

    Document doc_;
    CollisionWorld world_;
    Walker walker_;
    Vec3 spawn_;
    double spawnYaw_ = 0;
    Camera camera_;
    Renderer renderer_;
    bool glOk_ = false;
    QString glError_;
    std::set<int> held_;
    QElapsedTimer clock_;
    bool captured_ = false;
    bool canWarp_ = true;
    bool skipMotion_ = false;
    QPoint lastPos_;
};

// Runs the game window in this process (the `--game` command line). Returns the exit code.
int runGameWindow(const QString& path, const Vec3& feet, double yaw);

} // namespace gs
