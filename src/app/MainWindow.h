// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - main application window.
#pragma once

#include <functional>

#include "Inference.h"
#include "Tool.h"
#include "core/Document.h"

#include <QJsonObject>
#include <QMainWindow>

#include <map>
#include <memory>
#include <optional>

class QAction;
class QActionGroup;
class QLabel;
class QMimeData;
class QLineEdit;
class QDockWidget;
class QProcess;
class QTemporaryDir;

namespace forma {

class FormatPlugin;
struct PrimitiveParams;
struct PartChoice;
struct GearRef;
enum class PartFamily;

class Viewport;
class EntityInfoPanel;
class MaterialsPanel;
class StylesPanel;
class InstructorPanel;
class TerrainPanel;
class SunPanel;
class ScenePanel;
class EntityProperties;
class HierarchyPanel;
class MaterialEditor;
class MaterialBrowser;
class PrefabBrowser;
struct EffectDef;
class PropertiesPanel;
class PlacementSettings;
class AnimatorPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;
    bool openFile(const QString& path);
    // Writes a prefab file of the instance's component; returns its path, or
    // empty with a message in the status bar.
    QString savePrefabOf(Id instance, const QString& name);
    // Attaches a prefab to the cursor; the next click places it.
    bool startPrefabPlacement(const QString& path);
    // Builds a primitive as a new component and attaches it to the cursor.
    bool startPrimitivePlacement(const PrimitiveParams& params);
    // Builds a standard part (screw, nut, washer, gear, rack) as a new
    // component and attaches it to the cursor.
    bool startPartPlacement(const PartChoice& part);
    // Records the open component's geometry as a state. False if none is open.
    bool recordState(const QString& name);
    EntityProperties* entityProperties();
    MaterialBrowser* materialBrowser();
    PrefabBrowser* prefabBrowser();
    MaterialEditor* materialEditor();

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void buildActions();
    void buildMenus();
    void buildToolbars();
    void buildTray();
    void buildStatusBar();
    QAction* toolAction(ToolId id, const QString& text, const QString& shortcut);
    void onDocChanged();
    void onSelectionChanged();
    void updateTitle();
    void updateUndoActions();
    void syncStyleActions();

    bool maybeSave();
    void newFile();
    void open();
    bool save();
    bool saveAs();
    void importObj();
    void exportObj();
    void exportGltf();
    void exportStl();
    // 3D printing exports (io/PrintExport.h); asks for the unit, defaulting to the model's.
    void exportForPrint(int format);
    // 3D printing files (io/PrintImport.h): asks for the unit when the file has none.
    void importForPrint(const QString& ext);
    bool importPrintFile(const QString& path, bool intoEmpty);
    void showSettings();
public:
    // Open Recent: most recent first, missing files dropped.
    QStringList recentFiles() const;
    void addRecentFile(const QString& path);
    void clearRecentFiles();
private:
    void rebuildRecentMenu();
    class QMenu* recentMenu_ = nullptr;
    // file format plugins (Plugins.h): File > Import / Export entries, Help > Plugins
    bool importWithPlugin(FormatPlugin* f, const QString& path, bool intoEmpty);
    void importPlugin(FormatPlugin* f);
    void exportPlugin(FormatPlugin* f);
    void showPlugins();
public:
    // File > Export Selection: runs an export (a File > Export entry, or
    // saving as .forma when empty) on a model holding only the selection.
    // Returns false when nothing is selected.
    bool exportSelection(const std::function<void()>& exportFn = {});
    // What the exports write: the document, or the selection while
    // exportSelection runs one.
    const Document& exportDoc() const { return exportSource_ ? *exportSource_ : doc_; }
private:
    QString exportTitle(const QString& format) const;
    const Document* exportSource_ = nullptr;
    class QMenu* exportSelMenu_ = nullptr;

