// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include "Browsers.h"
#include "MaterialEditor.h"
#include "SceneTools.h"
#include "Hierarchy.h"
#include "EntityProperties.h"
#include "core/Animation.h"
#include "io/AssetLibrary.h"
#include "render/OffscreenRenderer.h"

#include "EffectsUi.h"
#include "Plugins.h"
#include "GameMode.h"
#include "Icons.h"
#include "ModifyTools.h"
#include "Panels.h"
#include "TerrainTools.h"
#include "Viewport.h"
#include "core/Components.h"
#include "core/Primitives.h"
#include "core/RoundCorners.h"
#include "io/GltfExport.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "io/ObjIO.h"
#include "io/StlExport.h"
#include "io/PrintExport.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTimer>
#include <QToolButton>

namespace gs {

namespace {

// Solid Tools toolbar/menu order.
const ToolId kSolidTools[] = {ToolId::SolidIntersect, ToolId::SolidUnion, ToolId::SolidSubtract, ToolId::SolidTrim,
                              ToolId::SolidSplit};

QJsonArray vec(const Vec3& v) { return QJsonArray{v.x, v.y, v.z}; }
Vec3 vec(const QJsonValue& j, const Vec3& def) {
    QJsonArray a = j.toArray();
    return a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : def;
}

const char* kFileFilter = "GNUSketch Model (*.gsketch)";

} // namespace

MainWindow::MainWindow() {
    setWindowIcon(Icons::get("pushpull"));
    setAcceptDrops(true);
    viewport_ = new Viewport(&doc_, this);
    setCentralWidget(viewport_);

    doc_.onChanged = [this] { onDocChanged(); };
    doc_.onSelectionChanged = [this] { onSelectionChanged(); };

    buildActions();
    buildMenus();
    buildToolbars();
    buildTray();
    buildStatusBar();

    connect(viewport_, &Viewport::statusText, this, [this](const QString& t) { statusLabel_->setText(t); });
    connect(viewport_, &Viewport::vcbChanged, this, [this](const QString& label, const QString& value) {
        if (!label.isEmpty()) vcbLabel_->setText(label);
        if (!vcbEdit_->hasFocus()) vcbEdit_->setText(value);
    });
    connect(viewport_, &Viewport::toolChanged, this, [this](ToolId id) {
        auto it = toolActions_.find(id);
        if (it != toolActions_.end()) it->second->setChecked(true);
        instructor_->showTool(id);
        if (isTerrainTool(id)) {
            terrain_->refresh();
            sections_["Terraform"]->setExpanded(true);
        }
    });
    connect(viewport_, &Viewport::materialPicked, this, [this] { materials_->refresh(); entityInfo_->refresh(); });
    connect(viewport_, &Viewport::imageDropped, this, [this](const QString& p) { materials_->addImageMaterial(p); });
    connect(viewport_, &Viewport::contextMenuAt, this, [this](const QPoint& p, PickResult pr) { contextMenu(p, pr); });
    connect(viewport_, &Viewport::glFailed, this, [this](const QString& log) {
        QMessageBox::critical(this, QStringLiteral("OpenGL error"),
                              QStringLiteral("GNUSketch needs OpenGL 3.3 (core profile).\n\n%1").arg(log));
    });
    connect(materials_, &MaterialsPanel::materialChosen, this, [this] { entityInfo_->refresh(); });
    connect(styles_, &StylesPanel::styleChanged, this, [this] { syncStyleActions(); });
    connect(sun_, &SunPanel::styleChanged, this, [this] { syncStyleActions(); });
    connect(materials_, &MaterialsPanel::browserRequested, this, [this] {
        materialBrowser()->show();
        materialBrowser()->raise();
    });
    connect(materials_, &MaterialsPanel::editorRequested, this, [this] {
        materialEditor()->show();
        materialEditor()->raise();
        materialEditor()->refresh();
    });
    connect(viewport_, &Viewport::characterPlaced, this, [this](const Vec3& feet, double yaw) { launchGame(feet, yaw); });

    QSettings s;
    restoreGeometry(s.value("window/geometry").toByteArray());
    if (!restoreState(s.value("window/state").toByteArray())) resize(1400, 900);
    int unit = s.value("model/units", (int)LengthUnit::Millimeters).toInt();
    doc_.units.unit = (LengthUnit)std::clamp(unit, 0, 4);
    if (s.value("camera/orbitPivot", 0).toInt() == (int)Viewport::OrbitPivot::CameraTarget)
        acts_["orbit_target"]->trigger();

    viewport_->setTool(ToolId::Select);
    instructor_->showTool(ToolId::Select);
    updateTitle();
    updateUndoActions();
    syncStyleActions();
    onSelectionChanged();
}

MainWindow::~MainWindow() {
    // The game window works on this window's temporary copy of the model.
    if (game_) {
        game_->disconnect(this);
        game_->kill();
        game_->waitForFinished(2000);
    }
    doc_.onChanged = nullptr;
    doc_.onSelectionChanged = nullptr;
}

// ---------------------------------------------------------------------------
// Actions / menus / toolbars
// ---------------------------------------------------------------------------

QAction* MainWindow::toolAction(ToolId id, const QString& text, const QString& shortcut) {
    auto* a = new QAction(Icons::get(Icons::toolIconName(id)), text, this);
    a->setCheckable(true);
    if (!shortcut.isEmpty()) a->setShortcut(QKeySequence(shortcut));
    a->setToolTip(shortcut.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, shortcut));
    a->setStatusTip(text);
    toolGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, id] { viewport_->setTool(id); });
    toolActions_[id] = a;
    return a;
}

