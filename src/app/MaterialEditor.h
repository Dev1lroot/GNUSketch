// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - window for editing one material: PBR maps and their strengths,
// and custom shader code.
#pragma once

#include "core/Document.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QToolButton;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace gs {

class Viewport;

class MaterialEditor : public QWidget {
    Q_OBJECT
public:
    // InSidebar: a narrow page of the Materials panel. InWindow: a window of its own.
    enum Mode { InSidebar, InWindow };
    MaterialEditor(Document* doc, Viewport* vp, Mode mode, QWidget* parent = nullptr);
    // Where the live preview goes when the editor has none of its own.
    void setPreviewLabel(QLabel* label) { preview_ = label; }
    // Edits doc->currentMaterial.
    void refresh();
    // Puts images into the maps their file names suggest. Returns how many went in.
    int addMaps(const QStringList& paths);
    // Sets one map from a file.
    bool loadMap(int slot, const QString& path); // slot MapSlotCount = base colour
    // Compiles the code in the shader tab and applies it if it builds.
    bool compileShader();
    QString shaderLog() const;
    QPlainTextEdit* shaderEdit() const { return code_; }

signals:
    void saved();
    void popOutRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    struct Row {
        QWidget* widget = nullptr;
        QToolButton* thumb = nullptr; // shows the image; click to load one
        QToolButton* clear = nullptr;
        QSlider* slider = nullptr;
        QDoubleSpinBox* value = nullptr;
        float max = 1;
        uint64_t thumbRevision = ~uint64_t(0);
    };
    Material* material();
    // One undo step changing the material. structural: the faces must be rebuilt.
    void apply(const char* op, const std::function<void(Material&)>& fn, bool structural);
    void addRow(QVBoxLayout* lay, int slot, const QString& title, const QString& hint, float max);
    void wireSlider(QSlider* slider, const char* op, const std::function<void(Material&, float)>& set, float max, bool structural, QDoubleSpinBox* value);
    QDoubleSpinBox* valueBox(QWidget* parent, float max);
    void rebuildProps();
    void schedulePreview();
    void clearMap(int slot);

    Document* doc_;
    Viewport* vp_;
    QLabel* title_;
    QLabel* preview_;
    QLineEdit* name_;
    QPushButton* color_;
    QSlider* opacity_;
    QDoubleSpinBox* opacityValue_;
    QPushButton* emission_;
    QCheckBox* flipY_;
    QLineEdit* tileW_;
    QLineEdit* tileH_;
    Row rows_[MapSlotCount + 1];
    QWidget* content_;
    QPlainTextEdit* code_;
    QPlainTextEdit* log_;
    QCheckBox* shaderOn_;
    QCheckBox* blend_;
    QWidget* propsBox_;
    QFormLayout* propsForm_;
    QTimer* previewTimer_;
    QLabel* message_;
    std::string propsFor_; // the shader the property widgets were made for
    Id editing_ = kNone;
    bool updating_ = false;
};

} // namespace gs