    void copySelection(bool cut);
    void paste();
    void deleteSelection();
    void selectAll();
    void hideSelection();
    void unhideAll();
    void reverseFaces();
    void softenSelection(bool soft);
    void textureFitSelection(bool fit);
    void deleteGuides();
    void modelInfo();
    void repairGeometry();
    void roundCorners();
    void insertPrimitive();
    void insertPart(PartFamily family);
    // The model's gears (selected placements first) that a new gear can fit.
    std::vector<GearRef> gearsInModel() const;
    // A new component of the mesh, following the cursor; one undo step.
    // A new component of the mesh on the cursor (or, with `at`, put right
    // there and selected); `extra` is kept with its definition.
    bool startMeshPlacement(Mesh mesh, const std::string& name, const std::string& extra = {}, const Mat4* at = nullptr);
    // The new instance (inside an open operation) follows the cursor until a
    // click places it; commits or cancels the operation.
    bool attachToCursor(Id inst);
    void makeComponent();
    void saveAsPrefab();
    void promptRecordState();
    void showEntityProperties();
    void addCameraFromView();
    void makeUnique();
    void explode();
    void editComponent();
    void traceImage();
    void runEffect(const EffectDef& def, bool repeat);
    void launchGame(const Vec3& feet, double yaw, bool native = false);
    void about();
    void showShortcuts();
    void contextMenu(const QPoint& globalPos, const PickResult& pick);
    QJsonObject viewState() const;
    void applyViewState(const QJsonObject& o);

    Document doc_;
    Viewport* viewport_ = nullptr;
    EntityInfoPanel* entityInfo_ = nullptr;
    MaterialsPanel* materials_ = nullptr;
    StylesPanel* styles_ = nullptr;
    InstructorPanel* instructor_ = nullptr;
    TerrainPanel* terrain_ = nullptr;
    SunPanel* sun_ = nullptr;
    ScenePanel* scene_ = nullptr;
    EntityProperties* entityProps_ = nullptr;
    HierarchyPanel* hierarchy_ = nullptr;
    QDockWidget* hierarchyDock_ = nullptr;
    QAction* sceneAct_ = nullptr;      // Scene toolbar: shows the Properties panel
    PropertiesPanel* properties_ = nullptr;
    PlacementSettings* placement_ = nullptr;
    AnimatorPanel* animator_ = nullptr;
    QDockWidget *propertiesDock_ = nullptr, *materialsDock_ = nullptr, *instructorDock_ = nullptr, *environmentDock_ = nullptr,
                *animatorDock_ = nullptr;
    void applyUiSizes(); // icon size from Window > Settings
    MaterialEditor* materialEditor_ = nullptr;
    MaterialBrowser* materialBrowser_ = nullptr;
    PrefabBrowser* prefabBrowser_ = nullptr;
    const EffectDef* lastEffect_ = nullptr;
    QProcess* game_ = nullptr;
    std::unique_ptr<QTemporaryDir> gameDir_;

    QLabel* statusLabel_ = nullptr;
    QLabel* vcbLabel_ = nullptr;
    QLineEdit* vcbEdit_ = nullptr;

    std::map<ToolId, QAction*> toolActions_;
    QActionGroup* toolGroup_ = nullptr;
    QAction *undoAct_ = nullptr, *redoAct_ = nullptr;
    QAction *perspectiveAct_ = nullptr, *parallelAct_ = nullptr;
    QAction *xrayAct_ = nullptr, *backEdgesAct_ = nullptr, *axesAct_ = nullptr, *guidesAct_ = nullptr,
            *hiddenAct_ = nullptr, *edgesAct_ = nullptr, *profilesAct_ = nullptr, *shadowsAct_ = nullptr, *cinematicAct_ = nullptr, *aoAct_ = nullptr,
            *antialiasAct_ = nullptr, *sunAct_ = nullptr, *cullAct_ = nullptr, *objectsAct_ = nullptr, *solidsAct_ = nullptr;
    std::map<int, QAction*> faceStyleActs_;
    std::map<QString, QAction*> acts_;

    // Pastes images from the clipboard (image data or image files) as planes.
    bool pasteImages(const QMimeData* md);
};

} // namespace forma