void MainWindow::buildActions() {
    toolGroup_ = new QActionGroup(this);
    toolGroup_->setExclusive(true);
    toolAction(ToolId::Select, QStringLiteral("Select"), QStringLiteral("Space"));
    toolAction(ToolId::Eraser, QStringLiteral("Eraser"), QStringLiteral("E"));
    toolAction(ToolId::Paint, QStringLiteral("Paint Bucket"), QStringLiteral("B"));
    toolAction(ToolId::Line, QStringLiteral("Line"), QStringLiteral("L"));
    toolAction(ToolId::Freehand, QStringLiteral("Freehand"), QString());
    toolAction(ToolId::Rectangle, QStringLiteral("Rectangle"), QStringLiteral("R"));
    toolAction(ToolId::Circle, QStringLiteral("Circle"), QStringLiteral("C"));
    toolAction(ToolId::Polygon, QStringLiteral("Polygon"), QStringLiteral("G"));
    toolAction(ToolId::Arc, QStringLiteral("2 Point Arc"), QStringLiteral("A"));
    toolAction(ToolId::PushPull, QStringLiteral("Push/Pull"), QStringLiteral("P"));
    toolAction(ToolId::Move, QStringLiteral("Move"), QStringLiteral("M"));
    toolAction(ToolId::Rotate, QStringLiteral("Rotate"), QStringLiteral("Q"));
    toolAction(ToolId::Scale, QStringLiteral("Scale"), QStringLiteral("S"));
    toolAction(ToolId::Offset, QStringLiteral("Offset"), QStringLiteral("F"));
    toolAction(ToolId::FollowMe, QStringLiteral("Follow Me"), QString());
    toolAction(ToolId::Tape, QStringLiteral("Tape Measure"), QStringLiteral("T"));
    toolAction(ToolId::Orbit, QStringLiteral("Orbit"), QStringLiteral("O"));
    toolAction(ToolId::Pan, QStringLiteral("Pan"), QStringLiteral("H"));
    toolAction(ToolId::Zoom, QStringLiteral("Zoom"), QStringLiteral("Z"));
    toolAction(ToolId::ZoomWindow, QStringLiteral("Zoom Window"), QStringLiteral("Ctrl+Shift+W"));
    toolAction(ToolId::Mountain, QStringLiteral("Mountain"), QString());
    toolAction(ToolId::Pit, QStringLiteral("Pit"), QString());
    toolAction(ToolId::Flatten, QStringLiteral("Flatten"), QString());
    toolAction(ToolId::TextureBrush, QStringLiteral("Texture Brush"), QString());
    toolAction(ToolId::SolidUnion, QStringLiteral("Union"), QString());
    toolAction(ToolId::SolidIntersect, QStringLiteral("Intersect"), QString());
    toolAction(ToolId::SolidSubtract, QStringLiteral("Subtract"), QString());
    toolAction(ToolId::SolidTrim, QStringLiteral("Trim"), QString());
    toolAction(ToolId::SolidSplit, QStringLiteral("Split"), QString());
    toolAction(ToolId::Bend, QStringLiteral("Bend"), QStringLiteral("Ctrl+B"));
    toolAction(ToolId::TexturePosition, QStringLiteral("Position Texture"), QStringLiteral("Ctrl+U"));
    toolActions_[ToolId::TexturePosition]->setObjectName(QStringLiteral("textureposition"));
    toolAction(ToolId::PlacePointLight, QStringLiteral("Point Light"), QString());
    toolAction(ToolId::PlaceSpotLight, QStringLiteral("Spot Light"), QString());
    toolAction(ToolId::PlaceDirectionalLight, QStringLiteral("Directional Light"), QString());
    toolAction(ToolId::PlaceCamera, QStringLiteral("Place Camera"), QString());
    toolAction(ToolId::PlaceCharacter, QStringLiteral("Game Mode"), QStringLiteral("Ctrl+G"));
    toolActions_[ToolId::PlaceCharacter]->setObjectName(QStringLiteral("gamemode"));
    toolActions_[ToolId::PlaceCharacter]->setStatusTip(QStringLiteral("Place a character on the model and walk around it in a separate window."));

    auto act = [this](const QString& key, const QString& icon, const QString& text, const QKeySequence& sc,
                      std::function<void()> fn) {
        auto* a = new QAction(icon.isEmpty() ? QIcon() : Icons::get(icon), text, this);
        if (!sc.isEmpty()) a->setShortcut(sc);
        a->setToolTip(sc.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, sc.toString(QKeySequence::NativeText)));
        connect(a, &QAction::triggered, this, std::move(fn));
        a->setObjectName(key);
        acts_[key] = a;
        return a;
    };
    act("new", "new", QStringLiteral("&New"), QKeySequence::New, [this] { newFile(); });
    act("open", "open", QStringLiteral("&Open…"), QKeySequence::Open, [this] { open(); });
    act("save", "save", QStringLiteral("&Save"), QKeySequence::Save, [this] { save(); });
    act("saveas", "", QStringLiteral("Save &As…"), QKeySequence::SaveAs, [this] { saveAs(); });
    act("importobj", "import", QStringLiteral("Wavefront OBJ (*.obj)…"), QKeySequence(), [this] { importObj(); });
    act("exportobj", "export", QStringLiteral("Wavefront OBJ, n-gons (*.obj)…"), QKeySequence(), [this] { exportObj(); });
    act("roundcorners", "roundcorners", QStringLiteral("&Round Corners…"), QKeySequence(), [this] { roundCorners(); })
        ->setStatusTip(QStringLiteral("Round the selected edges or corners with a radius and number of segments."));
    act("primitive", "", QStringLiteral("&Primitive…"), QKeySequence(), [this] { insertPrimitive(); })
        ->setStatusTip(QStringLiteral("Make a sphere, cylinder, torus, dodecahedron or other solid and place it with a click."));
    act("traceimage", "traceimage", QStringLiteral("&Trace Image…"), QKeySequence(), [this] { traceImage(); })
        ->setStatusTip(QStringLiteral("Turn the alpha cutout or the dark/light shapes of a face's image into outline geometry."));
    act("exportgltf", "", QStringLiteral("glTF 2.0 (*.gltf)…"), QKeySequence(), [this] { exportGltf(); });
    act("export3mf", "", QStringLiteral("3MF (*.3mf)…"), QKeySequence(), [this] { exportForPrint((int)PrintFormat::ThreeMF); })
        ->setStatusTip(QStringLiteral("3D Manufacturing Format: keeps the unit and the colours; preferred by most slicers."));
    act("exportamf", "", QStringLiteral("AMF (*.amf)…"), QKeySequence(), [this] { exportForPrint((int)PrintFormat::Amf); })
        ->setStatusTip(QStringLiteral("Additive Manufacturing File: keeps the unit and the colours."));
    act("exportstl", "", QStringLiteral("STL, binary (*.stl)…"), QKeySequence(), [this] { exportStl(); })
        ->setStatusTip(QStringLiteral("STL has no units: it is written in the unit you choose (the model's by default)."));
    act("exportstlascii", "", QStringLiteral("STL, ASCII (*.stl)…"), QKeySequence(), [this] { exportForPrint((int)PrintFormat::StlAscii); });
    act("exportply", "", QStringLiteral("PLY (*.ply)…"), QKeySequence(), [this] { exportForPrint((int)PrintFormat::Ply); });
    act("exportoff", "", QStringLiteral("OFF (*.off)…"), QKeySequence(), [this] { exportForPrint((int)PrintFormat::Off); });
    act("settings", "", QStringLiteral("&Settings…"), QKeySequence::Preferences, [this] { showSettings(); });
    act("quit", "", QStringLiteral("E&xit"), QKeySequence::Quit, [this] { close(); });

    undoAct_ = act("undo", "undo", QStringLiteral("&Undo"), QKeySequence::Undo, [this] {
        // Undo during a live move/paste just cancels it.
        bool live = doc_.inOp();
        viewport_->cancelTool();
        if (!live) doc_.undo();
    });
    redoAct_ = act("redo", "redo", QStringLiteral("&Redo"), QKeySequence(QStringLiteral("Ctrl+Y")), [this] {
        viewport_->cancelTool();
        doc_.redo();
    });
    acts_["redo"]->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    act("cut", "cut", QStringLiteral("Cu&t"), QKeySequence::Cut, [this] { copySelection(true); });
    act("copy", "copy", QStringLiteral("&Copy"), QKeySequence::Copy, [this] { copySelection(false); });
    act("paste", "paste", QStringLiteral("&Paste"), QKeySequence::Paste, [this] { paste(); });
    act("delete", "delete", QStringLiteral("&Delete"), QKeySequence::Delete, [this] { deleteSelection(); });
    act("deleteguides", "", QStringLiteral("Delete &Guides"), QKeySequence(), [this] { deleteGuides(); });
    act("selectall", "", QStringLiteral("Select &All"), QKeySequence::SelectAll, [this] { selectAll(); });
    act("selectnone", "", QStringLiteral("Select &None"), QKeySequence(QStringLiteral("Ctrl+T")), [this] {
        doc_.selection.clear();
        doc_.notifySelection();
        viewport_->update();
    });
    act("hide", "", QStringLiteral("&Hide"), QKeySequence(), [this] { hideSelection(); });
    act("unhide", "", QStringLiteral("&Unhide All"), QKeySequence(), [this] { unhideAll(); });
    act("reverse", "", QStringLiteral("&Reverse Faces"), QKeySequence(), [this] { reverseFaces(); });
    act("soften", "", QStringLiteral("&Soften/Smooth Edges"), QKeySequence(), [this] { softenSelection(true); });
    act("unsoften", "", QStringLiteral("U&nsoften Edges"), QKeySequence(), [this] { softenSelection(false); });
    act("texfit", "", QStringLiteral("Texture: &Fit to Face"), QKeySequence(), [this] { textureFitSelection(true); });
    act("texreset", "", QStringLiteral("Texture: &Reset Position"), QKeySequence(), [this] { textureFitSelection(false); });
    act("makecomponent", "", QStringLiteral("Make &Component…"), QKeySequence(), [this] { makeComponent(); })
        ->setStatusTip(QStringLiteral("Turn the selected geometry into a component; copies of it share their geometry."));
    act("recordstate", "", QStringLiteral("&Record Animation State"), QKeySequence(Qt::Key_F5), [this] { promptRecordState(); })
        ->setStatusTip(QStringLiteral("Save the open component's geometry as a named state; move it and press F5 again for the next state."));
    act("entityprops", "", QStringLiteral("Propert&ies…"), QKeySequence(), [this] { showEntityProperties(); })
        ->setStatusTip(QStringLiteral("Make the selected component a door, button or actor, link it and script it."));
    act("saveprefab", "", QStringLiteral("Save as &Prefab…"), QKeySequence(), [this] { saveAsPrefab(); })
        ->setStatusTip(QStringLiteral("Save the selected component as a file in the prefab library, to drop into any model."));
    act("addcamera", "camera_place", QStringLiteral("Add Camera From &View"), QKeySequence(), [this] { addCameraFromView(); })
        ->setStatusTip(QStringLiteral("Put a camera in the world that sees what the view sees now."));
    act("materialbrowser", "paint", QStringLiteral("&Material Browser…"), QKeySequence(), [this] {
        materialBrowser()->show();
        materialBrowser()->raise();
    });
    act("materialeditor", "", QStringLiteral("Material &Editor…"), QKeySequence(), [this] {
        materialEditor()->show();
        materialEditor()->raise();
        materialEditor()->refresh();
    });
    act("prefabbrowser", "", QStringLiteral("&Prefab Browser…"), QKeySequence(), [this] {
        prefabBrowser()->show();
        prefabBrowser()->raise();
    });
    act("editcomponent", "", QStringLiteral("&Edit Component"), QKeySequence(), [this] { editComponent(); })
        ->setStatusTip(QStringLiteral("Edit the selected component in place (or double-click it); every copy changes too."));
    act("closecomponent", "", QStringLiteral("C&lose Component"), QKeySequence(), [this] {
        viewport_->cancelTool();
        doc_.closeComponent();
        viewport_->refreshToolStatus();
    })->setStatusTip(QStringLiteral("Stop editing the component (Esc, or click outside it)."));
    act("makeunique", "", QStringLiteral("Make &Unique"), QKeySequence(), [this] { makeUnique(); })
        ->setStatusTip(QStringLiteral("Give the selected copies their own geometry, separate from the other copies."));
    act("explode", "", QStringLiteral("E&xplode"), QKeySequence(), [this] { explode(); })
        ->setStatusTip(QStringLiteral("Turn the selected components back into loose geometry."));
    act("modelinfo", "info", QStringLiteral("&Model Info…"), QKeySequence(), [this] { modelInfo(); });

    act("zoomext", "zoomextents", QStringLiteral("Zoom E&xtents"), QKeySequence(QStringLiteral("Shift+Z")), [this] { viewport_->zoomExtents(); });
    act("view_iso", "view_iso", QStringLiteral("&Iso"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(1, 1, -0.8)); });
    act("view_top", "view_top", QStringLiteral("&Top"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(0, 0, -1)); });
    act("view_front", "view_front", QStringLiteral("&Front"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(0, 1, 0)); });
    act("view_right", "view_right", QStringLiteral("&Right"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(-1, 0, 0)); });
    act("view_back", "view_back", QStringLiteral("&Back"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(0, -1, 0)); });
    act("view_left", "view_left", QStringLiteral("&Left"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(1, 0, 0)); });
    act("view_bottom", "", QStringLiteral("B&ottom"), QKeySequence(), [this] { viewport_->setStandardView(Vec3(0, 0, 1)); });

    auto* projGroup = new QActionGroup(this);
    perspectiveAct_ = act("perspective", "perspective", QStringLiteral("&Perspective"), QKeySequence(), [this] {
        viewport_->setPerspective(true);
    });
    parallelAct_ = act("parallel", "parallel", QStringLiteral("Parallel P&rojection"), QKeySequence(), [this] {
        viewport_->setPerspective(false);
    });
    for (QAction* a : {perspectiveAct_, parallelAct_}) {
        a->setCheckable(true);
        projGroup->addAction(a);
    }
    perspectiveAct_->setChecked(true);

    auto* pivotGroup = new QActionGroup(this);
    auto pivotAct = [this, &act, pivotGroup](const QString& key, const QString& text, Viewport::OrbitPivot p) {
        QAction* a = act(key, "", text, QKeySequence(), [this, p] {
            viewport_->setOrbitPivot(p);
            QSettings().setValue("camera/orbitPivot", (int)p);
        });
        a->setCheckable(true);
        a->setChecked(p == Viewport::OrbitPivot::Geometry);
        pivotGroup->addAction(a);
    };
    pivotAct("orbit_geometry", QStringLiteral("Around &Geometry"), Viewport::OrbitPivot::Geometry);
    pivotAct("orbit_target", QStringLiteral("Around Camera &Target"), Viewport::OrbitPivot::CameraTarget);

    auto styleToggle = [this, &act](const QString& key, const QString& icon, const QString& text, bool RenderStyle::*field) {
        QAction* a = act(key, icon, text, QKeySequence(), [] {});
        a->setCheckable(true);
        connect(a, &QAction::toggled, this, [this, field](bool on) {
            doc_.style.*field = on;
            styles_->refresh();
            sun_->refresh();
            viewport_->update();
        });
        return a;
    };
    xrayAct_ = styleToggle("xray", "style_xray", QStringLiteral("&X-ray"), &RenderStyle::xray);
    backEdgesAct_ = styleToggle("backedges", "style_backedges", QStringLiteral("&Back Edges"), &RenderStyle::backEdges);
    edgesAct_ = styleToggle("edges", "", QStringLiteral("&Edges"), &RenderStyle::edges);
    profilesAct_ = styleToggle("profiles", "", QStringLiteral("&Profiles"), &RenderStyle::profiles);
    axesAct_ = styleToggle("axes", "", QStringLiteral("&Axes"), &RenderStyle::showAxes);
    guidesAct_ = styleToggle("guides", "", QStringLiteral("&Guides"), &RenderStyle::showGuides);
    objectsAct_ = styleToggle("objects", "", QStringLiteral("&Lights and Cameras"), &RenderStyle::showObjects);
    hiddenAct_ = styleToggle("hiddengeom", "", QStringLiteral("&Hidden Geometry"), &RenderStyle::showHidden);
    shadowsAct_ = styleToggle("shadows", "", QStringLiteral("&Shadows"), &RenderStyle::shadows);
    aoAct_ = styleToggle("ambientocclusion", "", QStringLiteral("Ambient &Occlusion"), &RenderStyle::ambientOcclusion);
    antialiasAct_ = styleToggle("antialiasing", "", QStringLiteral("&Antialiasing"), &RenderStyle::antialiasing);
    sunAct_ = styleToggle("sun", "", QStringLiteral("S&un"), &RenderStyle::sun);
    sunAct_->setStatusTip(QStringLiteral("Sunlight and the sun in the sky. Window > Sun sets its direction and height."));
    cullAct_ = styleToggle("backfaceculling", "", QStringLiteral("&Backface Culling"), &RenderStyle::backfaceCulling);
    cullAct_->setStatusTip(QStringLiteral("Do not draw or pick the back of a face. A face looked at from behind becomes see-through."));

    act("repeateffect", "effects", QStringLiteral("&Repeat Effect"), QKeySequence(QStringLiteral("Ctrl+F")), [this] {
        if (lastEffect_) runEffect(*lastEffect_, true);
    });
    for (const EffectDef& d : effectRegistry()) {
        const EffectDef* dp = &d;
        QAction* a = act(d.key(), d.icon, d.menuText + (d.params.empty() ? QString() : QStringLiteral("…")), QKeySequence(),
                         [this, dp] { runEffect(*dp, false); });
        a->setStatusTip(d.tip);
    }

    auto* faceGroup = new QActionGroup(this);
    struct FS { RenderStyle::FaceStyle s; const char* icon; const char* text; };
    const FS fss[] = {{RenderStyle::Wireframe, "style_wireframe", "&Wireframe"},
                      {RenderStyle::HiddenLine, "style_hiddenline", "&Hidden Line"},
                      {RenderStyle::Shaded, "style_shaded", "&Shaded"},
                      {RenderStyle::ShadedTextures, "style_textured", "Shaded with &Textures"},
                      {RenderStyle::Monochrome, "style_mono", "&Monochrome"}};
    for (const FS& f : fss) {
        auto* a = new QAction(Icons::get(f.icon), QString::fromUtf8(f.text), this);
        a->setCheckable(true);
        a->setToolTip(QString::fromUtf8(f.text).remove('&'));
        faceGroup->addAction(a);
        RenderStyle::FaceStyle s = f.s;
        connect(a, &QAction::triggered, this, [this, s] {
            doc_.style.faceStyle = s;
            styles_->refresh();
            viewport_->update();
        });
        faceStyleActs_[(int)f.s] = a;
    }

    act("instructor", "help", QStringLiteral("&Instructor"), QKeySequence(QStringLiteral("F1")), [this] {
        auto it = sections_.find("Instructor");
        if (it != sections_.end()) it->second->setExpanded(!it->second->expanded());
        trayDock_->show();
    });
    act("shortcuts", "", QStringLiteral("&Keyboard Shortcuts"), QKeySequence(), [this] { showShortcuts(); });
    act("about", "", QStringLiteral("&About GNUSketch"), QKeySequence(), [this] { about(); });
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(acts_["new"]);
    file->addAction(acts_["open"]);
    recentMenu_ = file->addMenu(QStringLiteral("Open &Recent"));
    recentMenu_->setObjectName(QStringLiteral("RecentMenu"));
    connect(recentMenu_, &QMenu::aboutToShow, this, [this] { rebuildRecentMenu(); });
    rebuildRecentMenu();
    file->addAction(acts_["save"]);
    file->addAction(acts_["saveas"]);
    file->addSeparator();
    QMenu* imp = file->addMenu(Icons::get("import"), QStringLiteral("&Import"));
    imp->setObjectName(QStringLiteral("ImportMenu"));
    imp->addAction(acts_["importobj"]);
    for (const LoadedPlugin& p : Plugins::all())   // (formats from plugins)
        if (FormatPlugin* f = p.format; f->canImport())
            imp->addAction(QStringLiteral("%1 (*.%2)…").arg(f->formatName(), f->extensions().join(", *.")), this, [this, f] { importPlugin(f); });
    QMenu* exp = file->addMenu(Icons::get("export"), QStringLiteral("&Export"));
    exp->setObjectName(QStringLiteral("ExportMenu"));
    exp->addSection(QStringLiteral("3D Model"));
    exp->addAction(acts_["exportobj"]);
    exp->addAction(acts_["exportgltf"]);
    for (const LoadedPlugin& p : Plugins::all())
        if (FormatPlugin* f = p.format; f->canExport())
            exp->addAction(QStringLiteral("%1 (*.%2)…").arg(f->formatName(), f->extensions().join(", *.")), this, [this, f] { exportPlugin(f); });
    exp->addSection(QStringLiteral("3D Printing"));
    exp->addAction(acts_["export3mf"]);
    exp->addAction(acts_["exportamf"]);
    exp->addAction(acts_["exportstl"]);
    exp->addAction(acts_["exportstlascii"]);
    exp->addAction(acts_["exportply"]);
    exp->addAction(acts_["exportoff"]);
    file->addSeparator();
    file->addAction(acts_["quit"]);

    QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(undoAct_);
    edit->addAction(redoAct_);
    edit->addSeparator();
    edit->addAction(acts_["cut"]);
    edit->addAction(acts_["copy"]);
    edit->addAction(acts_["paste"]);
    edit->addAction(acts_["delete"]);
    edit->addAction(acts_["deleteguides"]);
    edit->addSeparator();
    edit->addAction(acts_["selectall"]);
    edit->addAction(acts_["selectnone"]);
    edit->addSeparator();
    edit->addAction(acts_["hide"]);
    edit->addAction(acts_["unhide"]);
    edit->addSeparator();
    edit->addAction(acts_["makecomponent"]);
    edit->addAction(acts_["editcomponent"]);
    edit->addAction(acts_["closecomponent"]);
    edit->addAction(acts_["makeunique"]);
    edit->addAction(acts_["explode"]);
    edit->addSeparator();
    edit->addAction(acts_["reverse"]);
    edit->addAction(acts_["soften"]);
    edit->addAction(acts_["unsoften"]);
    edit->addAction(acts_["texfit"]);
    edit->addAction(acts_["texreset"]);

    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(axesAct_);
    view->addAction(guidesAct_);
    view->addAction(objectsAct_);
    view->addAction(hiddenAct_);
    view->addSeparator();
    QMenu* edgeStyle = view->addMenu(QStringLiteral("Edge St&yle"));
    edgeStyle->addAction(edgesAct_);
    edgeStyle->addAction(backEdgesAct_);
    edgeStyle->addAction(profilesAct_);
    QMenu* faceStyle = view->addMenu(QStringLiteral("&Face Style"));
    faceStyle->addAction(xrayAct_);
    faceStyle->addSeparator();
    for (auto& [k, a] : faceStyleActs_) faceStyle->addAction(a);
    QMenu* render = view->addMenu(QStringLiteral("&Render"));
    render->addAction(sunAct_);
    render->addAction(shadowsAct_);
    render->addAction(aoAct_);
    render->addAction(antialiasAct_);
    render->addAction(cullAct_);
    view->addSeparator();
    view->addAction(acts_["instructor"]);

    QMenu* cam = menuBar()->addMenu(QStringLiteral("&Camera"));
    QMenu* sv = cam->addMenu(QStringLiteral("&Standard Views"));
    for (const char* k : {"view_top", "view_bottom", "view_front", "view_back", "view_left", "view_right", "view_iso"}) sv->addAction(acts_[k]);
    cam->addSeparator();
    cam->addAction(parallelAct_);
    cam->addAction(perspectiveAct_);
    cam->addSeparator();
    cam->addAction(toolActions_[ToolId::Orbit]);
    QMenu* orbitMode = cam->addMenu(QStringLiteral("Orbit &Mode"));
    orbitMode->addAction(acts_["orbit_geometry"]);
    orbitMode->addAction(acts_["orbit_target"]);
    cam->addAction(toolActions_[ToolId::Pan]);
    cam->addAction(toolActions_[ToolId::Zoom]);
    cam->addAction(toolActions_[ToolId::ZoomWindow]);
    cam->addAction(acts_["zoomext"]);
    cam->addSeparator();
    cam->addAction(acts_["addcamera"]);
    cam->addAction(toolActions_[ToolId::PlaceCamera]);
    QMenu* placed = cam->addMenu(QStringLiteral("Look &Through"));
    connect(placed, &QMenu::aboutToShow, this, [this, placed] {
        placed->clear();
        const auto& objs = doc_.state.objects;
        for (Id i = 0; i < (Id)objs.size(); ++i)
            if (objs[i].alive && objs[i].kind == SceneObject::ViewCamera)
                placed->addAction(QString::fromStdString(objs[i].name), this, [this, i] { viewport_->lookThrough(i); });
        if (placed->isEmpty()) placed->addAction(QStringLiteral("No cameras in the model"))->setEnabled(false);
    });

    QMenu* draw = menuBar()->addMenu(QStringLiteral("&Draw"));
    QMenu* lines = draw->addMenu(QStringLiteral("&Lines"));
    lines->addAction(toolActions_[ToolId::Line]);
    lines->addAction(toolActions_[ToolId::Freehand]);
    QMenu* arcs = draw->addMenu(QStringLiteral("&Arcs"));
    arcs->addAction(toolActions_[ToolId::Arc]);
    QMenu* lightsMenu = draw->addMenu(Icons::get("light_point"), QStringLiteral("L&ights"));
    for (ToolId id : {ToolId::PlacePointLight, ToolId::PlaceSpotLight, ToolId::PlaceDirectionalLight}) lightsMenu->addAction(toolActions_[id]);
    QMenu* shapes = draw->addMenu(QStringLiteral("&Shapes"));
    shapes->addAction(toolActions_[ToolId::Rectangle]);
    shapes->addAction(toolActions_[ToolId::Circle]);
    shapes->addAction(toolActions_[ToolId::Polygon]);
    draw->addSeparator();
    draw->addAction(acts_["primitive"]);

    QMenu* tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    for (ToolId id : {ToolId::Select, ToolId::Eraser, ToolId::Paint}) tools->addAction(toolActions_[id]);
    tools->addSeparator();
    for (ToolId id : {ToolId::Move, ToolId::Rotate, ToolId::Scale, ToolId::PushPull, ToolId::FollowMe, ToolId::Offset})
        tools->addAction(toolActions_[id]);
    tools->addSeparator();
    tools->addAction(toolActions_[ToolId::Tape]);
    tools->addSeparator();
    QMenu* terraform = tools->addMenu(Icons::get("mountain"), QStringLiteral("Terra&form"));
    for (ToolId id : {ToolId::Mountain, ToolId::Pit, ToolId::Flatten, ToolId::TextureBrush}) terraform->addAction(toolActions_[id]);
    QMenu* solids = tools->addMenu(Icons::get("solid_union"), QStringLiteral("&Solid Tools"));
    for (ToolId id : kSolidTools) solids->addAction(toolActions_[id]);
    tools->addSeparator();
    tools->addAction(toolActions_[ToolId::Bend]);
    tools->addAction(toolActions_[ToolId::TexturePosition]);
    tools->addAction(acts_["roundcorners"]);
    tools->addAction(acts_["traceimage"]);
    tools->addSeparator();
    tools->addAction(toolActions_[ToolId::PlaceCharacter]);
    tools->addAction(acts_["entityprops"]);
    tools->addAction(acts_["recordstate"]);

    QMenu* effects = menuBar()->addMenu(QStringLiteral("Effe&cts"));
    effects->addAction(acts_["repeateffect"]);
    effects->addSeparator();
    std::map<QString, QMenu*> effectMenus;
    for (const EffectDef& d : effectRegistry()) {
        QMenu*& sub = effectMenus[d.submenu];
        if (!sub) sub = effects->addMenu(d.submenu);
        sub->addAction(acts_[d.key()]);
    }

    QMenu* win = menuBar()->addMenu(QStringLiteral("&Window"));
    win->addAction(acts_["settings"]);
    win->addSeparator();
    win->addAction(acts_["modelinfo"]);
    win->addAction(acts_["materialbrowser"]);
    win->addAction(acts_["materialeditor"]);
    win->addAction(acts_["prefabbrowser"]);
    QAction* hierAct = win->addAction(QStringLiteral("&Hierarchy"));
    hierAct->setCheckable(true);
    connect(hierAct, &QAction::toggled, this, [this](bool on) { hierarchyDock_->setVisible(on); });
    connect(win, &QMenu::aboutToShow, this, [this, hierAct] { QSignalBlocker b(hierAct); hierAct->setChecked(hierarchyDock_->isVisible()); });
    win->addSeparator();
    QAction* trayAct = win->addAction(QStringLiteral("Default &Tray"));
    trayAct->setCheckable(true);
    trayAct->setChecked(true);
    connect(trayAct, &QAction::toggled, this, [this](bool on) { trayDock_->setVisible(on); });
    connect(win, &QMenu::aboutToShow, this, [this, trayAct] { trayAct->setChecked(trayDock_->isVisible()); });
    for (const char* name : {"Entity Info", "Terraform", "Materials", "Scene", "Styles", "Sun", "Instructor"}) {
        QAction* a = win->addAction(QString::fromUtf8(name));
        QString key = QString::fromUtf8(name);
        connect(a, &QAction::triggered, this, [this, key] {
            trayDock_->show();
            auto it = sections_.find(key);
            if (it != sections_.end()) it->second->setExpanded(true);
        });
    }

    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(acts_["instructor"]);
    help->addAction(acts_["shortcuts"]);
    help->addAction(QStringLiteral("&Plugins…"), this, [this] { showPlugins(); });
    help->addSeparator();
    help->addAction(acts_["about"]);
}

