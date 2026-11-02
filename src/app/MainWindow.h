// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - main application window.
#pragma once

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

namespace gs {

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
class TraySection;

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
    void roundCorners();
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
    MaterialEditor* materialEditor_ = nullptr;
    MaterialBrowser* materialBrowser_ = nullptr;
    PrefabBrowser* prefabBrowser_ = nullptr;
    const EffectDef* lastEffect_ = nullptr;
    QProcess* game_ = nullptr;
    std::unique_ptr<QTemporaryDir> gameDir_;
    QDockWidget* trayDock_ = nullptr;
    std::map<QString, TraySection*> sections_;

    QLabel* statusLabel_ = nullptr;
    QLabel* vcbLabel_ = nullptr;
    QLineEdit* vcbEdit_ = nullptr;

    std::map<ToolId, QAction*> toolActions_;
    QActionGroup* toolGroup_ = nullptr;
    QAction *undoAct_ = nullptr, *redoAct_ = nullptr;
    QAction *perspectiveAct_ = nullptr, *parallelAct_ = nullptr;
    QAction *xrayAct_ = nullptr, *backEdgesAct_ = nullptr, *axesAct_ = nullptr, *guidesAct_ = nullptr,
            *hiddenAct_ = nullptr, *edgesAct_ = nullptr, *profilesAct_ = nullptr, *shadowsAct_ = nullptr, *aoAct_ = nullptr,
            *antialiasAct_ = nullptr, *sunAct_ = nullptr, *cullAct_ = nullptr, *objectsAct_ = nullptr;
    std::map<int, QAction*> faceStyleActs_;
    std::map<QString, QAction*> acts_;

    // Pastes images from the clipboard (image data or image files) as planes.
    bool pasteImages(const QMimeData* md);
};

} // namespace gs
