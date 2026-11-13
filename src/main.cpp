// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - entry point.
#include "app/GameMode.h"
#include "app/MainWindow.h"
#include "app/SelfTest.h"
#include "app/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QTimer>


int main(int argc, char** argv) {
    QSurfaceFormat fmt;
    // Desktop OpenGL must be requested explicitly: on Wayland/EGL Qt otherwise
    // assumes OpenGL ES, and "GLES 3.3 core" fails with EGL_BAD_MATCH (3009).
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(fmt); // MSAA is requested by the viewport's own framebuffer


    QApplication app(argc, argv);
    QApplication::setApplicationName("GNUSketch");
    QApplication::setOrganizationName("GNUSketch");
    QApplication::setApplicationVersion(QStringLiteral(GNUSKETCH_VERSION));
    gs::Theme::applyStored();

    QCommandLineParser parser;
    parser.setApplicationDescription("Native 3D sketching tool");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption selfTest("selftest", "Initialise the renderer, draw a few frames and exit (for CI; use with QT_QPA_PLATFORM=offscreen).");
    parser.addOption(selfTest);
    QCommandLineOption glProbe("gl-probe", "Create an offscreen OpenGL context, render one frame with the real renderer and exit (no window).");
    parser.addOption(glProbe);
    QCommandLineOption shots("screenshots", "Render the README pictures into a folder and exit (use tools/readme-media.sh).", "dir");
    parser.addOption(shots);
    QCommandLineOption game("game", "Walk around a saved model in the Game Mode window.", "model");
    parser.addOption(game);
    QCommandLineOption spawn("spawn", "Game Mode: where the character stands (x,y,z in meters).", "point", "0,0,0");
    parser.addOption(spawn);
    QCommandLineOption yaw("yaw", "Game Mode: direction the character looks (radians).", "angle", "0");
    parser.addOption(yaw);
    parser.addPositionalArgument("file", "Model to open (.gsketch or .obj)");
    parser.process(app);

    if (parser.isSet(glProbe)) return gs::runGlProbe();

    if (parser.isSet(game)) {
        const QStringList p = parser.value(spawn).split(',');
        const gs::Vec3 feet = p.size() == 3 ? gs::Vec3(p[0].toDouble(), p[1].toDouble(), p[2].toDouble()) : gs::Vec3();
        return gs::runGameWindow(parser.value(game), feet, parser.value(yaw).toDouble());
    }

    gs::MainWindow w;
    if (!parser.positionalArguments().isEmpty()) w.openFile(parser.positionalArguments().first());
    w.show();

    if (parser.isSet(shots)) {
        const QString dir = parser.value(shots);
        QTimer::singleShot(300, &w, [&w, dir] { QApplication::exit(gs::runScreenshots(w, dir)); });
    }
    if (parser.isSet(selfTest)) {
        QTimer::singleShot(300, &w, [&w] { QApplication::exit(gs::runSelfTest(w)); });
    }
    return app.exec();
}