void MainWindow::buildToolbars() {
    auto* std = addToolBar(QStringLiteral("Standard"));
    std->setObjectName("StandardToolbar");
    std->setIconSize(QSize(24, 24));
    std->setMovable(true);
    for (const char* k : {"new", "open", "save"}) std->addAction(acts_[k]);
    std->addSeparator();
    for (const char* k : {"cut", "copy", "paste", "delete"}) std->addAction(acts_[k]);
    std->addSeparator();
    std->addAction(undoAct_);
    std->addAction(redoAct_);
    std->addSeparator();
    std->addAction(acts_["modelinfo"]);

    auto* views = addToolBar(QStringLiteral("Views"));
    views->setObjectName("ViewsToolbar");
    views->setIconSize(QSize(24, 24));
    for (const char* k : {"view_iso", "view_top", "view_front", "view_right", "view_back", "view_left"}) views->addAction(acts_[k]);

    auto* styles = addToolBar(QStringLiteral("Styles"));
    styles->setObjectName("StylesToolbar");
    styles->setIconSize(QSize(24, 24));
    styles->addAction(xrayAct_);
    styles->addAction(backEdgesAct_);
    for (auto& [k, a] : faceStyleActs_) styles->addAction(a);

    auto* camera = addToolBar(QStringLiteral("Camera"));
    camera->setObjectName("CameraToolbar");
    camera->setIconSize(QSize(24, 24));
    for (ToolId id : {ToolId::Orbit, ToolId::Pan, ToolId::Zoom, ToolId::ZoomWindow}) camera->addAction(toolActions_[id]);
    camera->addAction(acts_["zoomext"]);
    camera->addSeparator();
    camera->addAction(perspectiveAct_);
    camera->addAction(parallelAct_);

    auto* solids = addToolBar(QStringLiteral("Solid Tools"));
    solids->setObjectName("SolidToolsToolbar");
    solids->setIconSize(QSize(24, 24));
    for (ToolId id : kSolidTools) solids->addAction(toolActions_[id]);

    // Large tool set: two columns on the left like the reference layout.
    auto* large = new QToolBar(QStringLiteral("Large Tool Set"), this);
    large->setObjectName("LargeToolSet");
    large->setMovable(true);
    large->setOrientation(Qt::Vertical);
    auto* grid = new QWidget(large);
    auto* gl = new QGridLayout(grid);
    gl->setContentsMargins(2, 2, 2, 2);
    gl->setHorizontalSpacing(1);
    gl->setVerticalSpacing(1);
    // Cell values: tool id, -1 = empty, -2 = separator row, -3 = zoom extents,
    // -4 = round corners, -5 = trace image.
    const int rows[][2] = {
        {(int)ToolId::Select, (int)ToolId::Paint},
        {(int)ToolId::Eraser, (int)ToolId::Tape},
        {-2, -2},
        {(int)ToolId::Line, (int)ToolId::Freehand},
        {(int)ToolId::Rectangle, (int)ToolId::Circle},
        {(int)ToolId::Polygon, (int)ToolId::Arc},
        {-2, -2},
        {(int)ToolId::Move, (int)ToolId::PushPull},
        {(int)ToolId::Rotate, (int)ToolId::Offset},
        {(int)ToolId::Scale, (int)ToolId::FollowMe},
        {-4, -5},
        {(int)ToolId::Bend, (int)ToolId::TexturePosition},
        {-2, -2},
        {(int)ToolId::Mountain, (int)ToolId::Pit},
        {(int)ToolId::Flatten, (int)ToolId::TextureBrush},
        {-2, -2},
        {(int)ToolId::Orbit, (int)ToolId::Pan},
        {(int)ToolId::Zoom, (int)ToolId::ZoomWindow},
        {-3, (int)ToolId::PlaceCharacter},
    };
    int r = 0;
    for (const auto& row : rows) {
        if (row[0] == -2) {
            auto* sep = new QFrame(grid);
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Sunken);
            gl->addWidget(sep, r++, 0, 1, 2);
            continue;
        }
        for (int c = 0; c < 2; ++c) {
            if (row[c] == -1) continue;
            auto* b = new QToolButton(grid);
            b->setIconSize(QSize(28, 28));
            b->setAutoRaise(true);
            b->setDefaultAction(row[c] == -3 ? acts_["zoomext"]
                                : row[c] == -4 ? acts_["roundcorners"]
                                : row[c] == -5 ? acts_["traceimage"]
                                               : toolActions_[(ToolId)row[c]]);
            gl->addWidget(b, r, c);
        }
        ++r;
    }
    gl->setRowStretch(r, 1);
    large->addWidget(grid);
    addToolBar(Qt::LeftToolBarArea, large);
}

