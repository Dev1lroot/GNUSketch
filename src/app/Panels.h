// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - "Default Tray" panels: Entity Info, Materials, Styles, Instructor.
#pragma once

#include "Tool.h"
#include "core/Document.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QToolButton;
class QListWidget;
class QListWidgetItem;
class QComboBox;
class QSlider;
class QStackedWidget;
class QTextBrowser;
class QDoubleSpinBox;
class QVBoxLayout;
class QButtonGroup;
class QTimer;

namespace gs {

class Viewport;
class MaterialEditor;

// Collapsible section with a SketchUp-tray-like header.
class TraySection : public QWidget {
    Q_OBJECT
public:
    TraySection(const QString& title, QWidget* content, QWidget* parent = nullptr);
    void setExpanded(bool on);
    bool expanded() const;
    QWidget* content() const { return content_; }

private:
    QToolButton* header_;
    QWidget* content_;
};

class Tray : public QWidget {
    Q_OBJECT
public:
    explicit Tray(QWidget* parent = nullptr);
    TraySection* addSection(const QString& title, QWidget* content, bool expanded);

private:
    QVBoxLayout* layout_;
};

class EntityInfoPanel : public QWidget {
    Q_OBJECT
public:
    EntityInfoPanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();

private:
    void applyEdgeFlags();
    Document* doc_;
    Viewport* vp_;
    QLabel* title_;
    QLabel* swatch_;
    QLabel* matName_;
    QLabel* measureLabel_;
    QLabel* measure_;
    QCheckBox* soft_;
    QCheckBox* smooth_;
    QCheckBox* hidden_;
    QPushButton* reverse_;
    QWidget* edgeBox_;
    QWidget* faceBox_;
    bool updating_ = false;
};

class MaterialsPanel : public QWidget {
    Q_OBJECT
public:
    MaterialsPanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();
    // Adds a texture material from an image file. Returns its id or kNone.
    Id addImageMaterial(const QString& path);
    // Makes a new PBR material the current one and opens it for editing.
    Id createMaterial();
    MaterialEditor* editor() const { return editor_; }
    QListWidget* list() const { return list_; }
    void chooseRow(int row);
    void showLibrary(int index);
    // The categories of the Select page.
    enum Library { InModel, Architectural, Colors, Textures };

    const std::vector<Material>& colorLibrary() const { return colorLib_; }
    const std::vector<Material>& textureLibrary() const { return textureLib_; }

signals:
    void materialChosen();
    void browserRequested();
    void editorRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuildList();
    void chooseItem(QListWidgetItem* item);
    void showTab(int page);
    void fillGlobes();
    QPixmap swatch(const Material* m, int size) const;
    Id ensureInModel(const Material& m);

    Document* doc_;
    Viewport* vp_;
    QLabel* preview_;
    QLineEdit* name_;
    QComboBox* library_;
    QListWidget* list_;
    QStackedWidget* pages_;
    QToolButton* selectTab_;
    QToolButton* editTab_;
    MaterialEditor* editor_;
    QTimer* globeTimer_;
    std::vector<Material> colorLib_, textureLib_;
    uint64_t listRevision_ = 0;
    bool updating_ = false;
};

// Direction and strength of the sunlight.
class SunPanel : public QWidget {
    Q_OBJECT
public:
    SunPanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();

signals:
    void styleChanged();

private:
    void apply();
    Document* doc_;
    Viewport* vp_;
    QCheckBox* sun_;
    QCheckBox* shadows_;
    QCheckBox* cinematic_;
    QDoubleSpinBox* rays_;
    QDoubleSpinBox* azimuth_;
    QDoubleSpinBox* altitude_;
    QDoubleSpinBox* intensity_;
    bool updating_ = false;
};

class StylesPanel : public QWidget {
    Q_OBJECT
public:
    StylesPanel(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void refresh();

signals:
    void styleChanged();

private:
    void apply();
    Document* doc_;
    Viewport* vp_;
    QButtonGroup* faceGroup_;
    QToolButton* xray_;
    QCheckBox* edges_;
    QCheckBox* backEdges_;
    QCheckBox* profiles_;
    QDoubleSpinBox* profileW_;
    QCheckBox* sky_;
    QCheckBox* ground_;
    QCheckBox* axes_;
    QCheckBox* guides_;
    QCheckBox* hiddenGeom_;
    QPushButton* skyColor_;
    QPushButton* groundColor_;
    QPushButton* bgColor_;
    QPushButton* frontColor_;
    QPushButton* backColor_;
    bool updating_ = false;
};

// Brush settings of the Terraform tools (Mountain, Pit, Flatten).
class TerrainPanel : public QWidget {
    Q_OBJECT
public:
    TerrainPanel(Document* doc, Viewport* vp, const std::vector<QAction*>& tools, QWidget* parent = nullptr);
    void refresh();

private:
    void apply();
    Document* doc_;
    Viewport* vp_;
    QLineEdit* radius_;
    QDoubleSpinBox* force_;
    QSlider* hardness_;
    QLabel* hardnessValue_;
    QDoubleSpinBox* density_;
    bool updating_ = false;
};

class InstructorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InstructorPanel(QWidget* parent = nullptr);
    void showTool(ToolId id);

private:
    QTextBrowser* text_;
};

} // namespace gs
