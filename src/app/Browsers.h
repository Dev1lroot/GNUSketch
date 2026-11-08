// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - large searchable windows for materials, textures and prefabs.
#pragma once

#include "core/Document.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

#include <deque>
#include <functional>
#include <vector>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QTimer;

namespace gs {

class Viewport;
class MaterialsPanel;

// A picture of a material, kept until it changes (a ball for PBR materials).
QPixmap materialThumbnail(const Material& m);
// The material on a ball, whatever kind it is.
QPixmap materialGlobe(const Material& m);

// Search box, category choice, zoomable grid of pictures made in the background.
class AssetBrowser : public QWidget {
    Q_OBJECT
public:
    AssetBrowser(const QString& title, QWidget* parent);
    QLineEdit* searchBox() const { return search_; }
    QListWidget* list() const { return list_; }

protected:
    void addItem(const QString& name, const QString& thumbKey, const std::function<QPixmap()>& makeThumb, int index);
    void resetList();
    void finishList(); // applies the search and starts making pictures
    QByteArray dragData(const QListWidgetItem* item) const;
    virtual QByteArray dragPayload(int index, QString* mime) const = 0;
    virtual void rebuild() = 0;
    void showEvent(QShowEvent* e) override;
    QHBoxLayout* toolbar() const { return toolbar_; }
    QComboBox* category_ = nullptr; // subclasses fill it, or it stays hidden

private:
    friend class AssetList;
    void applySearch();
    void workOnThumbs();
    QLineEdit* search_;
    QSlider* zoom_;
    QListWidget* list_;
    QLabel* count_;
    QHBoxLayout* toolbar_;
    QTimer* thumbTimer_;
    struct Job {
        QString key;
        std::function<QPixmap()> make;
    };
    std::deque<Job> jobs_;
    QHash<QString, QPixmap> thumbs_;
    QHash<QString, QList<QListWidgetItem*>> waiting_;
};

class MaterialBrowser : public AssetBrowser {
    Q_OBJECT
public:
    MaterialBrowser(Document* doc, Viewport* vp, MaterialsPanel* panel, QWidget* parent);
    void refreshFromModel();
    // Number of entries the search currently shows.
    int visibleCount() const;
    void rebuildNow() { rebuild(); }
    // Makes the entry the current material, as a click on it does.
    void choose(int row);

signals:
    void chosen();
    void editRequested();

protected:
    void rebuild() override;
    QByteArray dragPayload(int index, QString* mime) const override;

private:
    struct Entry {
        enum Source { InModel, Pbr, Library, Color, Texture, ImageFile } source = InModel;
        QString name, path;
        Id id = kNone;
        Material material;
    };
    // The entry as a material (loads image files).
    bool resolve(const Entry& e, Material& out, QString* error) const;
    void contextMenu(const QPoint& pos);
    Document* doc_;
    Viewport* vp_;
    MaterialsPanel* panel_;
    std::vector<Entry> entries_;
    std::vector<Material> builtin_;
    QLabel* message_;
    uint64_t modelRevision_ = 0;
};

class PrefabBrowser : public AssetBrowser {
    Q_OBJECT
public:
    explicit PrefabBrowser(QWidget* parent);
    void rebuildNow() { rebuild(); }
    int prefabCount() const { return (int)paths_.size(); }

signals:
    void placeRequested(const QString& path);
    void openRequested(const QString& path);

protected:
    void rebuild() override;
    QByteArray dragPayload(int index, QString* mime) const override;

private:
    void contextMenu(const QPoint& pos);
    std::vector<QString> paths_;
    struct Cached {
        qint64 modified = 0;
        QString name;
        QImage thumbnail;
    };
    QHash<QString, Cached> cache_;
};

} // namespace gs