void MainWindow::buildTray() {
    trayDock_ = new QDockWidget(QStringLiteral("Default Tray"), this);
    trayDock_->setObjectName("DefaultTray");
    trayDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* scroll = new QScrollArea(trayDock_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* tray = new Tray(scroll);
    entityInfo_ = new EntityInfoPanel(&doc_, viewport_, tray);
    materials_ = new MaterialsPanel(&doc_, viewport_, tray);
    styles_ = new StylesPanel(&doc_, viewport_, tray);
    sun_ = new SunPanel(&doc_, viewport_, tray);
    scene_ = new ScenePanel(&doc_, viewport_, tray);
    instructor_ = new InstructorPanel(tray);
    terrain_ = new TerrainPanel(&doc_, viewport_,
                                {toolActions_[ToolId::Mountain], toolActions_[ToolId::Pit], toolActions_[ToolId::Flatten],
                                 toolActions_[ToolId::TextureBrush]},
                                tray);
    sections_["Entity Info"] = tray->addSection(QStringLiteral("Entity Info"), entityInfo_, true);
    sections_["Terraform"] = tray->addSection(QStringLiteral("Terraform"), terrain_, false);
    sections_["Materials"] = tray->addSection(QStringLiteral("Materials"), materials_, true);
    sections_["Scene"] = tray->addSection(QStringLiteral("Scene"), scene_, false);
    sections_["Styles"] = tray->addSection(QStringLiteral("Styles"), styles_, false);
    sections_["Sun"] = tray->addSection(QStringLiteral("Sun"), sun_, false);
    sections_["Instructor"] = tray->addSection(QStringLiteral("Instructor"), instructor_, false);
    scroll->setWidget(tray);
    trayDock_->setWidget(scroll);
    trayDock_->setMinimumWidth(368);
    addDockWidget(Qt::RightDockWidgetArea, trayDock_);

    hierarchyDock_ = new QDockWidget(QStringLiteral("Hierarchy"), this);
    hierarchyDock_->setObjectName("Hierarchy");
    hierarchy_ = new HierarchyPanel(&doc_, viewport_, acts_["explode"], acts_["delete"], acts_["entityprops"], hierarchyDock_);
    hierarchyDock_->setWidget(hierarchy_);
    hierarchyDock_->setMinimumWidth(230);
    addDockWidget(Qt::LeftDockWidgetArea, hierarchyDock_);
}

void MainWindow::buildStatusBar() {
    QStatusBar* sb = statusBar();
    sb->setSizeGripEnabled(true);
    auto iconLabel = [&](const char* name, const QString& tip) {
        auto* l = new QToolButton(sb);
        l->setIcon(Icons::get(name));
        l->setIconSize(QSize(16, 16));
        l->setAutoRaise(true);
        l->setToolTip(tip);
        sb->addWidget(l);
        return l;
    };
    iconLabel("geo", QStringLiteral("Model location (not set)"));
    iconLabel("credits", QStringLiteral("GNUSketch is free software"));
    QToolButton* help = iconLabel("help", QStringLiteral("Show Instructor"));
    connect(help, &QToolButton::clicked, acts_["instructor"], &QAction::trigger);
    statusLabel_ = new QLabel(sb);
    statusLabel_->setMinimumWidth(100);
    sb->addWidget(statusLabel_, 1);
    vcbLabel_ = new QLabel(QStringLiteral("Measurements"), sb);
    vcbEdit_ = new QLineEdit(sb);
    vcbEdit_->setFixedWidth(190);
    vcbEdit_->setObjectName("Measurements");
    sb->addPermanentWidget(vcbLabel_);
    sb->addPermanentWidget(vcbEdit_);
    connect(vcbEdit_, &QLineEdit::returnPressed, this, [this] {
        QString t = vcbEdit_->text();
        viewport_->setFocus();
        viewport_->vcbEnter(t);
    });
}

// ---------------------------------------------------------------------------
// Document notifications
// ---------------------------------------------------------------------------

void MainWindow::onDocChanged() {
    updateTitle();
    updateUndoActions();
    entityInfo_->refresh();
    materials_->refresh();
    scene_->refresh();
    if (hierarchy_) hierarchy_->refresh();
    if (materialEditor_ && materialEditor_->isVisible()) materialEditor_->refresh();
    if (entityProps_ && entityProps_->isVisible()) entityProps_->refresh();
    if (materialBrowser_) materialBrowser_->refreshFromModel();
    viewport_->update();
}

void MainWindow::onSelectionChanged() {
    entityInfo_->refresh();
    scene_->refresh();
    if (hierarchy_) hierarchy_->refresh();
    if (doc_.selection.objects.size() == 1 && !doc_.selection.faces.size() && !doc_.selection.edges.size() && !doc_.selection.instances.size())
        sections_["Scene"]->setExpanded(true);
    bool any = !doc_.selection.empty();
    for (const char* k : {"cut", "copy", "delete", "hide", "roundcorners"}) acts_[k]->setEnabled(any);
    acts_["traceimage"]->setEnabled(!doc_.selection.faces.empty());
    const bool effectTargets = effectFaceCount(doc_.state, doc_.selection) > 0;
    for (const EffectDef& d : effectRegistry()) acts_[d.key()]->setEnabled(effectTargets);
    acts_["repeateffect"]->setEnabled(effectTargets && lastEffect_);
    const bool geometry = !doc_.selection.faces.empty() || !doc_.selection.edges.empty();
    const bool instances = !doc_.selection.instances.empty();
    acts_["makecomponent"]->setEnabled(geometry || instances);
    acts_["saveprefab"]->setEnabled(geometry || instances);
    acts_["editcomponent"]->setEnabled(doc_.selection.instances.size() == 1);
    acts_["entityprops"]->setEnabled(doc_.selection.instances.size() == 1 || doc_.editingComponent());
    acts_["closecomponent"]->setEnabled(doc_.editingComponent());
    acts_["explode"]->setEnabled(instances);
    bool shared = false;
    for (Id i : doc_.selection.instances) shared = shared || instanceCount(doc_.state, doc_.state.instances[i].def) > 1;
    acts_["makeunique"]->setEnabled(shared);
    viewport_->update();
}

void MainWindow::updateTitle() {
    QString name = doc_.filePath.empty() ? QStringLiteral("Untitled") : QFileInfo(QString::fromStdString(doc_.filePath)).fileName();
    setWindowTitle(QStringLiteral("%1%2 - GNUSketch").arg(name, doc_.modified ? QStringLiteral("*") : QString()));
}

void MainWindow::updateUndoActions() {
    undoAct_->setEnabled(doc_.canUndo());
    redoAct_->setEnabled(doc_.canRedo());
    undoAct_->setText(doc_.canUndo() ? QStringLiteral("&Undo %1").arg(QString::fromStdString(doc_.undoName())) : QStringLiteral("&Undo"));
    redoAct_->setText(doc_.canRedo() ? QStringLiteral("&Redo %1").arg(QString::fromStdString(doc_.redoName())) : QStringLiteral("&Redo"));
}

void MainWindow::syncStyleActions() {
    const RenderStyle& s = doc_.style;
    for (QAction* a : {xrayAct_, backEdgesAct_, edgesAct_, profilesAct_, axesAct_, guidesAct_, hiddenAct_, shadowsAct_, aoAct_, antialiasAct_, sunAct_, cullAct_, objectsAct_}) a->blockSignals(true);
    xrayAct_->setChecked(s.xray);
    backEdgesAct_->setChecked(s.backEdges);
    edgesAct_->setChecked(s.edges);
    profilesAct_->setChecked(s.profiles);
    axesAct_->setChecked(s.showAxes);
    guidesAct_->setChecked(s.showGuides);
    objectsAct_->setChecked(s.showObjects);
    hiddenAct_->setChecked(s.showHidden);
    shadowsAct_->setChecked(s.shadows);
    aoAct_->setChecked(s.ambientOcclusion);
    antialiasAct_->setChecked(s.antialiasing);
    sunAct_->setChecked(s.sun);
    cullAct_->setChecked(s.backfaceCulling);
    for (QAction* a : {xrayAct_, backEdgesAct_, edgesAct_, profilesAct_, axesAct_, guidesAct_, hiddenAct_, shadowsAct_, aoAct_, antialiasAct_, sunAct_, cullAct_, objectsAct_}) a->blockSignals(false);
    auto it = faceStyleActs_.find((int)s.faceStyle);
    if (it != faceStyleActs_.end()) it->second->setChecked(true);
    perspectiveAct_->setChecked(viewport_->camera().perspective);
    parallelAct_->setChecked(!viewport_->camera().perspective);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

bool MainWindow::maybeSave() {
    if (!doc_.modified) return true;
    auto r = QMessageBox::question(this, QStringLiteral("GNUSketch"),
                                   QStringLiteral("Save changes to the current model?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return save();
    return true;
}

void MainWindow::newFile() {
    if (!maybeSave()) return;
    viewport_->cancelTool();
    doc_.reset();
    viewport_->camera() = Camera();
    viewport_->update();
    updateTitle();
}

void MainWindow::open() {
    if (!maybeSave()) return;
    QSettings s;
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Model"), s.value("files/lastDir").toString(),
                                                QStringLiteral("GNUSketch Model (*.gsketch);;Wavefront OBJ (*.obj)") +
                                                    (Plugins::fileFilters(false).isEmpty() ? QString() : ";;" + Plugins::fileFilters(false)));
    if (path.isEmpty()) return;
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    openFile(path);
}

bool MainWindow::openFile(const QString& path) {
    viewport_->cancelTool();
    QString err;
    if (FormatPlugin* f = Plugins::forFile(path, false)) {
        const bool ok = importWithPlugin(f, path, true);
        if (ok) addRecentFile(path);
        return ok;
    }
    if (path.endsWith(".obj", Qt::CaseInsensitive)) {
        doc_.reset();
        if (gs::importObj(doc_, path, true, 1.0, &err) < 0) {
            QMessageBox::warning(this, QStringLiteral("Open"), err);
            return false;
        }
        doc_.modified = true;
        viewport_->zoomExtents();
        onDocChanged();
        addRecentFile(path);
        return true;
    }
    QJsonObject view;
    if (!loadNative(doc_, path, &view, &err)) {
        QMessageBox::warning(this, QStringLiteral("Open"), QStringLiteral("Could not open %1:\n%2").arg(path, err));
        return false;
    }
    applyViewState(view);
    styles_->refresh();
    syncStyleActions();
    onDocChanged();
    onSelectionChanged();
    addRecentFile(path);
    return true;
}

bool MainWindow::save() {
    if (doc_.filePath.empty() || !QString::fromStdString(doc_.filePath).endsWith(".gsketch")) return saveAs();
    QString err;
    if (!saveNative(doc_, QString::fromStdString(doc_.filePath), viewState(), &err)) {
        QMessageBox::warning(this, QStringLiteral("Save"), err);
        return false;
    }
    doc_.modified = false;
    updateTitle();
    addRecentFile(QString::fromStdString(doc_.filePath));
    statusLabel_->setText(QStringLiteral("Saved %1").arg(QString::fromStdString(doc_.filePath)));
    return true;
}

bool MainWindow::saveAs() {
    QSettings s;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Model"), s.value("files/lastDir").toString(),
                                                QString::fromUtf8(kFileFilter));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".gsketch")) path += ".gsketch";
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    doc_.filePath = path.toStdString();
    return save();
}

void MainWindow::importObj() {
    QSettings s;
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import OBJ"), s.value("files/lastDir").toString(),
                                                QStringLiteral("Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return;
    viewport_->cancelTool();
    QString err;
    doc_.beginOp("Import");
    int n = gs::importObj(doc_, path, true, 1.0, &err);
    if (n < 0) {
        doc_.cancelOp();
        QMessageBox::warning(this, QStringLiteral("Import"), err);
        return;
    }
    doc_.commitOp();
    viewport_->zoomExtents();
    statusLabel_->setText(QStringLiteral("Imported %1 faces").arg(n));
}

bool MainWindow::importWithPlugin(FormatPlugin* f, const QString& path, bool intoEmpty) {
    viewport_->cancelTool();
    if (intoEmpty) doc_.reset();
    QStringList msgs;
    doc_.beginOp(QStringLiteral("Import %1").arg(f->formatName()).toStdString());
    if (!f->importFile(doc_, path, &msgs)) {
        doc_.cancelOp();
        QMessageBox::warning(this, intoEmpty ? QStringLiteral("Open") : QStringLiteral("Import"),
                             QStringLiteral("Could not read %1:\n%2").arg(path, msgs.join('\n')));
        return false;
    }
    doc_.commitOp();
    if (intoEmpty) doc_.clearHistory();
    doc_.modified = true;
    viewport_->zoomExtents();
    onDocChanged();
    onSelectionChanged();
    if (!msgs.isEmpty()) QMessageBox::information(this, f->formatName(), msgs.join('\n'));
    statusLabel_->setText(QStringLiteral("Read %1").arg(QFileInfo(path).fileName()));
    return true;
}

void MainWindow::importPlugin(FormatPlugin* f) {
    QSettings s;
    QStringList pats;
    for (const QString& e : f->extensions()) pats << "*." + e;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import %1").arg(f->formatName()), s.value("files/lastDir").toString(),
                                                      QStringLiteral("%1 (%2)").arg(f->formatName(), pats.join(' ')));
    if (path.isEmpty()) return;
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    importWithPlugin(f, path, false);
}

void MainWindow::exportPlugin(FormatPlugin* f) {
    QSettings s;
    QStringList pats;
    for (const QString& e : f->extensions()) pats << "*." + e;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export %1").arg(f->formatName()), s.value("files/lastDir").toString(),
                                                QStringLiteral("%1 (%2)").arg(f->formatName(), pats.join(' ')));
    if (path.isEmpty()) return;
    if (!f->extensions().isEmpty() && !f->extensions().contains(QFileInfo(path).suffix().toLower())) path += "." + f->extensions().first();
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    QStringList msgs;
    if (!f->exportFile(doc_, path, &msgs)) {
        QMessageBox::warning(this, QStringLiteral("Export"), QStringLiteral("Could not write %1:\n%2").arg(path, msgs.join('\n')));
        return;
    }
    if (!msgs.isEmpty()) QMessageBox::information(this, f->formatName(), msgs.join('\n'));
    statusLabel_->setText(QStringLiteral("Exported %1").arg(path));
}

void MainWindow::showPlugins() {
    QString html = QStringLiteral("<h3>Plugins</h3>");
    if (Plugins::all().empty()) html += QStringLiteral("<p>None loaded.</p>");
    else {
        html += QStringLiteral("<table cellpadding=3>");
        for (const LoadedPlugin& p : Plugins::all()) {
            QStringList can;
            if (p.format->canImport()) can << QStringLiteral("open, import");
            if (p.format->canExport()) can << QStringLiteral("export");
            html += QStringLiteral("<tr><td><b>%1</b> (.%2)</td><td>%3</td></tr><tr><td colspan=2><small>%4<br>%5</small></td></tr>")
                        .arg(p.format->formatName().toHtmlEscaped(), p.format->extensions().join(", ."), can.join(", "),
                             p.format->description().toHtmlEscaped(), p.file.toHtmlEscaped());
        }
        html += QStringLiteral("</table>");
    }
    QStringList folders;
    for (const QString& d : Plugins::folders()) folders << d.toHtmlEscaped();
    html += QStringLiteral("<p>Looked in:<br><small>%1</small></p>").arg(folders.join("<br>"));
    if (!Plugins::problems().isEmpty())
        html += QStringLiteral("<p>Not loaded:<br><small>%1</small></p>").arg(Plugins::problems().join('\n').toHtmlEscaped().replace('\n', "<br>"));
    QMessageBox::information(this, QStringLiteral("Plugins"), html);
}

