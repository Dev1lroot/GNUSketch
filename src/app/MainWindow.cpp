// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainWindow.h"

#include "Icons.h"
#include "ModifyTools.h"
#include "Panels.h"
#include "Viewport.h"
#include "core/RoundCorners.h"
#include "io/GltfExport.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "io/ObjIO.h"
#include "io/StlExport.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

namespace gs {

namespace {

QJsonArray vec(const Vec3& v) { return QJsonArray{v.x, v.y, v.z}; }
Vec3 vec(const QJsonValue& j, const Vec3& def) {
    QJsonArray a = j.toArray();
    return a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : def;
}
QJsonArray col(const Color& c) { return QJsonArray{c.r, c.g, c.b, c.a}; }
Color col(const QJsonValue& j, const Color& def) {
    QJsonArray a = j.toArray();
    return a.size() == 4 ? Color((float)a[0].toDouble(), (float)a[1].toDouble(), (float)a[2].toDouble(), (float)a[3].toDouble()) : def;
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
    toolAction(ToolId::Tape, QStringLiteral("Tape Measure"), QStringLiteral("T"));
    toolAction(ToolId::Orbit, QStringLiteral("Orbit"), QStringLiteral("O"));
    toolAction(ToolId::Pan, QStringLiteral("Pan"), QStringLiteral("H"));
    toolAction(ToolId::Zoom, QStringLiteral("Zoom"), QStringLiteral("Z"));
    toolAction(ToolId::ZoomWindow, QStringLiteral("Zoom Window"), QStringLiteral("Ctrl+Shift+W"));

    auto act = [this](const QString& key, const QString& icon, const QString& text, const QKeySequence& sc,
                      std::function<void()> fn) {
        auto* a = new QAction(icon.isEmpty() ? QIcon() : Icons::get(icon), text, this);
        if (!sc.isEmpty()) a->setShortcut(sc);
        a->setToolTip(sc.isEmpty() ? text : QStringLiteral("%1 (%2)").arg(text, sc.toString(QKeySequence::NativeText)));
        connect(a, &QAction::triggered, this, std::move(fn));
        acts_[key] = a;
        return a;
    };
    act("new", "new", QStringLiteral("&New"), QKeySequence::New, [this] { newFile(); });
    act("open", "open", QStringLiteral("&Open…"), QKeySequence::Open, [this] { open(); });
    act("save", "save", QStringLiteral("&Save"), QKeySequence::Save, [this] { save(); });
    act("saveas", "", QStringLiteral("Save &As…"), QKeySequence::SaveAs, [this] { saveAs(); });
    act("importobj", "import", QStringLiteral("Import OBJ…"), QKeySequence(), [this] { importObj(); });
    act("exportobj", "export", QStringLiteral("Export OBJ (n-gons)…"), QKeySequence(), [this] { exportObj(); });
    act("roundcorners", "roundcorners", QStringLiteral("&Round Corners…"), QKeySequence(), [this] { roundCorners(); })
        ->setStatusTip(QStringLiteral("Round the selected edges or corners with a radius and number of segments."));
    act("exportgltf", "", QStringLiteral("Export glTF 2.0…"), QKeySequence(), [this] { exportGltf(); });
    act("exportstl", "", QStringLiteral("Export STL…"), QKeySequence(), [this] { exportStl(); });
    act("quit", "", QStringLiteral("E&xit"), QKeySequence::Quit, [this] { close(); });

    undoAct_ = act("undo", "undo", QStringLiteral("&Undo"), QKeySequence::Undo, [this] {
        viewport_->cancelTool();
        doc_.undo();
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
    hiddenAct_ = styleToggle("hiddengeom", "", QStringLiteral("&Hidden Geometry"), &RenderStyle::showHidden);

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
    file->addAction(acts_["save"]);
    file->addAction(acts_["saveas"]);
    file->addSeparator();
    file->addAction(acts_["importobj"]);
    QMenu* exp = file->addMenu(QStringLiteral("&Export"));
    exp->addAction(acts_["exportobj"]);
    exp->addAction(acts_["exportgltf"]);
    exp->addAction(acts_["exportstl"]);
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
    edit->addAction(acts_["reverse"]);
    edit->addAction(acts_["soften"]);
    edit->addAction(acts_["unsoften"]);
    edit->addAction(acts_["texfit"]);
    edit->addAction(acts_["texreset"]);

    QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(axesAct_);
    view->addAction(guidesAct_);
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

    QMenu* draw = menuBar()->addMenu(QStringLiteral("&Draw"));
    QMenu* lines = draw->addMenu(QStringLiteral("&Lines"));
    lines->addAction(toolActions_[ToolId::Line]);
    lines->addAction(toolActions_[ToolId::Freehand]);
    QMenu* arcs = draw->addMenu(QStringLiteral("&Arcs"));
    arcs->addAction(toolActions_[ToolId::Arc]);
    QMenu* shapes = draw->addMenu(QStringLiteral("&Shapes"));
    shapes->addAction(toolActions_[ToolId::Rectangle]);
    shapes->addAction(toolActions_[ToolId::Circle]);
    shapes->addAction(toolActions_[ToolId::Polygon]);

    QMenu* tools = menuBar()->addMenu(QStringLiteral("&Tools"));
    for (ToolId id : {ToolId::Select, ToolId::Eraser, ToolId::Paint}) tools->addAction(toolActions_[id]);
    tools->addSeparator();
    for (ToolId id : {ToolId::Move, ToolId::Rotate, ToolId::Scale, ToolId::PushPull, ToolId::Offset}) tools->addAction(toolActions_[id]);
    tools->addSeparator();
    tools->addAction(toolActions_[ToolId::Tape]);
    tools->addSeparator();
    tools->addAction(acts_["roundcorners"]);

    QMenu* win = menuBar()->addMenu(QStringLiteral("&Window"));
    win->addAction(acts_["modelinfo"]);
    QAction* trayAct = win->addAction(QStringLiteral("Default &Tray"));
    trayAct->setCheckable(true);
    trayAct->setChecked(true);
    connect(trayAct, &QAction::toggled, this, [this](bool on) { trayDock_->setVisible(on); });
    connect(win, &QMenu::aboutToShow, this, [this, trayAct] { trayAct->setChecked(trayDock_->isVisible()); });
    for (const char* name : {"Entity Info", "Materials", "Styles", "Instructor"}) {
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
    // -4 = round corners.
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
        {(int)ToolId::Scale, -4},
        {-2, -2},
        {(int)ToolId::Orbit, (int)ToolId::Pan},
        {(int)ToolId::Zoom, (int)ToolId::ZoomWindow},
        {-3, -1},
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
    instructor_ = new InstructorPanel(tray);
    sections_["Entity Info"] = tray->addSection(QStringLiteral("Entity Info"), entityInfo_, true);
    sections_["Materials"] = tray->addSection(QStringLiteral("Materials"), materials_, true);
    sections_["Styles"] = tray->addSection(QStringLiteral("Styles"), styles_, false);
    sections_["Instructor"] = tray->addSection(QStringLiteral("Instructor"), instructor_, false);
    scroll->setWidget(tray);
    trayDock_->setWidget(scroll);
    trayDock_->setMinimumWidth(290);
    addDockWidget(Qt::RightDockWidgetArea, trayDock_);
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
    viewport_->update();
}

void MainWindow::onSelectionChanged() {
    entityInfo_->refresh();
    bool any = !doc_.selection.empty();
    for (const char* k : {"cut", "copy", "delete", "hide", "roundcorners"}) acts_[k]->setEnabled(any);
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
    for (QAction* a : {xrayAct_, backEdgesAct_, edgesAct_, profilesAct_, axesAct_, guidesAct_, hiddenAct_}) a->blockSignals(true);
    xrayAct_->setChecked(s.xray);
    backEdgesAct_->setChecked(s.backEdges);
    edgesAct_->setChecked(s.edges);
    profilesAct_->setChecked(s.profiles);
    axesAct_->setChecked(s.showAxes);
    guidesAct_->setChecked(s.showGuides);
    hiddenAct_->setChecked(s.showHidden);
    for (QAction* a : {xrayAct_, backEdgesAct_, edgesAct_, profilesAct_, axesAct_, guidesAct_, hiddenAct_}) a->blockSignals(false);
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
                                                QStringLiteral("GNUSketch Model (*.gsketch);;Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return;
    s.setValue("files/lastDir", QFileInfo(path).absolutePath());
    openFile(path);
}

bool MainWindow::openFile(const QString& path) {
    viewport_->cancelTool();
    QString err;
    if (path.endsWith(".obj", Qt::CaseInsensitive)) {
        doc_.reset();
        if (gs::importObj(doc_, path, true, 1.0, &err) < 0) {
            QMessageBox::warning(this, QStringLiteral("Open"), err);
            return false;
        }
        doc_.modified = true;
        viewport_->zoomExtents();
        onDocChanged();
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

void MainWindow::exportStl() {
    QSettings s;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export STL"), s.value("files/lastDir").toString(),
                                                QStringLiteral("STL (*.stl)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".stl", Qt::CaseInsensitive)) path += ".stl";
    QString err;
    if (!gs::exportStl(doc_, path, 1000.0, &err)) QMessageBox::warning(this, QStringLiteral("Export"), err);
    else statusLabel_->setText(QStringLiteral("Exported %1 (millimeters)").arg(path));
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
    const RenderStyle& s = doc_.style;
    QJsonObject st;
    st["faceStyle"] = (int)s.faceStyle;
    st["xray"] = s.xray;
    st["edges"] = s.edges;
    st["backEdges"] = s.backEdges;
    st["profiles"] = s.profiles;
    st["profileWidth"] = s.profileWidth;
    st["axes"] = s.showAxes;
    st["guides"] = s.showGuides;
    st["sky"] = s.sky;
    st["ground"] = s.ground;
    st["skyColor"] = col(s.skyColor);
    st["groundColor"] = col(s.groundColor);
    st["background"] = col(s.background);
    st["frontColor"] = col(s.frontColor);
    st["backColor"] = col(s.backColor);
    o["style"] = st;
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
    RenderStyle& s = doc_.style;
    if (o.contains("style")) {
        QJsonObject st = o["style"].toObject();
        s.faceStyle = (RenderStyle::FaceStyle)std::clamp(st["faceStyle"].toInt((int)s.faceStyle), 0, 4);
        s.xray = st["xray"].toBool(s.xray);
        s.edges = st["edges"].toBool(s.edges);
        s.backEdges = st["backEdges"].toBool(s.backEdges);
        s.profiles = st["profiles"].toBool(s.profiles);
        s.profileWidth = (float)st["profileWidth"].toDouble(s.profileWidth);
        s.showAxes = st["axes"].toBool(s.showAxes);
        s.showGuides = st["guides"].toBool(s.showGuides);
        s.sky = st["sky"].toBool(s.sky);
        s.ground = st["ground"].toBool(s.ground);
        s.skyColor = col(st["skyColor"], s.skyColor);
        s.groundColor = col(st["groundColor"], s.groundColor);
        s.background = col(st["background"], s.background);
        s.frontColor = col(st["frontColor"], s.frontColor);
        s.backColor = col(st["backColor"], s.backColor);
    }
    if (o.contains("units")) doc_.units.unit = (LengthUnit)std::clamp(o["units"].toInt(), 0, 4);
    if (o.contains("precision")) doc_.units.precision = std::clamp(o["precision"].toInt(), 0, 6);
    viewport_->update();
}

// ---------------------------------------------------------------------------
// Editing commands
// ---------------------------------------------------------------------------

void MainWindow::copySelection(bool cut) {
    if (doc_.selection.empty()) return;
    Clip clip;
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
    clip.materials = doc_.state.materials;
    clipboard_ = std::move(clip);
    if (cut) deleteSelection();
    statusLabel_->setText(cut ? QStringLiteral("Cut %1 entities").arg(clipboard_->mesh.faces.size() + clipboard_->mesh.edges.size())
                              : QStringLiteral("Copied %1 entities").arg(clipboard_->mesh.faces.size() + clipboard_->mesh.edges.size()));
}

void MainWindow::paste() {
    if (!clipboard_) return;
    viewport_->cancelTool();
    const Clip& c = *clipboard_;
    Mesh& m = doc_.mesh();
    doc_.beginOp("Paste");
    // Map clipboard materials onto document materials (by name + image).
    std::vector<Id> matMap(c.materials.size(), kNone);
    for (size_t i = 0; i < c.materials.size(); ++i) {
        const Material& cm = c.materials[i];
        for (Id j = 0; j < (Id)doc_.state.materials.size(); ++j)
            if (doc_.state.materials[j].name == cm.name && doc_.state.materials[j].texture.data == cm.texture.data) matMap[i] = j;
    }
    auto mapMat = [&](Id id) -> Id {
        if (id < 0 || id >= (Id)matMap.size()) return kNone;
        if (matMap[id] == kNone) matMap[id] = doc_.addMaterial(c.materials[id]);
        return matMap[id];
    };
    std::vector<Id> vmap(c.mesh.vertices.size());
    std::set<Id> newVerts;
    for (size_t i = 0; i < c.mesh.vertices.size(); ++i) {
        vmap[i] = m.addVertexRaw(c.mesh.vertices[i].p);
        newVerts.insert(vmap[i]);
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
    m.weld(newVerts);
    doc_.selection = sel;
    doc_.commitOp();
    doc_.validateSelection();
    doc_.notifySelection();
    viewport_->setTool(ToolId::Move);
    statusLabel_->setText(QStringLiteral("Pasted in place. Pick a base point to move the pasted geometry."));
}

void MainWindow::deleteSelection() {
    if (doc_.selection.empty()) return;
    viewport_->cancelTool();
    Mesh& m = doc_.mesh();
    doc_.beginOp("Erase");
    for (Id f : doc_.selection.faces) m.eraseFace(f);
    for (Id e : doc_.selection.edges) m.eraseEdge(e, false);
    m.removeOrphanVertices();
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
    doc_.notifySelection();
}

void MainWindow::hideSelection() {
    if (doc_.selection.empty()) return;
    doc_.beginOp("Hide");
    for (Id f : doc_.selection.faces) doc_.mesh().faces[f].hidden = true;
    for (Id e : doc_.selection.edges) doc_.mesh().edges[e].hidden = true;
    doc_.mesh().touch();
    doc_.selection.clear();
    doc_.commitOp();
    doc_.notifySelection();
}

void MainWindow::unhideAll() {
    doc_.beginOp("Unhide All");
    for (Face& f : doc_.mesh().faces) f.hidden = false;
    for (Edge& e : doc_.mesh().edges) e.hidden = false;
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
                                            "</table><p>Type numbers at any time to enter exact values in the Measurements box.</p>"));
}

void MainWindow::contextMenu(const QPoint& globalPos, const PickResult& pick) {
    Selection& s = doc_.selection;
    bool picked = pick.face != kNone || pick.edge != kNone;
    bool inSel = (pick.face != kNone && s.faces.count(pick.face)) || (pick.edge != kNone && s.edges.count(pick.edge));
    if (picked && !inSel) {
        s.clear();
        if (pick.edge != kNone) {
            s.edges.insert(pick.edge);
            expandCurves(doc_.mesh(), s.edges);
        } else {
            s.faces.insert(pick.face);
        }
        doc_.notifySelection();
    }
    QMenu menu(this);
    if (!s.empty()) {
        menu.addAction(acts_["delete"]);
        menu.addAction(acts_["hide"]);
        menu.addAction(acts_["roundcorners"]);
        menu.addSeparator();
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
            tex->addAction(acts_["texfit"]);
            tex->addAction(acts_["texreset"]);
        }
        menu.addAction(acts_["soften"]);
        menu.addAction(acts_["unsoften"]);
        menu.addSeparator();
        QAction* zs = menu.addAction(QStringLiteral("Zoom Selection"));
        connect(zs, &QAction::triggered, this, [this] {
            std::set<Id> vs = selectionVertices(doc_.mesh(), doc_.selection);
            if (vs.empty()) return;
            Vec3 mn(1e300, 1e300, 1e300), mx(-1e300, -1e300, -1e300);
            for (Id v : vs)
                for (int i = 0; i < 3; ++i) {
                    mn[i] = std::min(mn[i], doc_.mesh().pos(v)[i]);
                    mx[i] = std::max(mx[i], doc_.mesh().pos(v)[i]);
                }
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
            if (p.endsWith(".gsketch", Qt::CaseInsensitive) || p.endsWith(".obj", Qt::CaseInsensitive) || isImageFile(p)) {
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
        if (isImageFile(p)) materials_->addImageMaterial(p);
    }
    e->acceptProposedAction();
}

} // namespace gs
