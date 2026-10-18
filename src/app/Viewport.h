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
    void cancelTool();
    bool renderOk() const { return glOk_; }
    QString glError() const { return glError_; }
    // Applies an image file as texture to the face under (x, y). Returns true if applied.
    bool dropImage(const QString& path, double x, double y, bool tile);

signals:
    void statusText(const QString& text);
    void vcbChanged(const QString& label, const QString& value);
    void toolChanged(gs::ToolId id);
    void materialPicked();
    void contextMenuAt(const QPoint& globalPos, gs::PickResult pick);
    void imageDropped(const QString& path);
    void glFailed(const QString& log);

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
    Vec3 navGrab_;
    bool navMoved_ = false;
    QPointF mousePos_;
    bool mouseInside_ = false;
    int dropHoverFace_ = -1;
    QString vcbLabel_;
    QString vcbTyped_; // characters typed into the Measurements box from the viewport
};

} // namespace gs