void MainWindow::exportObj() {
    QSettings s;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export OBJ"), s.value("files/lastDir").toString(),
                                                QStringLiteral("Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".obj", Qt::CaseInsensitive)) path += ".obj";
    QString err;
    if (!gs::exportObj(doc_, path, {}, &err)) QMessageBox::warning(this, QStringLiteral("Export"), err);
    else statusLabel_->setText(QStringLiteral("Exported %1").arg(path));
}

void MainWindow::exportGltf() {
    QSettings s;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export glTF"), s.value("files/lastDir").toString(),
                                                QStringLiteral("glTF 2.0 (*.gltf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".gltf", Qt::CaseInsensitive)) path += ".gltf";
    QString err;
    if (!gs::exportGltf(doc_, path, &err)) QMessageBox::warning(this, QStringLiteral("Export"), err);
    else statusLabel_->setText(QStringLiteral("Exported %1").arg(path));
}

void MainWindow::exportStl() { exportForPrint((int)PrintFormat::StlBinary); }

void MainWindow::exportForPrint(int format) {
    const auto fmt = (PrintFormat)format;
    // Unit: the model's own by default (that is what "keeping units" means for
    // the formats without a unit field).
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Export %1").arg(QString::fromLatin1(printFormatName(fmt))));
    auto* form = new QFormLayout(&dlg);
    auto* unitBox = new QComboBox(&dlg);
    unitBox->setObjectName(QStringLiteral("PrintUnit"));
    unitBox->addItems({QStringLiteral("Millimeters"), QStringLiteral("Centimeters"), QStringLiteral("Meters"), QStringLiteral("Inches"), QStringLiteral("Feet")});
    unitBox->setCurrentIndex((int)doc_.units.unit);
    form->addRow(QStringLiteral("Unit:"), unitBox);
    auto* note = new QLabel(&dlg);
    note->setProperty("hint", true);
    note->setWordWrap(true);
    auto updateNote = [&] {
        const auto u = (LengthUnit)unitBox->currentIndex();
        const bool declared = fmt == PrintFormat::ThreeMF || fmt == PrintFormat::Amf;
        QString t = declared ? QStringLiteral("The unit is stored in the file (%1); slicers scale it correctly.").arg(QString::fromLatin1(printUnitName(fmt, u)))
                             : QStringLiteral("This format has no unit field: numbers are written in %1. Most slicers assume millimeters.")
                                   .arg(unitBox->currentText().toLower());
        if (printUnitFor(fmt, u) != u) t += QStringLiteral(" AMF has no centimeters, millimeters are used.");
        Vec3 mn, mx;
        bool any = false;
        flattenModel(doc_.state).bounds(mn, mx, any);
        if (any) {
            const double k = 1.0 / Units::toMeters(printUnitFor(fmt, u));
            const Vec3 d = (mx - mn) * k;
            t += QStringLiteral("\nSize: %1 × %2 × %3").arg(d.x, 0, 'g', 6).arg(d.y, 0, 'g', 6).arg(d.z, 0, 'g', 6);
        }
        note->setText(t);
    };
    updateNote();
    connect(unitBox, &QComboBox::currentIndexChanged, &dlg, updateNote);
    form->addRow(note);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const auto unit = (LengthUnit)unitBox->currentIndex();

    QSettings s;
    const QString ext = QString::fromLatin1(printFormatExtension(fmt));
    QString path = QFileDialog::getSaveFileName(this, dlg.windowTitle(), s.value("files/lastDir").toString(),
                                                QStringLiteral("%1 (*.%2)").arg(QString::fromLatin1(printFormatName(fmt)), ext));
    if (path.isEmpty()) return;
    if (!path.endsWith("." + ext, Qt::CaseInsensitive)) path += "." + ext;
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    QString err;
    if (!exportPrint(doc_, path, fmt, unit, &err)) QMessageBox::warning(this, QStringLiteral("Export"), err);
    else statusLabel_->setText(QStringLiteral("Exported %1 (%2)").arg(path, QString::fromLatin1(printUnitName(fmt, unit))));
}

void MainWindow::showSettings() {
    SettingsDialog dlg(this);
    dlg.exec();
    viewport_->update();
}

QStringList MainWindow::recentFiles() const {
    QStringList out;
    for (const QString& p : QSettings().value("files/recent").toStringList())
        if (QFileInfo::exists(p) && !out.contains(p)) out << p;
    return out;
}

void MainWindow::addRecentFile(const QString& path) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    QStringList list = QSettings().value("files/recent").toStringList();
    list.removeAll(abs);
    list.prepend(abs);
    while (list.size() > 12) list.removeLast();
    QSettings().setValue("files/recent", list);
    rebuildRecentMenu();
}

void MainWindow::clearRecentFiles() {
    QSettings().remove("files/recent");
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    const QStringList list = recentFiles();
    for (int i = 0; i < list.size(); ++i) {
        const QString path = list[i];
        QString label = QFileInfo(path).fileName();
        if (i < 9) label = QStringLiteral("&%1  %2").arg(i + 1).arg(label);
        QAction* a = recentMenu_->addAction(label, this, [this, path] {
            if (!maybeSave()) return;
            openFile(path);
        });
        a->setToolTip(path);
        a->setStatusTip(path);
    }
    if (list.isEmpty()) recentMenu_->addAction(QStringLiteral("No recent files"))->setEnabled(false);
    else {
        recentMenu_->addSeparator();
        recentMenu_->addAction(QStringLiteral("Clear Recent Files"), this, [this] { clearRecentFiles(); });
    }
    recentMenu_->setEnabled(true);
}

QJsonObject MainWindow::viewState() const {
    QJsonObject o;
    const Camera& c = viewport_->camera();
    QJsonObject cam;
    cam["eye"] = vec(c.eye);
    cam["target"] = vec(c.target);
    cam["fov"] = c.fovY;
    cam["perspective"] = c.perspective;
    o["camera"] = cam;
    o["style"] = styleToJson(doc_.style);
    o["units"] = (int)doc_.units.unit;
    o["precision"] = doc_.units.precision;
    return o;
}

void MainWindow::applyViewState(const QJsonObject& o) {
    Camera& c = viewport_->camera();
    if (o.contains("camera")) {
        QJsonObject cam = o["camera"].toObject();
        c.eye = vec(cam["eye"], c.eye);
        c.target = vec(cam["target"], c.target);
        c.fovY = cam["fov"].toDouble(35.0);
        c.perspective = cam["perspective"].toBool(true);
    } else {
        viewport_->zoomExtents();
    }
    if (o.contains("style")) styleFromJson(o["style"].toObject(), doc_.style);
    if (o.contains("units")) doc_.units.unit = (LengthUnit)std::clamp(o["units"].toInt(), 0, 4);
    if (o.contains("precision")) doc_.units.precision = std::clamp(o["precision"].toInt(), 0, 6);
    viewport_->update();
}

// ---------------------------------------------------------------------------
// Editing commands
// ---------------------------------------------------------------------------

// Geometry goes to the system clipboard as .gsketch JSON under this type, so
// it can be pasted into any GNUSketch window or process.
static const QString kClipMime = QStringLiteral("application/x-gnusketch");

void MainWindow::copySelection(bool cut) {
    if (doc_.selection.empty()) return;
    ModelState clip;
    {
        std::map<Id, Id> defMap;
        for (Id i : doc_.selection.instances) copyInstanceInto(clip, doc_.state, i, defMap);
    }
    for (Id o : doc_.selection.objects) clip.objects.push_back(doc_.state.objects[o]);
    clip.mesh = doc_.mesh();
    Mesh& m = clip.mesh;
    std::set<Id> keepEdges = doc_.selection.edges;
    for (Id f : doc_.selection.faces)
        for (Id e : m.faceEdges(f)) keepEdges.insert(e);
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (!doc_.selection.faces.count(f)) m.faces[f].alive = false;
    for (Id e = 0; e < (Id)m.edges.size(); ++e)
        if (!keepEdges.count(e)) m.edges[e].alive = false;
    m.removeOrphanVertices();
    m.compact();
    // Only the materials the copied faces use.
    std::map<Id, Id> matMap;
    std::vector<Face*> clipFaces;
    for (Face& f : m.faces) clipFaces.push_back(&f);
    for (ComponentDef& d : clip.defs)
        for (Face& f : d.mesh.faces) clipFaces.push_back(&f);
    auto mapMaterial = [&](Id& mat) {
        if (mat < 0 || mat >= (Id)doc_.state.materials.size()) {
            mat = kNone;
            return;
        }
        auto it = matMap.find(mat);
        if (it == matMap.end()) {
            it = matMap.emplace(mat, (Id)clip.materials.size()).first;
            clip.materials.push_back(doc_.state.materials[mat]);
        }
        mat = it->second;
    };
    for (Face* fp : clipFaces)
        for (FaceSide* s : {&fp->front, &fp->back}) mapMaterial(s->material);
    // Textures painted with the Texture Brush.
    std::vector<Mesh*> clipMeshes = {&m};
    for (ComponentDef& d : clip.defs) clipMeshes.push_back(&d.mesh);
    for (Mesh* cm : clipMeshes)
        for (Vertex& v : cm->vertices) {
            for (PaintWeight& w : v.paint) mapMaterial(w.material);
            std::erase_if(v.paint, [](const PaintWeight& w) { return w.material == kNone; });
        }
    const size_t count = m.faces.size() + m.edges.size() + clip.instances.size();
    auto* md = new QMimeData;
    md->setData(kClipMime, modelToJson(clip));
    QApplication::clipboard()->setMimeData(md);
    if (cut) deleteSelection();
    statusLabel_->setText(cut ? QStringLiteral("Cut %1 entities").arg(count) : QStringLiteral("Copied %1 entities").arg(count));
}

bool MainWindow::pasteImages(const QMimeData* md) {
    QPoint local = viewport_->mapFromGlobal(QCursor::pos());
    if (!viewport_->rect().contains(local)) local = viewport_->rect().center();
    if (md->hasImage()) {
        QImage img = qvariant_cast<QImage>(md->imageData());
        if (!img.isNull()) return viewport_->addImagePlane(textureFromImage(img), QStringLiteral("Pasted image"), local.x(), local.y());
    }
    bool any = false;
    if (md->hasUrls()) {
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile() || !isImageFile(u.toLocalFile())) continue;
            if (!any) any = viewport_->addImagePlane(u.toLocalFile(), local.x(), local.y());
            else materials_->addImageMaterial(u.toLocalFile());
        }
    }
    return any;
}

void MainWindow::paste() {
    const QMimeData* md = QApplication::clipboard()->mimeData();
    if (!md) return;
    viewport_->cancelTool();
    ModelState c;
    if (!md->hasFormat(kClipMime) || !modelFromJson(md->data(kClipMime), c)) {
        if (!pasteImages(md)) statusLabel_->setText(QStringLiteral("Nothing to paste: copy GNUSketch geometry or an image first."));
        return;
    }
    viewport_->setTool(ToolId::Move);
    Mesh& m = doc_.mesh();
    doc_.beginOp("Paste");
    // Map clipboard materials onto document materials (when they look the same).
    std::vector<Id> matMap(c.materials.size(), kNone);
    for (size_t i = 0; i < c.materials.size(); ++i) {
        const Material& cm = c.materials[i];
        for (Id j = 0; j < (Id)doc_.state.materials.size(); ++j) {
            if (sameMaterial(doc_.state.materials[j], cm)) matMap[i] = j;
        }
    }
    auto mapMat = [&](Id id) -> Id {
        if (id < 0 || id >= (Id)matMap.size()) return kNone;
        if (matMap[id] == kNone) matMap[id] = doc_.addMaterial(c.materials[id]);
        return matMap[id];
    };
    // Pasted vertices stay separate from the model (no weld): the paste lands
    // in place and is moved next; the move welds wherever it is dropped.
    std::vector<Id> vmap(c.mesh.vertices.size());
    for (size_t i = 0; i < c.mesh.vertices.size(); ++i) {
        vmap[i] = m.addVertexRaw(c.mesh.vertices[i].p);
        for (PaintWeight w : c.mesh.vertices[i].paint)
            if ((w.material = mapMat(w.material)) != kNone) m.vertices[vmap[i]].paint.push_back(w);
    }
    Selection sel;
    std::map<Id, Id> curveMap;
    for (const Edge& e : c.mesh.edges) {
        Id ne = m.addEdgeRaw(vmap[e.v[0]], vmap[e.v[1]]);
        if (ne == kNone) continue;
        m.edges[ne].soft = e.soft;
        m.edges[ne].smooth = e.smooth;
        m.edges[ne].hidden = e.hidden;
        if (e.curve != kNone) {
            if (!curveMap.count(e.curve)) curveMap[e.curve] = m.newCurveId();
            m.edges[ne].curve = curveMap[e.curve];
        }
        sel.edges.insert(ne);
    }
    for (const Face& f : c.mesh.faces) {
        std::vector<Id> o;
        for (Id v : f.outer) o.push_back(vmap[v]);
        std::vector<std::vector<Id>> hs;
        for (const auto& h : f.holes) {
            std::vector<Id> nh;
            for (Id v : h) nh.push_back(vmap[v]);
            hs.push_back(nh);
        }
        Id nf = m.addFaceRaw(o, hs, f.normal);
        if (nf == kNone) continue;
        m.faces[nf].front = f.front;
        m.faces[nf].back = f.back;
        m.faces[nf].front.material = mapMat(f.front.material);
        m.faces[nf].back.material = mapMat(f.back.material);
        m.faces[nf].hidden = f.hidden;
        sel.faces.insert(nf);
    }
    // Components: reuse a definition with the same identity so pasted copies
    // stay linked to the originals. Materials merge on the way in.
    for (const Instance& in : c.instances) {
        if (in.parent != kNone || in.def < 0 || in.def >= (Id)c.defs.size()) continue;
        const Id d = importDefinition(doc_, c, in.def);
        if (d == kNone) continue;
        if (!canPlaceInActive(doc_.state, d)) {
            // Into itself: paste its geometry instead.
            appendMesh(m, c.defs[in.def].mesh, in.xf, nullptr, nullptr, false, &sel);
            continue;
        }
        Instance ni = in;
        ni.def = d;
        ni.parent = doc_.state.activeDef();
        ni.alive = true;
        doc_.state.instances.push_back(ni);
        sel.instances.insert((Id)doc_.state.instances.size() - 1);
    }
    if (!doc_.editingComponent())
        for (const SceneObject& o : c.objects) sel.objects.insert(doc_.addObject(o));
    doc_.selection = sel;
    doc_.validateSelection();
    doc_.notifySelection();
    // Grab the paste by the bottom corner of its bounding box and let it follow
    // the cursor; the click that places it finishes the "Paste" operation.
    Vec3 mn(1e300, 1e300, 1e300);
    bool any = false;
    auto grow = [&](const Vec3& p) {
        for (int k = 0; k < 3; ++k) mn[k] = std::min(mn[k], p[k]);
        any = true;
    };
    for (Id v : selectionVertices(doc_.mesh(), sel)) grow(doc_.mesh().pos(v));
    for (Id i : sel.instances) {
        Vec3 a, b;
        if (instanceBounds(doc_.state, i, a, b)) grow(a);
    }
    for (Id o : sel.objects) grow(doc_.state.objects[o].pos);
    QPoint local = viewport_->mapFromGlobal(QCursor::pos());
    if (!viewport_->rect().contains(local)) local = viewport_->rect().center();
    auto* move = dynamic_cast<MoveTool*>(viewport_->tool());
    if (!any || !move || !move->startPlacement(mn, local.x(), local.y())) {
        doc_.commitOp();
        statusLabel_->setText(QStringLiteral("Pasted in place."));
        return;
    }
    viewport_->setFocus();
    viewport_->update();
}

void MainWindow::deleteSelection() {
    if (doc_.selection.empty()) return;
    viewport_->cancelTool();
    Mesh& m = doc_.mesh();
    doc_.beginOp("Erase");
    for (Id f : doc_.selection.faces) m.eraseFace(f);
    for (Id e : doc_.selection.edges) m.eraseEdge(e, false);
    for (Id i : doc_.selection.instances) doc_.state.instances[i].alive = false;
    for (Id o : doc_.selection.objects) doc_.state.objects[o].alive = false;
    m.removeOrphanVertices();
    m.touch();
    doc_.selection.clear();
    doc_.commitOp();
    doc_.notifySelection();
}

void MainWindow::selectAll() {
    const Mesh& m = doc_.mesh();
    doc_.selection.clear();
    for (Id f = 0; f < (Id)m.faces.size(); ++f)
        if (m.faces[f].alive && !m.faces[f].hidden) doc_.selection.faces.insert(f);
    for (Id e = 0; e < (Id)m.edges.size(); ++e)
        if (m.edges[e].alive && !m.edges[e].hidden) doc_.selection.edges.insert(e);
    const auto& insts = doc_.state.instances;
    for (Id i = 0; i < (Id)insts.size(); ++i)
        if (insts[i].alive && !insts[i].hidden && insts[i].parent == doc_.state.activeDef()) doc_.selection.instances.insert(i);
    const auto& objs = doc_.state.objects;
    for (Id i = 0; i < (Id)objs.size() && !doc_.editingComponent() && doc_.style.showObjects; ++i)
        if (objs[i].alive) doc_.selection.objects.insert(i);
    doc_.notifySelection();
}

