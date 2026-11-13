// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the 3D modelling viewport (OpenGL widget hosting the tools).
#pragma once

#include "Inference.h"
#include "Tool.h"
#include "core/Document.h"
#include "render/Camera.h"
#include "render/Renderer.h"

#include <QOpenGLWidget>
#include <QPointF>

#include <memory>

class QTimer;

namespace gs {

class Viewport : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit Viewport(Document* doc, QWidget* parent = nullptr);
    ~Viewport() override;

    Document& doc() { return *doc_; }
    Camera& camera() { return camera_; }
    InferenceEngine& inference() { return *inference_; }
    Tool* tool() const { return tool_.get(); }
    ToolId toolId() const { return tool_ ? tool_->id() : ToolId::Select; }

    void setTool(ToolId id);
    void refreshToolStatus();
    void setVcbValue(const QString& v);
    void vcbEnter(const QString& text);
    void cameraChanged();
    void materialSampled() { emit materialPicked(); }
    void zoomExtents();
    void setStandardView(const Vec3& dir);
    void setPerspective(bool on);
    // What orbiting rotates around: the geometry under the cursor (falling back
    // to the selection, the model and the world origin), or the camera's own
    // target point.
    enum class OrbitPivot { Geometry, CameraTarget };
    OrbitPivot orbitPivot() const { return orbitPivot_; }
    void setOrbitPivot(OrbitPivot p) { orbitPivot_ = p; }
    Vec3 orbitPivotAt(double x, double y);
    void cancelTool();
    bool renderOk() const { return glOk_; }
    QString glError() const { return glError_; }
    // Applies an image file as texture to the face under (x, y). Returns true if applied.
    bool dropImage(const QString& path, double x, double y, bool tile);
    // Places an image file as a textured rectangle (1 px = 1 mm) on the axis
    // plane that faces the camera most, centred under (x, y).
    bool addImagePlane(const QString& path, double x, double y);
    bool addImagePlane(Texture tex, const QString& name, double x, double y);
    // Moves the view to a camera placed in the world.
    void lookThrough(Id object);
    // A light or camera where the view looks from, aimed at what it looks at.
    SceneObject objectFromView(SceneObject::Kind kind) const;
    // Point on a surface under (x, y), or on the ground; false if neither is hit.
    bool surfacePointAt(double x, double y, Vec3& out);
    // Puts an instance of a prefab with its base centre at `at`, as one undo
    // step, and selects it. Returns the instance, kNone when it cannot be placed.
    Id placePrefab(const ModelState& prefab, const Vec3& at);
    // Applies a material (as if with the Paint Bucket) to what is under (x, y).
    bool dropMaterial(const Material& m, double x, double y, Qt::KeyboardModifiers mods);
    // What the browsers drag: a material as JSON, a prefab as its file path.
    static const char* materialMime() { return "application/x-gnusketch-material"; }
    static const char* prefabMime() { return "application/x-gnusketch-prefab"; }

signals:
    void statusText(const QString& text);
    void vcbChanged(const QString& label, const QString& value);
    void toolChanged(gs::ToolId id);
    void materialPicked();
    void contextMenuAt(const QPoint& globalPos, gs::PickResult pick);
    void imageDropped(const QString& path);
    void glFailed(const QString& log);
    // Game Mode: where the character was placed and which way it looks (radians).
    void characterPlaced(const gs::Vec3& feet, double yaw);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    MouseEvent toEvent(QMouseEvent* e) const;
    void paintOverlay(QPainter& p, const Preview& pv);
    void updateSceneBounds();

    Document* doc_;
    Camera camera_;
    Renderer renderer_;
    std::unique_ptr<InferenceEngine> inference_;
    std::unique_ptr<Tool> tool_;
    bool glOk_ = false;
    QString glError_;

    // Temporary navigation with the middle mouse button.
    enum class Nav { None, Orbit, Pan };
    Nav nav_ = Nav::None;
    QPointF navLast_;
    Vec3 navGrab_, navPivot_;
    OrbitPivot orbitPivot_ = OrbitPivot::Geometry;
    bool navMoved_ = false;
    QPointF mousePos_;
    bool mouseInside_ = false;
    int dropHoverFace_ = -1;
    // Prefab being dragged over the view: its file and where it would land.
    QString dragPrefabPath_;
    std::unique_ptr<ModelState> dragPrefab_;
    bool dragPrefabHit_ = false;
    Vec3 dragPrefabAt_;
    QTimer* animTimer_ = nullptr; // repaints while a custom shader uses _Time
    QString vcbLabel_;
    QString vcbTyped_; // characters typed into the Measurements box from the viewport
    // X/Y/Z held down: which one, and for which tool (it may change meanwhile).
    int heldAxisArrow_ = 0;
    const Tool* heldAxisTool_ = nullptr;
    static int axisArrowForKey(int key);
    static int axisArrowFor(const QKeyEvent* e);
};

} // namespace gs
