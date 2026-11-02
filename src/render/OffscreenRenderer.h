// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - renders thumbnails and checks shaders without any window.
#pragma once

#include "core/Document.h"
#include "render/Camera.h"

#include <QImage>
#include <QObject>

#include <memory>

class QOffscreenSurface;
class QOpenGLContext;

namespace gs {

class Renderer;

class OffscreenRenderer : public QObject {
    Q_OBJECT
public:
    // The shared instance; it frees its GL objects when the application quits.
    static OffscreenRenderer& instance();
    ~OffscreenRenderer() override;

    // A ball with the material on it (null image if OpenGL is unavailable).
    QImage materialBall(const Material& m, int size);
    // The model seen from a three-quarter view.
    QImage modelThumbnail(const ModelState& state, int size);
    bool compileShader(const std::string& source, QString* log);

private:
    OffscreenRenderer();
    bool ensure();
    QImage renderDocument(Document& doc, const Camera& cam, int size);
    void shutdown();

    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<Renderer> renderer_;
    bool failed_ = false;
};

} // namespace gs