void MainWindow::hideSelection() {
    if (doc_.selection.empty()) return;
    doc_.beginOp("Hide");
    for (Id f : doc_.selection.faces) doc_.mesh().faces[f].hidden = true;
    for (Id e : doc_.selection.edges) doc_.mesh().edges[e].hidden = true;
    for (Id i : doc_.selection.instances) doc_.state.instances[i].hidden = true;
    for (Id o : doc_.selection.objects) doc_.state.objects[o].hidden = true;
    doc_.mesh().touch();
    doc_.selection.clear();
    doc_.commitOp();
    doc_.notifySelection();
}

void MainWindow::unhideAll() {
    doc_.beginOp("Unhide All");
    for (Face& f : doc_.mesh().faces) f.hidden = false;
    for (Edge& e : doc_.mesh().edges) e.hidden = false;
    for (Instance& i : doc_.state.instances) i.hidden = false;
    for (SceneObject& o : doc_.state.objects) o.hidden = false;
    doc_.mesh().touch();
    doc_.commitOp();
}

void MainWindow::reverseFaces() {
    if (doc_.selection.faces.empty()) return;
    doc_.beginOp("Reverse Faces");
    for (Id f : doc_.selection.faces) doc_.mesh().reverseFace(f);
    doc_.mesh().touch();
    doc_.commitOp();
}

void MainWindow::softenSelection(bool soft) {
    std::set<Id> edges = doc_.selection.edges;
    if (edges.empty()) {
        // Soften the edges between selected faces.
        for (Id f : doc_.selection.faces)
            for (Id e : doc_.mesh().faceEdges(f)) {
                int n = 0;
                for (Id g : doc_.mesh().edgeFaces(e)) n += doc_.selection.faces.count(g);
                if (n >= 2) edges.insert(e);
            }
    }
    if (edges.empty()) return;
    doc_.beginOp(soft ? "Soften Edges" : "Unsoften Edges");
    for (Id e : edges) doc_.mesh().edges[e].soft = doc_.mesh().edges[e].smooth = soft;
    doc_.mesh().touch();
    doc_.commitOp();
}

void MainWindow::textureFitSelection(bool fit) {
    if (doc_.selection.faces.empty()) return;
    Mesh& m = doc_.mesh();
    const Camera& c = viewport_->camera();
    doc_.beginOp(fit ? "Fit Texture" : "Reset Texture");
    for (Id f : doc_.selection.faces) {
        bool front = c.perspective ? m.faces[f].normal.dot(c.eye - m.faceInteriorPoint(f)) > 0
                                   : m.faces[f].normal.dot(c.forward()) < 0;
        FaceSide& s = front ? m.faces[f].front : m.faces[f].back;
        if (fit) s.tex = fitTextureToFace(m, f, front);
        else s.tex.valid = false;
    }
    m.touch();
    doc_.commitOp();
}

void MainWindow::deleteGuides() {
    if (doc_.state.guides.empty()) return;
    doc_.beginOp("Delete Guides");
    doc_.state.guides.clear();
    doc_.mesh().touch();
    doc_.commitOp();
}

void MainWindow::modelInfo() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Model Info"));
    auto* fl = new QFormLayout(&dlg);
    auto* unit = new QComboBox(&dlg);
    unit->addItems({QStringLiteral("Millimeters"), QStringLiteral("Centimeters"), QStringLiteral("Meters"),
                    QStringLiteral("Inches"), QStringLiteral("Feet and inches")});
    unit->setCurrentIndex((int)doc_.units.unit);
    auto* prec = new QSpinBox(&dlg);
    prec->setRange(0, 6);
    prec->setValue(doc_.units.precision);
    fl->addRow(QStringLiteral("Length unit:"), unit);
    fl->addRow(QStringLiteral("Precision (decimals):"), prec);
    const Mesh& m = doc_.mesh();
    fl->addRow(QStringLiteral("Statistics:"), new QLabel(QStringLiteral("%1 faces, %2 edges, %3 vertices, %4 materials, %5 guides")
                                                             .arg(m.aliveFaceCount()).arg(m.aliveEdgeCount()).arg(m.aliveVertexCount())
                                                             .arg(doc_.state.materials.size()).arg(doc_.state.guides.size()), &dlg));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    fl->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    doc_.units.unit = (LengthUnit)unit->currentIndex();
    doc_.units.precision = prec->value();
    QSettings().setValue("model/units", unit->currentIndex());
    entityInfo_->refresh();
    materials_->refresh();
}

void MainWindow::runEffect(const EffectDef& def, bool repeat) {
    viewport_->cancelTool();
    const Selection original = doc_.selection;
    const int before = effectFaceCount(doc_.state, original);
    if (before == 0) {
        statusBar()->showMessage(QStringLiteral("Select faces or components for the effect first."), 5000);
        return;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("effects"));
    QVariantMap values = settings.value(def.key()).toMap();
    for (const EffectParam& p : def.params)
        if (!values.contains(p.key)) {
            if (p.type == EffectParam::Bool) values[p.key] = p.def != 0;
            else if (p.type == EffectParam::Double) values[p.key] = p.def;
            else values[p.key] = (int)p.def;
        }

    // Live preview: the operation stays open and is redone from the original
    // geometry whenever a setting changes.
    const std::string opName = def.name.toStdString();
    QString lastMessage;
    Selection result;
    bool ok = false;
    doc_.beginOp(opName);
    auto apply = [&](const QVariantMap& v, bool& okOut) {
        doc_.cancelOp();
        doc_.beginOp(opName);
        result = original;
        bool limited = false;
        std::string err;
        ok = applyEffect(doc_.state, result, [&](Mesh& m, const std::set<Id>& faces, EffectResult* out, std::string* e) {
            return def.apply(m, faces, v, out, e);
        }, &limited, &err);
        QString text;
        if (ok) {
            text = QStringLiteral("Polygons: %1 → %2").arg(before).arg(effectFaceCount(doc_.state, result));
            if (limited) text += QStringLiteral(". Detail reduced: the area is too large for this density.");
        } else {
            // Nothing half-done is left on screen.
            doc_.cancelOp();
            doc_.beginOp(opName);
            text = QString::fromStdString(err);
        }
        doc_.selection.clear(); // the result is easier to judge without the highlight
        viewport_->update();
        okOut = ok;
        lastMessage = text;
        return text;
    };

    bool accepted = true;
    if (repeat || def.params.empty()) {
        bool unused;
        apply(values, unused);
    } else {
        EffectDialog dlg(def, values, apply, this);
        accepted = dlg.exec() == QDialog::Accepted;
        values = dlg.values();
    }
    if (accepted && ok) {
        settings.setValue(def.key(), values);
        doc_.selection = result;
        doc_.commitOp();
        lastEffect_ = &def;
        acts_["repeateffect"]->setText(QStringLiteral("&Repeat %1").arg(def.name));
        statusBar()->showMessage(QStringLiteral("%1: %2").arg(def.name, lastMessage), 5000);
    } else {
        doc_.cancelOp();
        doc_.selection = original;
        if (!lastMessage.isEmpty()) statusBar()->showMessage(lastMessage, 5000);
    }
    doc_.notifySelection();
    viewport_->update();
}

