// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - entry point.
#include "app/MainWindow.h"
#include "app/SelfTest.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QPalette>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTimer>

namespace {

void applyLightTheme(QApplication& app) {
    // A light, classic desktop look regardless of the system theme.
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(240, 240, 240));
    p.setColor(QPalette::WindowText, Qt::black);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(246, 246, 246));
    p.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
    p.setColor(QPalette::ToolTipText, Qt::black);
    p.setColor(QPalette::Text, Qt::black);
    p.setColor(QPalette::Button, QColor(240, 240, 240));
    p.setColor(QPalette::ButtonText, Qt::black);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Highlight, QColor(0, 120, 215));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0, 102, 204));
    p.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(150, 150, 150));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(150, 150, 150));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(150, 150, 150));
    app.setPalette(p);
    app.setStyleSheet(R"(
        QMainWindow::separator { background: #d8d8d8; width: 4px; height: 4px; }
        QToolBar { background: #f0f0f0; border: none; spacing: 1px; padding: 2px; }
        QToolBar::separator { background: #c8c8c8; width: 1px; height: 1px; margin: 4px; }
        QToolButton { border: 1px solid transparent; border-radius: 2px; padding: 2px; }
        QToolButton:hover { border: 1px solid #9cc4ea; background: #e5f1fb; }
        QToolButton:checked { border: 1px solid #569de5; background: #cce4f7; }
        QToolButton#TrayHeader { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #fafafa, stop:1 #e6e6e6);
                                 border: 1px solid #cfcfcf; border-radius: 0; text-align: left; padding: 3px 4px; font-weight: 600; }
        QToolButton#TrayHeader:hover { background: #e9f2fb; }
        QToolButton#FlatTab { border: none; border-bottom: 2px solid transparent; padding: 2px 8px; }
        QToolButton#FlatTab:checked { background: transparent; border-bottom: 2px solid #3b6fb6; }
        QDockWidget::title { background: #e9e9e9; padding: 4px; border-bottom: 1px solid #d0d0d0; }
        QStatusBar { background: #f0f0f0; border-top: 1px solid #d0d0d0; }
        QStatusBar::item { border: none; }
        QLineEdit#Measurements { border: 1px solid #a8a8a8; padding: 1px 4px; background: white; }
    )");
}

} // namespace

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
    QApplication::setApplicationVersion("0.1.0");
    applyLightTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription("Native 3D sketching tool");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption selfTest("selftest", "Initialise the renderer, draw a few frames and exit (for CI; use with QT_QPA_PLATFORM=offscreen).");
    parser.addOption(selfTest);
    QCommandLineOption glProbe("gl-probe", "Create an offscreen OpenGL context, render one frame with the real renderer and exit (no window).");
    parser.addOption(glProbe);
    parser.addPositionalArgument("file", "Model to open (.gsketch or .obj)");
    parser.process(app);

    if (parser.isSet(glProbe)) return gs::runGlProbe();

    gs::MainWindow w;
    if (!parser.positionalArguments().isEmpty()) w.openFile(parser.positionalArguments().first());
    w.show();

    if (parser.isSet(selfTest)) {
        QTimer::singleShot(300, &w, [&w] { QApplication::exit(gs::runSelfTest(w)); });
    }
    return app.exec();
}
