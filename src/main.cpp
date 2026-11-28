// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - entry point.
#include "app/GameMode.h"
#include "app/MainWindow.h"
#include "app/SelfTest.h"
#include "app/Theme.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTimer>

namespace {

// Copies the files under `from` that `to` does not have yet.
void copyMissing(const QString& from, const QString& to) {
    const QDir src(from);
    if (!src.exists()) return;
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString dest = QDir(to).filePath(src.relativeFilePath(file));
        if (QFile::exists(dest)) continue;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile::copy(file, dest);
    }
}

// Carries over what GNUSketch (the name before Forma) kept: settings, and the
// user's prefabs, materials, textures, scripts and plugins. Runs once; the old
// files stay where they are.
void migrateFromGNUSketch() {
    QSettings now;
    if (now.value(QStringLiteral("migratedFromGNUSketch")).toBool()) return;
    QSettings old(QStringLiteral("GNUSketch"), QStringLiteral("GNUSketch"));
    if (now.allKeys().isEmpty())
        for (const QString& k : old.allKeys()) now.setValue(k, old.value(k));
    if (!qEnvironmentVariableIsSet("FORMA_DATA_DIR")) {
        const QDir data(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
        const QString oldRoot = data.filePath(QStringLiteral("GNUSketch"));
        for (const char* kind : {"prefabs", "materials", "textures", "scripts"})
            copyMissing(QDir(oldRoot).filePath(QLatin1String(kind)), data.filePath(QStringLiteral("Forma/") + QLatin1String(kind)));
        copyMissing(QDir(oldRoot).filePath(QStringLiteral("GNUSketch/plugins")),
                    QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/plugins"));
    }
    now.setValue(QStringLiteral("migratedFromGNUSketch"), true);
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
    QApplication::setApplicationName("Forma");
    QApplication::setOrganizationName("Dev1lroot Software");
    QApplication::setApplicationVersion(QStringLiteral(FORMA_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("forma"));
    migrateFromGNUSketch();
    forma::Theme::applyStored();
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/forma.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription("Dev1lroot Software: Forma, a native 3D sketching tool");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption selfTest("selftest", "Initialise the renderer, draw a few frames and exit (for CI; use with QT_QPA_PLATFORM=offscreen).");
    parser.addOption(selfTest);
    QCommandLineOption glProbe("gl-probe", "Create an offscreen OpenGL context, render one frame with the real renderer and exit (no window).");
    parser.addOption(glProbe);
    QCommandLineOption bench("render-bench", "Time off-screen frames of a model with each render setting and exit (no window).", "model");
    parser.addOption(bench);
    QCommandLineOption ibench("interact-bench", "Time mouse moves with each tool over the model given as the file, then exit.");
    parser.addOption(ibench);
    QCommandLineOption shots("screenshots", "Render the README pictures into a folder and exit (use tools/readme-media.sh).", "dir");
    parser.addOption(shots);
    QCommandLineOption game("game", "Walk around a saved model in the Game Mode window.", "model");
    parser.addOption(game);
    QCommandLineOption spawn("spawn", "Game Mode: where the character stands (x,y,z in meters).", "point", "0,0,0");
    parser.addOption(spawn);
    QCommandLineOption yaw("yaw", "Game Mode: direction the character looks (radians).", "angle", "0");
    parser.addOption(yaw);
    parser.addPositionalArgument("file", "Model to open (.forma or .obj)");
    parser.process(app);

    if (parser.isSet(glProbe)) return forma::runGlProbe();
    if (parser.isSet(bench)) return forma::runRenderBench(parser.value(bench), 120);

    if (parser.isSet(game)) {
        const QStringList p = parser.value(spawn).split(',');
        const forma::Vec3 feet = p.size() == 3 ? forma::Vec3(p[0].toDouble(), p[1].toDouble(), p[2].toDouble()) : forma::Vec3();
        return forma::runGameWindow(parser.value(game), feet, parser.value(yaw).toDouble());
    }

    forma::MainWindow w;
    if (!parser.positionalArguments().isEmpty()) w.openFile(parser.positionalArguments().first());
    w.show();

    if (parser.isSet(shots)) {
        const QString dir = parser.value(shots);
        QTimer::singleShot(300, &w, [&w, dir] { QApplication::exit(forma::runScreenshots(w, dir)); });
    }
    if (parser.isSet(ibench)) {
        QTimer::singleShot(300, &w, [&w] { QApplication::exit(forma::runInteractBench(w)); });
    }
    if (parser.isSet(selfTest)) {
        QTimer::singleShot(300, &w, [&w] { QApplication::exit(forma::runSelfTest(w)); });
    }
    return app.exec();
}