void MainWindow::launchGame(const Vec3& feet, double yaw, bool native) {
    if (game_) {
        game_->disconnect(this);
        game_->kill();
        game_->waitForFinished(2000);
        game_->deleteLater();
        game_ = nullptr;
    }
    if (entityProps_) entityProps_->applyScript(); // what is being typed counts
    if (!gameDir_) gameDir_ = std::make_unique<QTemporaryDir>();
    const QString path = gameDir_->filePath(QStringLiteral("game.gsketch"));
    QString error;
    if (!gameDir_->isValid() || !saveNative(doc_, path, viewState(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Game Mode"), QStringLiteral("Could not prepare the model for the game: %1").arg(error));
        return;
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // See GameMode.h: mouse look needs XWayland on a Wayland session.
    const bool viaX = !native && QGuiApplication::platformName().startsWith(QLatin1String("wayland")) && env.contains(QStringLiteral("DISPLAY"));
    if (viaX) {
        env.remove(QStringLiteral("WAYLAND_DISPLAY"));
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb"));
    }
    game_ = new QProcess(this);
    game_->setProcessEnvironment(env);
    game_->setProcessChannelMode(QProcess::ForwardedChannels);
    auto started = std::make_shared<QElapsedTimer>();
    started->start();
    connect(game_, &QProcess::finished, this, [this, feet, yaw, viaX, started](int code, QProcess::ExitStatus status) {
        const bool failed = code != 0 || status != QProcess::NormalExit;
        if (failed && started->elapsed() < 5000) {
            if (viaX) {
                // No usable XWayland: run natively, the mouse just stops at the window edges.
                QTimer::singleShot(0, this, [this, feet, yaw] { launchGame(feet, yaw, true); });
                return;
            }
            QMessageBox::warning(this, QStringLiteral("Game Mode"), QStringLiteral("The game window could not start."));
        }
    });
    game_->start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--game"), path,
                  QStringLiteral("--spawn"), QStringLiteral("%1,%2,%3").arg(feet.x, 0, 'g', 17).arg(feet.y, 0, 'g', 17).arg(feet.z, 0, 'g', 17),
                  QStringLiteral("--yaw"), QString::number(yaw, 'g', 17)});
    statusLabel_->setText(QStringLiteral("Game Mode started in its own window."));
}

void MainWindow::roundCorners() {
    viewport_->cancelTool();
    // Selected edges plus the outlines of selected faces.
    std::set<Id> edges = doc_.selection.edges;
    for (Id f : doc_.selection.faces)
        for (Id e : doc_.mesh().faceEdges(f)) edges.insert(e);
    if (edges.empty()) {
        statusBar()->showMessage(QStringLiteral("Select the edges or faces to round first."), 5000);
        return;
    }

    QSettings settings;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Round Corners"));
    auto* fl = new QFormLayout(&dlg);
    auto* radius = new QDoubleSpinBox(&dlg);
    radius->setRange(0.01, 1e6);
    radius->setDecimals(2);
    radius->setSuffix(QStringLiteral(" mm"));
    radius->setValue(settings.value("roundCorners/radiusMm", 10.0).toDouble());
    auto* segments = new QSpinBox(&dlg);
    segments->setRange(1, 64);
    segments->setValue(settings.value("roundCorners/segments", 6).toInt());
    auto* msg = new QLabel(&dlg);
    msg->setWordWrap(true);
    fl->addRow(QStringLiteral("Radius:"), radius);
    fl->addRow(QStringLiteral("Segments:"), segments);
    fl->addRow(msg);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    fl->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // Live preview: the operation stays open while the dialog is shown and is
    // re-applied to the original geometry whenever a value changes.
    bool ok = false;
    doc_.beginOp("Round Corners");
    auto apply = [&] {
        doc_.cancelOp();
        doc_.beginOp("Round Corners");
        std::string err;
        ok = gs::roundCorners(doc_.mesh(), edges, radius->value() / 1000.0, segments->value(), &err);
        msg->setText(ok ? QString() : QString::fromStdString(err));
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        viewport_->update();
    };
    connect(radius, &QDoubleSpinBox::valueChanged, &dlg, apply);
    connect(segments, &QSpinBox::valueChanged, &dlg, apply);
    apply();

    if (dlg.exec() == QDialog::Accepted && ok) {
        settings.setValue("roundCorners/radiusMm", radius->value());
        settings.setValue("roundCorners/segments", segments->value());
        doc_.selection.clear();
        doc_.commitOp();
        doc_.notifySelection();
    } else {
        doc_.cancelOp();
    }
    viewport_->update();
}

void MainWindow::makeComponent() {
    const Selection& s = doc_.selection;
    if (s.faces.empty() && s.edges.empty() && s.instances.empty()) return;
    viewport_->cancelTool();
    bool ok = false;
    QString name = QInputDialog::getText(this, QStringLiteral("Make Component"), QStringLiteral("Name:"), QLineEdit::Normal,
                                         QString::fromStdString(uniqueComponentName(doc_.state, "Component")), &ok);
    if (!ok) return;
    doc_.beginOp("Make Component");
    Id inst = gs::makeComponent(doc_.state, s, name.trimmed().toStdString());
    if (inst == kNone) {
        doc_.cancelOp();
        return;
    }
    doc_.selection.clear();
    doc_.selection.instances.insert(inst);
    doc_.commitOp();
    doc_.notifySelection();
    statusLabel_->setText(QStringLiteral("Made component \"%1\". Ctrl+Move copies it; double-click to edit every copy at once.")
                              .arg(QString::fromStdString(doc_.state.defs[doc_.state.instances[inst].def].name)));
}

QString MainWindow::savePrefabOf(Id instance, const QString& name) {
    if (instance < 0 || instance >= (Id)doc_.state.instances.size() || !doc_.state.instances[instance].alive) return {};
    const Id def = doc_.state.instances[instance].def;
    const QImage thumb = OffscreenRenderer::instance().modelThumbnail(extractDefinition(doc_.state, def), 256);
    QString err;
    const QString path = savePrefab(doc_.state, def, name, thumb, &err, doc_.state.instances[instance].entity);
    if (path.isEmpty()) {
        statusLabel_->setText(QStringLiteral("Could not save the prefab: %1").arg(err));
        return {};
    }
    statusLabel_->setText(QStringLiteral("Saved prefab to %1").arg(path));
    if (prefabBrowser_ && prefabBrowser_->isVisible()) prefabBrowser_->rebuildNow();
    return path;
}

void MainWindow::saveAsPrefab() {
    viewport_->cancelTool();
    Selection s = doc_.selection;
    QString suggested = QStringLiteral("Prefab");
    std::set<Id> defs;
    for (Id i : s.instances) defs.insert(doc_.state.instances[i].def);
    if (defs.size() == 1) suggested = QString::fromStdString(doc_.state.defs[*defs.begin()].name);
    bool ok = true;
    // Several components: each becomes a prefab under its own name.
    QString name = suggested;
    if (defs.size() <= 1) {
        name = QInputDialog::getText(this, QStringLiteral("Save as Prefab"), QStringLiteral("Name:"), QLineEdit::Normal, suggested, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
    }
    // Loose geometry becomes a component first, like Make Component.
    if (!s.faces.empty() || !s.edges.empty()) {
        doc_.beginOp("Make Component");
        Selection loose = s;
        loose.instances.clear();
        loose.objects.clear();
        const Id inst = gs::makeComponent(doc_.state, loose, name.toStdString());
        if (inst == kNone) {
            doc_.cancelOp();
            return;
        }
        doc_.selection.clear();
        doc_.selection.instances = s.instances;
        doc_.selection.instances.insert(inst);
        doc_.commitOp();
        doc_.notifySelection();
        s = doc_.selection;
    }
    std::set<Id> done;
    QStringList saved;
    for (Id i : s.instances) {
        const Id d = doc_.state.instances[i].def;
        if (!done.insert(d).second) continue;
        const QString path = savePrefabOf(i, done.size() == 1 && s.instances.size() == 1 ? name : QString::fromStdString(doc_.state.defs[d].name));
        if (!path.isEmpty()) saved << path;
    }
    if (saved.size() > 1) statusLabel_->setText(QStringLiteral("Saved %1 prefabs to %2").arg(saved.size()).arg(assetDir(QStringLiteral("prefabs"))));
}

bool MainWindow::startPrefabPlacement(const QString& path) {
    ModelState prefab;
    QString err;
    if (!loadPrefab(path, prefab, nullptr, nullptr, &err)) {
        statusLabel_->setText(err);
        return false;
    }
    viewport_->cancelTool();
    viewport_->setTool(ToolId::Move);
    doc_.beginOp("Place Prefab");
    const Id def = importDefinition(doc_, prefab, 0);
    doc_.state.defs[def].prefab = QFileInfo(path).completeBaseName().toStdString();
    if (!canPlaceInActive(doc_.state, def)) {
        doc_.cancelOp();
        statusLabel_->setText(QStringLiteral("A component cannot go inside itself."));
        return false;
    }
    Instance in;
    in.def = def;
    in.parent = doc_.state.activeDef();
    if (!prefab.instances.empty()) in.entity = prefab.instances[0].entity;
    doc_.state.instances.push_back(in);
    return attachToCursor((Id)doc_.state.instances.size() - 1);
}

bool MainWindow::attachToCursor(Id inst) {
    doc_.selection.clear();
    doc_.selection.instances.insert(inst);
    doc_.notifySelection();
    Vec3 mn, mx;
    if (!instanceBounds(doc_.state, inst, mn, mx)) {
        doc_.cancelOp();
        return false;
    }
    QPoint local = viewport_->mapFromGlobal(QCursor::pos());
    if (!viewport_->rect().contains(local)) local = viewport_->rect().center();
    auto* move = dynamic_cast<MoveTool*>(viewport_->tool());
    if (!move || !move->startPlacement(Vec3((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, mn.z), local.x(), local.y())) {
        doc_.commitOp();
        return true;
    }
    activateWindow();
    viewport_->setFocus();
    viewport_->update();
    return true;
}

bool MainWindow::startPrimitivePlacement(const PrimitiveParams& params) {
    Mesh mesh;
    std::string err;
    if (!buildPrimitive(params, mesh, &err)) {
        statusLabel_->setText(QString::fromStdString(err));
        return false;
    }
    viewport_->cancelTool();
    viewport_->setTool(ToolId::Move);
    const std::string name = primitiveName(params.kind);
    doc_.beginOp("Add " + name);
    ComponentDef def;
    def.name = uniqueComponentName(doc_.state, name);
    def.uid = newComponentUid();
    def.mesh = std::move(mesh);
    def.mesh.touch();
    doc_.state.defs.push_back(std::move(def));
    Instance in;
    in.def = (Id)doc_.state.defs.size() - 1;
    in.parent = doc_.state.activeDef();
    doc_.state.instances.push_back(in);
    if (!attachToCursor((Id)doc_.state.instances.size() - 1)) return false;
    statusLabel_->setText(QStringLiteral("Click to place the %1. Explode it to merge it with the model's geometry.")
                              .arg(QString::fromStdString(name).toLower()));
    return true;
}

void MainWindow::insertPrimitive() {
    viewport_->cancelTool();
    QSettings settings;
    settings.beginGroup(QStringLiteral("primitive"));
    const LengthUnit unit = doc_.units.unit;
    const double unitM = Units::toMeters(unit);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Primitive"));
    auto* fl = new QFormLayout(&dlg);
    auto* kind = new QComboBox(&dlg);
    kind->setObjectName(QStringLiteral("primitiveKind"));
    for (int k = 0; k < kPrimitiveKindCount; ++k) kind->addItem(QString::fromUtf8(primitiveName(PrimitiveKind(k))), k);
    kind->setCurrentIndex(std::clamp(settings.value("kind", int(PrimitiveKind::Sphere)).toInt(), 0, kPrimitiveKindCount - 1));
    fl->addRow(QStringLiteral("Shape:"), kind);

    const PrimitiveParams defaults;
    auto length = [&](const char* key, double defMeters) {
        auto* sb = new QDoubleSpinBox(&dlg);
        sb->setObjectName(QString::fromUtf8(key));
        sb->setDecimals(unit == LengthUnit::Meters ? 3 : 2);
        sb->setRange(0, 1e6);
        sb->setSuffix(QStringLiteral(" ") + QString::fromUtf8(Units::suffix(unit)));
        sb->setValue(settings.value(key, defMeters).toDouble() / unitM);
        return sb;
    };
    auto count = [&](const char* key, int def, int lo, int hi) {
        auto* sb = new QSpinBox(&dlg);
        sb->setObjectName(QString::fromUtf8(key));
        sb->setRange(lo, hi);
        sb->setValue(settings.value(key, def).toInt());
        return sb;
    };
    auto* radius = length("radius", defaults.radius);
    auto* radius2 = length("radius2", defaults.radius2);
    auto* width = length("width", defaults.width);
    auto* depth = length("depth", defaults.depth);
    auto* height = length("height", defaults.height);
    auto* segments = count("segments", defaults.segments, 3, 512);
    auto* rings = count("rings", defaults.rings, 2, 256);
    auto* frequency = count("frequency", defaults.frequency, 1, 32);
    auto* smooth = new QCheckBox(QStringLiteral("Smooth the curved surface"), &dlg);
    smooth->setChecked(settings.value("smooth", defaults.smooth).toBool());
    auto* radiusLabel = new QLabel(&dlg);
    auto* radius2Label = new QLabel(&dlg);
    auto* ringsLabel = new QLabel(&dlg);
    fl->addRow(radiusLabel, radius);
    fl->addRow(radius2Label, radius2);
    fl->addRow(QStringLiteral("Width:"), width);
    fl->addRow(QStringLiteral("Depth:"), depth);
    fl->addRow(QStringLiteral("Height:"), height);
    fl->addRow(QStringLiteral("Segments:"), segments);
    fl->addRow(ringsLabel, rings);
    fl->addRow(QStringLiteral("Subdivisions:"), frequency);
    fl->addRow(QString(), smooth);
    auto* info = new QLabel(&dlg);
    info->setWordWrap(true);
    fl->addRow(info);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Place"));
    fl->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto params = [&] {
        PrimitiveParams p;
        p.kind = PrimitiveKind(kind->currentData().toInt());
        p.radius = radius->value() * unitM;
        p.radius2 = radius2->value() * unitM;
        p.width = width->value() * unitM;
        p.depth = depth->value() * unitM;
        p.height = height->value() * unitM;
        p.segments = segments->value();
        p.rings = rings->value();
        p.frequency = frequency->value();
        p.smooth = smooth->isChecked();
        return p;
    };
    // Shows the fields the shape uses and what it would build.
    auto refresh = [&] {
        const PrimitiveParams p = params();
        const PrimitiveFields f = primitiveFields(p.kind);
        const bool torus = p.kind == PrimitiveKind::Torus, cone = p.kind == PrimitiveKind::Cone;
        const bool platonic = p.kind >= PrimitiveKind::Tetrahedron;
        radiusLabel->setText(torus ? QStringLiteral("Ring radius:")
                             : (cone || p.kind == PrimitiveKind::Cylinder) ? QStringLiteral("Base radius:")
                             : platonic ? QStringLiteral("Radius (to corners):") : QStringLiteral("Radius:"));
        radius2Label->setText(torus ? QStringLiteral("Tube radius:") : QStringLiteral("Top radius:"));
        ringsLabel->setText(torus ? QStringLiteral("Tube segments:") : QStringLiteral("Rings:"));
        rings->setMinimum(torus ? 3 : 2);
        fl->setRowVisible(radius, f.radius);
        fl->setRowVisible(radius2, f.radius2);
        fl->setRowVisible(width, f.width);
        fl->setRowVisible(depth, f.depth);
        fl->setRowVisible(height, f.height);
        fl->setRowVisible(segments, f.segments);
        fl->setRowVisible(rings, f.rings);
        fl->setRowVisible(frequency, f.frequency);
        fl->setRowVisible(smooth, f.smooth);
        Mesh m;
        std::string err;
        const bool ok = buildPrimitive(p, m, &err);
        info->setText(ok ? QStringLiteral("%1 faces, %2 edges").arg(m.aliveFaceCount()).arg(m.aliveEdgeCount())
                         : QString::fromStdString(err));
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        dlg.adjustSize();
    };
    connect(kind, &QComboBox::currentIndexChanged, &dlg, refresh);
    for (QDoubleSpinBox* sb : {radius, radius2, width, depth, height}) connect(sb, &QDoubleSpinBox::valueChanged, &dlg, refresh);
    for (QSpinBox* sb : {segments, rings, frequency}) connect(sb, &QSpinBox::valueChanged, &dlg, refresh);
    connect(smooth, &QCheckBox::toggled, &dlg, refresh);
    refresh();

    if (dlg.exec() != QDialog::Accepted) return;
    const PrimitiveParams p = params();
    settings.setValue("kind", int(p.kind));
    settings.setValue("radius", p.radius);
    settings.setValue("radius2", p.radius2);
    settings.setValue("width", p.width);
    settings.setValue("depth", p.depth);
    settings.setValue("height", p.height);
    settings.setValue("segments", p.segments);
    settings.setValue("rings", p.rings);
    settings.setValue("frequency", p.frequency);
    settings.setValue("smooth", p.smooth);
    startPrimitivePlacement(p);
}

bool MainWindow::recordState(const QString& name) {
    if (!doc_.editingComponent() || name.trimmed().isEmpty()) return false;
    viewport_->cancelTool();
    doc_.beginOp("Record State");
    if (!gs::recordState(doc_.state, name.trimmed().toStdString())) {
        doc_.cancelOp();
        return false;
    }
    doc_.commitOp();
    const int n = (int)doc_.state.defs[doc_.state.instances[doc_.state.editing].def].states.size();
    statusLabel_->setText(QStringLiteral("Saved state \"%1\" (%2 in all). Move or rotate the geometry and press F5 for the next one.").arg(name.trimmed()).arg(n));
    return true;
}

void MainWindow::promptRecordState() {
    if (!doc_.editingComponent()) {
        statusLabel_->setText(QStringLiteral("Open a component first (double-click it), then F5 saves its geometry as a state."));
        return;
    }
    Instance& in = doc_.state.instances[doc_.state.editing];
    const ComponentDef& def = doc_.state.defs[in.def];
    const std::string& cls = in.entity.classname;
    // Names that fit: the first state is where it starts.
    QStringList suggestions;
    if (cls == "func_door") suggestions = {QStringLiteral("CLOSED"), QStringLiteral("OPEN")};
    else if (cls == "func_button") suggestions = {QStringLiteral("IDLE"), QStringLiteral("PRESSED")};
    QStringList names;
    for (const AnimState& s : def.states) names << QString::fromStdString(s.name);
    for (const QString& s : suggestions)
        if (!names.contains(s)) names.prepend(s);
    std::stable_sort(names.begin(), names.end(), [&](const QString& a, const QString& b) {
        return findState(def, a.toStdString()) < findState(def, b.toStdString());
    });
    QString suggested = QStringLiteral("STATE%1").arg(def.states.size() + 1);
    for (const QString& s : suggestions)
        if (findState(def, s.toStdString()) < 0) {
            suggested = s;
            break;
        }
    if (!names.contains(suggested)) names.prepend(suggested);
    bool ok = false;
    const QString name = QInputDialog::getItem(this, QStringLiteral("Record State"), QStringLiteral("Name of this state:"), names,
                                               (int)names.indexOf(suggested), true, &ok);
    if (ok) recordState(name);
    viewport_->setFocus();
}

EntityProperties* MainWindow::entityProperties() {
    if (!entityProps_) {
        entityProps_ = new EntityProperties(&doc_, viewport_, this);
        connect(entityProps_, &EntityProperties::recordRequested, this, [this] { promptRecordState(); });
    }
    return entityProps_;
}

void MainWindow::showEntityProperties() {
    // What is selected in the open component, or else the open component.
    Id inst = doc_.selection.instances.size() == 1 ? *doc_.selection.instances.begin() : doc_.state.editing;
    if (inst == kNone) return;
    entityProperties()->setInstance(inst);
    entityProps_->show();
    entityProps_->raise();
}

void MainWindow::addCameraFromView() {
    if (doc_.editingComponent()) {
        statusLabel_->setText(QStringLiteral("Close the open component first."));
        return;
    }
    viewport_->cancelTool();
    doc_.beginOp("Add Camera");
    const Id id = doc_.addObject(viewport_->objectFromView(SceneObject::ViewCamera));
    doc_.selection.clear();
    doc_.selection.objects.insert(id);
    doc_.commitOp();
    doc_.notifySelection();
}

MaterialEditor* MainWindow::materialEditor() {
    if (!materialEditor_) {
        materialEditor_ = new MaterialEditor(&doc_, viewport_, MaterialEditor::InWindow, this);
        connect(materialEditor_, &MaterialEditor::saved, this, [this] {
            if (materialBrowser_ && materialBrowser_->isVisible()) materialBrowser_->rebuildNow();
        });
    }
    return materialEditor_;
}

MaterialBrowser* MainWindow::materialBrowser() {
    if (!materialBrowser_) {
        materialBrowser_ = new MaterialBrowser(&doc_, viewport_, materials_, this);
        connect(materialBrowser_, &MaterialBrowser::chosen, this, [this] {
            materials_->refresh();
            entityInfo_->refresh();
            if (materialEditor_ && materialEditor_->isVisible()) materialEditor_->refresh();
        });
        connect(materialBrowser_, &MaterialBrowser::editRequested, this, [this] {
            materialEditor()->show();
            materialEditor()->raise();
            materialEditor()->refresh();
        });
    }
    return materialBrowser_;
}

PrefabBrowser* MainWindow::prefabBrowser() {
    if (!prefabBrowser_) {
        prefabBrowser_ = new PrefabBrowser(this);
        connect(prefabBrowser_, &PrefabBrowser::placeRequested, this, [this](const QString& p) { startPrefabPlacement(p); });
        connect(prefabBrowser_, &PrefabBrowser::openRequested, this, [this](const QString& p) {
            if (maybeSave()) openFile(p);
        });
    }
    return prefabBrowser_;
}

void MainWindow::makeUnique() {
    if (doc_.selection.instances.empty()) return;
    viewport_->cancelTool();
    doc_.beginOp("Make Unique");
    gs::makeUnique(doc_.state, doc_.selection.instances);
    doc_.commitOp();
    doc_.notifySelection();
}

void MainWindow::explode() {
    if (doc_.selection.instances.empty()) return;
    viewport_->cancelTool();
    doc_.beginOp("Explode");
    Selection out;
    for (Id i : doc_.selection.instances) explodeInstance(doc_.state, i, &out);
    doc_.selection = out;
    doc_.commitOp();
    doc_.validateSelection();
    doc_.notifySelection();
}

void MainWindow::editComponent() {
    if (doc_.selection.instances.size() != 1) return;
    viewport_->cancelTool();
    doc_.openComponent(*doc_.selection.instances.begin());
    viewport_->refreshToolStatus();
}

void MainWindow::traceImage() {
    viewport_->cancelTool();
    // Selected faces that show an image, with the side and mapping to trace.
    struct Target {
        Id face;
        TexMap map;
        Id material;
    };
    std::vector<Target> targets;
    const Mesh& mesh = doc_.mesh();
    for (Id f : doc_.selection.faces) {
        const Face& F = mesh.faces[f];
        for (bool front : {true, false}) {
            Id mid = front ? F.front.material : F.back.material;
            const Material* mat = doc_.material(mid);
            if (!mat || !mat->hasTexture()) continue;
            targets.push_back({f, effectiveTexMap(F, front, mat), mid});
            break;
        }
    }
    if (targets.empty()) {
        statusBar()->showMessage(QStringLiteral("Select a face that shows an image (drop an image file into the scene to make one)."), 6000);
        return;
    }
    std::map<Id, QImage> images;
    bool alpha = false;
    for (const Target& t : targets) {
        if (images.count(t.material)) continue;
        images[t.material] = decodeTexture(doc_.material(t.material)->texture);
        alpha = alpha || hasTransparency(images[t.material]);
    }

    QSettings settings;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Trace Image"));
    auto* fl = new QFormLayout(&dlg);
    auto* source = new QComboBox(&dlg);
    source->addItems({QStringLiteral("Alpha cutout (transparent background)"), QStringLiteral("Dark shapes (black on white)"),
                      QStringLiteral("Light shapes (white on black)")});
    source->setCurrentIndex(alpha ? 0 : 1);
    auto* threshold = new QSpinBox(&dlg);
    threshold->setRange(1, 99);
    threshold->setSuffix(QStringLiteral(" %"));
    threshold->setValue(settings.value("traceImage/threshold", 50).toInt());
    auto* detail = new QDoubleSpinBox(&dlg);
    detail->setRange(0.2, 20);
    detail->setSingleStep(0.25);
    detail->setSuffix(QStringLiteral(" px"));
    detail->setValue(settings.value("traceImage/simplify", 1.0).toDouble());
    detail->setToolTip(QStringLiteral("How far the outline may deviate from the image. Larger values give fewer edges."));
    auto* specks = new QSpinBox(&dlg);
    specks->setRange(0, 100000);
    specks->setSuffix(QStringLiteral(" px²"));
    specks->setValue(settings.value("traceImage/minArea", 16).toInt());
    auto* removeBg = new QCheckBox(QStringLiteral("Remove the background (cut out the shape)"), &dlg);
    removeBg->setChecked(settings.value("traceImage/removeBackground", true).toBool());
    auto* msg = new QLabel(&dlg);
    msg->setWordWrap(true);
    fl->addRow(QStringLiteral("Trace:"), source);
    fl->addRow(QStringLiteral("Threshold:"), threshold);
    fl->addRow(QStringLiteral("Smoothing:"), detail);
    fl->addRow(QStringLiteral("Ignore specks below:"), specks);
    fl->addRow(removeBg);
    fl->addRow(msg);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    fl->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // Per-material pixel values for the current source (recomputed on change).
    std::map<Id, ScalarImage> values;
    int valuesSource = -1;
    bool ok = false;
    doc_.beginOp("Trace Image");
    auto apply = [&] {
        doc_.cancelOp();
        doc_.beginOp("Trace Image");
        if (valuesSource != source->currentIndex()) {
            values.clear();
            for (const auto& [mid, img] : images) values[mid] = scalarImage(img, (TraceSource)source->currentIndex());
            valuesSource = source->currentIndex();
        }
        TraceOptions opt;
        opt.threshold = threshold->value() / 100.0;
        opt.simplify = detail->value();
        opt.minArea = specks->value();
        opt.removeBackground = removeBg->isChecked();
        std::string err;
        int traced = 0;
        for (const Target& t : targets) {
            std::string e;
            if (traceImageOnFace(doc_.mesh(), t.face, t.map, values[t.material], opt, &e)) ++traced;
            else err = e;
        }
        ok = traced > 0;
        msg->setText(ok ? QStringLiteral("%1 faces, %2 edges in the model.").arg(doc_.mesh().aliveFaceCount()).arg(doc_.mesh().aliveEdgeCount())
                        : QString::fromStdString(err));
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
        viewport_->update();
    };
    connect(source, &QComboBox::currentIndexChanged, &dlg, apply);
    connect(threshold, &QSpinBox::valueChanged, &dlg, apply);
    connect(detail, &QDoubleSpinBox::valueChanged, &dlg, apply);
    connect(specks, &QSpinBox::valueChanged, &dlg, apply);
    connect(removeBg, &QCheckBox::toggled, &dlg, apply);
    apply();

    if (dlg.exec() == QDialog::Accepted && ok) {
        settings.setValue("traceImage/threshold", threshold->value());
        settings.setValue("traceImage/simplify", detail->value());
        settings.setValue("traceImage/minArea", specks->value());
        settings.setValue("traceImage/removeBackground", removeBg->isChecked());
        doc_.selection.clear();
        doc_.commitOp();
        doc_.notifySelection();
    } else {
        doc_.cancelOp();
    }
    viewport_->update();
}

void MainWindow::about() {
    QMessageBox::about(this, QStringLiteral("About GNUSketch"),
                       QStringLiteral("<h3>GNUSketch %1</h3><p>A native Linux 3D sketching tool.</p>"
                                      "<p>Models are stored in an open JSON format (.gsketch). "
                                      "Export OBJ (keeps n-gon faces) or glTF 2.0 to convert to other formats such as FBX "
                                      "with Blender or assimp.</p>").arg(QApplication::applicationVersion()));
}

void MainWindow::showShortcuts() {
    QMessageBox::information(this, QStringLiteral("Keyboard Shortcuts"),
                             QStringLiteral("<table>"
                                            "<tr><td><b>Space</b></td><td>Select</td><td><b>E</b></td><td>Eraser</td></tr>"
                                            "<tr><td><b>B</b></td><td>Paint Bucket</td><td><b>L</b></td><td>Line</td></tr>"
                                            "<tr><td><b>R</b></td><td>Rectangle</td><td><b>C</b></td><td>Circle</td></tr>"
                                            "<tr><td><b>G</b></td><td>Polygon</td><td><b>A</b></td><td>2 Point Arc</td></tr>"
                                            "<tr><td><b>P</b></td><td>Push/Pull</td><td><b>M</b></td><td>Move</td></tr>"
                                            "<tr><td><b>Q</b></td><td>Rotate</td><td><b>S</b></td><td>Scale</td></tr>"
                                            "<tr><td><b>F</b></td><td>Offset</td><td><b>T</b></td><td>Tape Measure</td></tr>"
                                            "<tr><td><b>O</b></td><td>Orbit</td><td><b>H</b></td><td>Pan</td></tr>"
                                            "<tr><td><b>Z</b></td><td>Zoom</td><td><b>Shift+Z</b></td><td>Zoom Extents</td></tr>"
                                            "<tr><td><b>Middle mouse</b></td><td>Orbit</td><td><b>Shift+Middle</b></td><td>Pan</td></tr>"
                                            "<tr><td><b>Arrows</b></td><td>Lock axis / plane</td><td><b>Shift</b></td><td>Lock inference</td></tr>"
                                            "<tr><td><b>Ctrl+T</b></td><td>Select none</td><td><b>Esc</b></td><td>Cancel</td></tr>"
                                            "<tr><td><b>Ctrl+F</b></td><td>Repeat last effect</td><td><b>Ctrl+G</b></td><td>Game Mode</td></tr>"
                                            "<tr><td><b>Ctrl+B</b></td><td>Bend</td><td><b>Ctrl+U</b></td><td>Position Texture</td></tr>"
                                            "</table><p>Type numbers at any time to enter exact values in the Measurements box.</p>"));
}

void MainWindow::contextMenu(const QPoint& globalPos, const PickResult& pick) {
    Selection& s = doc_.selection;
    bool picked = pick.face != kNone || pick.edge != kNone || pick.instance != kNone || pick.object != kNone;
    bool inSel = pick.object != kNone ? s.objects.count(pick.object) > 0
                                      : (pick.face != kNone && s.faces.count(pick.face)) || (pick.edge != kNone && s.edges.count(pick.edge)) ||
                                            (pick.instance != kNone && s.instances.count(pick.instance));
    if (picked && !inSel) {
        s.clear();
        if (pick.object != kNone) {
            s.objects.insert(pick.object);
        } else if (pick.instance != kNone) {
            s.instances.insert(pick.instance);
        } else if (pick.edge != kNone) {
            s.edges.insert(pick.edge);
            expandCurves(doc_.mesh(), s.edges);
        } else {
            s.faces.insert(pick.face);
        }
        doc_.notifySelection();
    }
    QMenu menu(this);
    const bool geometry = !s.faces.empty() || !s.edges.empty();
    if (!s.instances.empty()) {
        if (s.instances.size() == 1) {
            const ComponentDef& d = doc_.state.defs[doc_.state.instances[*s.instances.begin()].def];
            menu.addSection(QStringLiteral("Component: %1").arg(QString::fromStdString(d.name)));
            menu.addAction(acts_["editcomponent"]);
            menu.addAction(acts_["entityprops"]);
        }
        menu.addAction(acts_["saveprefab"]);
        menu.addAction(acts_["makeunique"]);
        menu.addAction(acts_["explode"]);
        QMenu* solids = menu.addMenu(Icons::get("solid_union"), QStringLiteral("Solid Tools"));
        for (ToolId id : kSolidTools) solids->addAction(toolActions_[id]);
        menu.addSeparator();
    }
    if (s.objects.size() == 1) {
        const Id id = *s.objects.begin();
        const SceneObject& o = doc_.state.objects[id];
        menu.addSection(QStringLiteral("%1: %2").arg(QString::fromLatin1(sceneObjectTypeName(o.kind)), QString::fromStdString(o.name)));
        if (o.kind == SceneObject::ViewCamera)
            menu.addAction(QStringLiteral("Look Through"), this, [this, id] { viewport_->lookThrough(id); });
        menu.addSeparator();
    }
    if (doc_.editingComponent()) {
        menu.addAction(acts_["entityprops"]);
        menu.addAction(acts_["recordstate"]);
        menu.addAction(acts_["closecomponent"]);
        menu.addSeparator();
    }
    if (!s.empty()) {
        if (geometry || !s.instances.empty()) {
            menu.addAction(acts_["makecomponent"]);
            if (geometry) menu.addAction(acts_["saveprefab"]);
            menu.addSeparator();
        }
        menu.addAction(acts_["delete"]);
        menu.addAction(acts_["hide"]);
        if (geometry) menu.addAction(acts_["roundcorners"]);
        if (!s.faces.empty()) menu.addAction(acts_["traceimage"]);
        menu.addSeparator();
    }
    if (geometry) {
        QAction* conn = menu.addAction(QStringLiteral("Select All Connected"));
        connect(conn, &QAction::triggered, this, [this] {
            selectConnected(doc_.mesh(), doc_.selection);
            doc_.notifySelection();
        });
        if (!s.faces.empty()) {
            QAction* bound = menu.addAction(QStringLiteral("Select Bounding Edges"));
            connect(bound, &QAction::triggered, this, [this] {
                for (Id f : std::set<Id>(doc_.selection.faces))
                    for (Id e : doc_.mesh().faceEdges(f)) doc_.selection.edges.insert(e);
                doc_.notifySelection();
            });
            menu.addSeparator();
            menu.addAction(acts_["reverse"]);
            QMenu* tex = menu.addMenu(QStringLiteral("Texture"));
            tex->addAction(toolActions_[ToolId::TexturePosition]);
            tex->addAction(acts_["texfit"]);
            tex->addAction(acts_["texreset"]);
        }
        menu.addAction(acts_["soften"]);
        menu.addAction(acts_["unsoften"]);
        menu.addSeparator();
    }
    if (!s.empty()) {
        QAction* zs = menu.addAction(QStringLiteral("Zoom Selection"));
        connect(zs, &QAction::triggered, this, [this] {
            std::set<Id> vs = selectionVertices(doc_.mesh(), doc_.selection);
            Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
            bool any = false;
            auto grow = [&](const Vec3& p) {
                for (int i = 0; i < 3; ++i) {
                    mn[i] = std::min(mn[i], p[i]);
                    mx[i] = std::max(mx[i], p[i]);
                }
                any = true;
            };
            for (Id v : vs) grow(doc_.mesh().pos(v));
            for (Id i : doc_.selection.instances) {
                Vec3 a, b;
                if (!instanceBounds(doc_.state, i, a, b)) continue;
                grow(a);
                grow(b);
            }
            for (Id o : doc_.selection.objects) grow(doc_.state.objects[o].pos);
            if (!any) return;
            viewport_->camera().zoomExtents(mn, mx);
            viewport_->update();
        });
        menu.addSeparator();
    }
    menu.addAction(acts_["unhide"]);
    menu.addAction(acts_["selectall"]);
    menu.addAction(acts_["zoomext"]);
    menu.exec(globalPos);
}

// ---------------------------------------------------------------------------
// Window events
// ---------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls())
        for (const QUrl& u : e->mimeData()->urls()) {
            QString p = u.toLocalFile();
            if (p.endsWith(".gsketch", Qt::CaseInsensitive) || p.endsWith(".obj", Qt::CaseInsensitive) || isImageFile(p) ||
                Plugins::forFile(p, false)) {
                e->acceptProposedAction();
                return;
            }
        }
    e->ignore();
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const QUrl& u : e->mimeData()->urls()) {
        QString p = u.toLocalFile();
        if (p.endsWith(".gsketch", Qt::CaseInsensitive)) {
            if (maybeSave()) openFile(p);
            break;
        }
        if (p.endsWith(".obj", Qt::CaseInsensitive)) {
            doc_.beginOp("Import");
            QString err;
            if (gs::importObj(doc_, p, true, 1.0, &err) < 0) doc_.cancelOp();
            else doc_.commitOp();
            viewport_->zoomExtents();
            break;
        }
        if (FormatPlugin* f = Plugins::forFile(p, false)) {   // (into what is open, as an OBJ is)
            importWithPlugin(f, p, false);
            break;
        }
        if (isImageFile(p)) materials_->addImageMaterial(p);
    }
    e->acceptProposedAction();
}

} // namespace gs
