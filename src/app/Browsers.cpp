// SPDX-License-Identifier: GPL-3.0-or-later
#include "Browsers.h"

#include "Panels.h"
#include "Viewport.h"
#include "io/ArchMaterials.h"
#include "io/AssetLibrary.h"
#include "io/ImageUtil.h"
#include "io/NativeFormat.h"
#include "render/OffscreenRenderer.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace gs {

namespace {

constexpr int kThumb = 256; // pictures are made this big; the grid scales them

QString materialKey(const Material& m) {
    QString k = QString::fromStdString(m.name);
    k += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7").arg(m.color.r).arg(m.color.g).arg(m.color.b).arg(m.opacity).arg(m.baseStrength).arg(m.flipNormalY).arg(m.blend);
    k += QStringLiteral("|%1,%2,%3").arg(m.emission.r).arg(m.emission.g).arg(m.emission.b);
    k += QStringLiteral("|%1").arg(m.texture.revision);
    for (const MaterialMap& mp : m.maps) k += QStringLiteral(",%1:%2").arg(mp.tex.revision).arg(mp.strength);
    if (m.customShader()) {
        k += QStringLiteral("|%1").arg(qHash(QString::fromStdString(m.shader)));
        for (const auto& [name, v] : m.shaderProps) k += QStringLiteral(",%1=%2,%3,%4,%5").arg(QString::fromStdString(name)).arg(v[0]).arg(v[1]).arg(v[2]).arg(v[3]);
    }
    return k;
}

QPixmap makeMaterialThumb(const Material& m) {
    if (m.advanced()) {
        const QImage ball = OffscreenRenderer::instance().materialBall(m, kThumb);
        if (!ball.isNull()) return QPixmap::fromImage(ball);
    }
    if (m.hasTexture()) {
        const QImage img = decodeTexture(m.texture);
        if (!img.isNull()) return QPixmap::fromImage(img.scaled(kThumb, kThumb, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
    QPixmap pm(kThumb, kThumb);
    pm.fill(QColor::fromRgbF(m.color.r, m.color.g, m.color.b));
    return pm;
}

QPixmap imageFileThumb(const QString& path) {
    QImageReader reader(path);
    const QSize s = reader.size();
    if (s.isValid()) reader.setScaledSize(s.scaled(kThumb, kThumb, Qt::KeepAspectRatio)); // cheap for large JPEGs
    const QImage img = reader.read();
    return img.isNull() ? QPixmap() : QPixmap::fromImage(img);
}

} // namespace

QPixmap materialThumbnail(const Material& m) {
    static QHash<QString, QPixmap> cache;
    const QString key = materialKey(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it.value();
    if (cache.size() > 500) cache.clear();
    const QPixmap pm = makeMaterialThumb(m);
    cache.insert(key, pm);
    return pm;
}

QPixmap materialGlobe(const Material& m) {
    static QHash<QString, QPixmap> cache;
    const QString key = materialKey(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it.value();
    if (cache.size() > 500) cache.clear();
    const QImage ball = OffscreenRenderer::instance().materialBall(m, 192);
    const QPixmap pm = ball.isNull() ? QPixmap() : QPixmap::fromImage(ball);
    cache.insert(key, pm);
    return pm;
}

// The grid; a drag carries what the browser says the item is.
class AssetList : public QListWidget {
public:
    AssetList(AssetBrowser* owner) : QListWidget(owner), owner_(owner) {}

protected:
    QStringList mimeTypes() const override {
        return {QString::fromLatin1(Viewport::materialMime()), QString::fromLatin1(Viewport::prefabMime()), QStringLiteral("text/uri-list")};
    }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        QString type;
        const QByteArray data = owner_->dragPayload(items.first()->data(Qt::UserRole).toInt(), &type);
        if (data.isEmpty()) return nullptr;
        auto* md = new QMimeData;
        md->setData(type, data);
        return md;
    }

private:
    AssetBrowser* owner_;
};

AssetBrowser::AssetBrowser(const QString& title, QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle(title);
    resize(860, 640);
    auto* lay = new QVBoxLayout(this);
    toolbar_ = new QHBoxLayout();
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search…"));
    search_->setClearButtonEnabled(true);
    toolbar_->addWidget(search_, 1);
    category_ = new QComboBox(this);
    category_->hide();
    toolbar_->addWidget(category_);
    lay->addLayout(toolbar_);

    list_ = new AssetList(this);
    list_->setViewMode(QListView::IconMode);
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setWordWrap(true);
    list_->setUniformItemSizes(true);
    list_->setSpacing(4);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setDragEnabled(true);
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    lay->addWidget(list_, 1);

    auto* bottom = new QHBoxLayout();
    count_ = new QLabel(this);
    bottom->addWidget(count_, 1);
    bottom->addWidget(new QLabel(QStringLiteral("Size:"), this));
    zoom_ = new QSlider(Qt::Horizontal, this);
    zoom_->setRange(48, kThumb);
    zoom_->setFixedWidth(180);
    bottom->addWidget(zoom_);
    lay->addLayout(bottom);

    connect(zoom_, &QSlider::valueChanged, this, [this](int v) {
        list_->setIconSize(QSize(v, v));
        list_->setGridSize(QSize(v + 24, v + 44));
        QSettings().setValue("browser/iconSize", v);
    });
    zoom_->setValue(QSettings().value("browser/iconSize", 112).toInt());
    connect(search_, &QLineEdit::textChanged, this, [this] { applySearch(); });
    connect(category_, &QComboBox::currentIndexChanged, this, [this] { rebuild(); });

    thumbTimer_ = new QTimer(this);
    thumbTimer_->setInterval(0);
    connect(thumbTimer_, &QTimer::timeout, this, [this] { workOnThumbs(); });
}

void AssetBrowser::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    rebuild();
}

void AssetBrowser::resetList() {
    list_->clear();
    jobs_.clear();
    waiting_.clear();
}

void AssetBrowser::addItem(const QString& name, const QString& key, const std::function<QPixmap()>& make, int index) {
    auto* it = new QListWidgetItem(name, list_);
    it->setData(Qt::UserRole, index);
    it->setToolTip(name);
    it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    auto cached = thumbs_.constFind(key);
    if (cached != thumbs_.constEnd()) {
        it->setIcon(QIcon(cached.value()));
        return;
    }
    QPixmap blank(kThumb, kThumb);
    blank.fill(QColor(232, 232, 232));
    it->setIcon(QIcon(blank));
    auto w = waiting_.find(key);
    if (w == waiting_.end()) {
        waiting_.insert(key, {it});
        jobs_.push_back({key, make});
    } else {
        w.value().append(it);
    }
}

void AssetBrowser::finishList() {
    applySearch();
    if (!jobs_.empty()) thumbTimer_->start();
}

void AssetBrowser::applySearch() {
    const QStringList words = search_->text().split(' ', Qt::SkipEmptyParts);
    int shown = 0;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        bool ok = true;
        for (const QString& w : words) ok = ok && it->text().contains(w, Qt::CaseInsensitive);
        it->setHidden(!ok);
        shown += ok;
    }
    count_->setText(QStringLiteral("%1 of %2").arg(shown).arg(list_->count()));
}

void AssetBrowser::workOnThumbs() {
    // A slice at a time, so the window stays responsive with thousands of images.
    QElapsedTimer t;
    t.start();
    while (!jobs_.empty() && t.elapsed() < 16) {
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        const QPixmap pm = job.make();
        if (thumbs_.size() > 4000) thumbs_.clear();
        thumbs_.insert(job.key, pm);
        for (QListWidgetItem* it : waiting_.take(job.key))
            if (!pm.isNull()) it->setIcon(QIcon(pm));
    }
    if (jobs_.empty()) thumbTimer_->stop();
}

// ===========================================================================
// Materials and textures
// ===========================================================================

MaterialBrowser::MaterialBrowser(Document* doc, Viewport* vp, MaterialsPanel* panel, QWidget* parent)
    : AssetBrowser(QStringLiteral("Material Browser"), parent), doc_(doc), vp_(vp), panel_(panel) {
    builtin_ = builtinPbrMaterials();
    category_->addItems({QStringLiteral("All"), QStringLiteral("In Model"), QStringLiteral("PBR Materials"), QStringLiteral("Textures"), QStringLiteral("Colors")});
    category_->show();
    auto* fromImages = new QPushButton(QStringLiteral("New From Images…"), this);
    fromImages->setToolTip(QStringLiteral("Select all the map images of a material; each goes to the right map by its file name."));
    toolbar()->addWidget(fromImages);
    auto* addFolder = new QPushButton(QStringLiteral("Add Texture Folder…"), this);
    toolbar()->addWidget(addFolder);
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->hide();
    static_cast<QVBoxLayout*>(layout())->insertWidget(1, message_);

    connect(fromImages, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Choose the Material's Images"), QString(),
                                                                QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp *.tga *.tif *.tiff)"));
        if (paths.isEmpty()) return;
        Material m;
        QString err;
        if (!materialFromImages(paths, m, &err)) {
            message_->setText(err);
            message_->show();
            return;
        }
        doc_->beginOp("Add Material");
        doc_->currentMaterial = doc_->addMaterial(m);
        doc_->commitOp();
        emit chosen();
        emit editRequested();
    });
    connect(addFolder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Add Texture Folder"));
        if (dir.isEmpty()) return;
        QSettings s;
        QStringList folders = s.value("browser/textureFolders").toStringList();
        if (!folders.contains(dir)) folders << dir;
        s.setValue("browser/textureFolders", folders);
        rebuild();
    });
    connect(list(), &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { choose(list()->row(it)); });
    connect(list(), &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        choose(list()->row(it));
        emit editRequested();
    });
    connect(list(), &QListWidget::customContextMenuRequested, this, [this](const QPoint& p) { contextMenu(p); });
}

int MaterialBrowser::visibleCount() const {
    int n = 0;
    for (int i = 0; i < list()->count(); ++i) n += !list()->item(i)->isHidden();
    return n;
}

void MaterialBrowser::refreshFromModel() {
    if (!isVisible() || modelRevision_ == doc_->materialsRevision) return;
    rebuild();
}

void MaterialBrowser::rebuild() {
    modelRevision_ = doc_->materialsRevision;
    resetList();
    entries_.clear();
    const int cat = category_->currentIndex();
    const bool all = cat == 0;
    auto push = [&](Entry e) {
        const int index = (int)entries_.size();
        entries_.push_back(std::move(e));
        const Entry& en = entries_.back();
        if (en.source == Entry::ImageFile) {
            const QString path = en.path;
            addItem(en.name, QStringLiteral("file:") + path, [path] { return imageFileThumb(path); }, index);
        } else {
            const Material m = en.material;
            addItem(en.name, materialKey(m), [m] { return makeMaterialThumb(m); }, index);
        }
    };
    if (all || cat == 1)
        for (Id i = 0; i < (Id)doc_->materials().size(); ++i) {
            Entry e;
            e.source = Entry::InModel;
            e.id = i;
            e.material = doc_->materials()[i];
            e.name = QString::fromStdString(e.material.name);
            push(std::move(e));
        }
    if (all || cat == 2) {
        for (const QString& path : libraryMaterialFiles()) {
            Entry e;
            e.source = Entry::Library;
            e.path = path;
            if (!loadLibraryMaterial(path, e.material)) continue;
            e.name = QString::fromStdString(e.material.name);
            push(std::move(e));
        }
        for (int i = 0; i < architecturalMaterialCount(); ++i) {
            Entry e;
            e.source = Entry::Pbr;
            e.material = architecturalMaterial(i);
            e.name = QString::fromStdString(e.material.name);
            push(std::move(e));
        }
        for (const Material& m : builtin_) {
            Entry e;
            e.source = Entry::Pbr;
            e.material = m;
            e.name = QString::fromStdString(m.name);
            push(std::move(e));
        }
    }
    if (all || cat == 3) {
        for (const Material& m : panel_->textureLibrary()) {
            Entry e;
            e.source = Entry::Texture;
            e.material = m;
            e.name = QString::fromStdString(m.name);
            push(std::move(e));
        }
        for (const QString& path : textureFiles(QSettings().value("browser/textureFolders").toStringList())) {
            Entry e;
            e.source = Entry::ImageFile;
            e.path = path;
            e.name = QFileInfo(path).completeBaseName();
            push(std::move(e));
        }
    }
    if (all || cat == 4)
        for (const Material& m : panel_->colorLibrary()) {
            Entry e;
            e.source = Entry::Color;
            e.material = m;
            e.name = QString::fromStdString(m.name);
            push(std::move(e));
        }
    finishList();
}

bool MaterialBrowser::resolve(const Entry& e, Material& out, QString* error) const {
    if (e.source == Entry::InModel) {
        if (e.id < 0 || e.id >= (Id)doc_->materials().size()) return false;
        out = doc_->materials()[e.id];
        return true;
    }
    if (e.source != Entry::ImageFile) {
        out = e.material;
        return true;
    }
    Texture t;
    if (!loadTextureFile(e.path, t, error)) return false;
    out = Material();
    out.name = e.name.toStdString();
    out.texture = t;
    out.color = averageColor(decodeTexture(t));
    return true;
}

void MaterialBrowser::choose(int row) {
    QListWidgetItem* it = list()->item(row);
    if (!it) return;
    const int index = it->data(Qt::UserRole).toInt();
    if (index < 0 || index >= (int)entries_.size()) return;
    const Entry& e = entries_[index];
    Material m;
    QString err;
    if (!resolve(e, m, &err)) {
        message_->setText(err);
        message_->show();
        return;
    }
    message_->hide();
    if (e.source == Entry::InModel) {
        doc_->currentMaterial = e.id;
    } else {
        Id id = kNone;
        for (Id i = 0; i < (Id)doc_->materials().size() && id == kNone; ++i)
            if (sameMaterial(doc_->materials()[i], m)) id = i;
        if (id == kNone) {
            doc_->beginOp("Add Material");
            id = doc_->addMaterial(m);
            doc_->commitOp();
        }
        doc_->currentMaterial = id;
    }
    // The active tool stays: the Texture Brush and others use this material too.
    panel_->refresh();
    emit chosen();
}

QByteArray MaterialBrowser::dragPayload(int index, QString* mime) const {
    if (index < 0 || index >= (int)entries_.size()) return {};
    Material m;
    if (!resolve(entries_[index], m, nullptr)) return {};
    *mime = QString::fromLatin1(Viewport::materialMime());
    return QJsonDocument(materialToJson(m)).toJson(QJsonDocument::Compact);
}

void MaterialBrowser::contextMenu(const QPoint& pos) {
    QListWidgetItem* it = list()->itemAt(pos);
    QMenu menu(this);
    if (it) {
        const int row = list()->row(it);
        const int index = it->data(Qt::UserRole).toInt();
        const Entry e = entries_[index];
        menu.addAction(QStringLiteral("Edit Maps and Shader…"), this, [this, row] {
            choose(row);
            emit editRequested();
        });
        if (e.source == Entry::InModel)
            menu.addAction(QStringLiteral("Save to Library"), this, [this, e] {
                QString err;
                const QString path = saveLibraryMaterial(doc_->materials()[e.id], &err);
                message_->setText(path.isEmpty() ? err : QStringLiteral("Saved to %1").arg(path));
                message_->show();
                rebuild();
            });
        if (e.source == Entry::Library)
            menu.addAction(QStringLiteral("Delete From Library…"), this, [this, e] {
                if (QMessageBox::question(this, QStringLiteral("Delete Material"),
                                          QStringLiteral("Delete \"%1\" from the library? Models using it keep their copy.").arg(e.name)) != QMessageBox::Yes)
                    return;
                QFile::remove(e.path);
                rebuild();
            });
        menu.addSeparator();
    }
    menu.addAction(QStringLiteral("Open Library Folder"), this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(assetDir(QStringLiteral("materials")))); });
    menu.addAction(QStringLiteral("Open Textures Folder"), this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(assetDir(QStringLiteral("textures")))); });
    menu.exec(list()->viewport()->mapToGlobal(pos));
}

// ===========================================================================
// Prefabs
// ===========================================================================

PrefabBrowser::PrefabBrowser(QWidget* parent) : AssetBrowser(QStringLiteral("Prefab Browser"), parent) {
    auto* refresh = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar()->addWidget(refresh);
    auto* folder = new QPushButton(QStringLiteral("Open Folder"), this);
    toolbar()->addWidget(folder);
    connect(refresh, &QPushButton::clicked, this, [this] { rebuild(); });
    connect(folder, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(assetDir(QStringLiteral("prefabs")))); });
    connect(list(), &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const int i = it->data(Qt::UserRole).toInt();
        if (i >= 0 && i < (int)paths_.size()) emit placeRequested(paths_[i]);
    });
    connect(list(), &QListWidget::customContextMenuRequested, this, [this](const QPoint& p) { contextMenu(p); });
    list()->setToolTip(QStringLiteral("Drag a prefab into the model, or double-click it to place it with the mouse."));
}

void PrefabBrowser::rebuild() {
    resetList();
    paths_.clear();
    for (const QString& path : prefabFiles()) {
        const qint64 modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        auto c = cache_.find(path);
        if (c == cache_.end() || c->modified != modified) {
            // Reads the whole file, so only once per version of it.
            Cached fresh;
            fresh.modified = modified;
            ModelState st;
            if (!loadPrefab(path, st, &fresh.name, &fresh.thumbnail)) continue;
            if (fresh.thumbnail.isNull()) fresh.thumbnail = OffscreenRenderer::instance().modelThumbnail(st, kThumb);
            c = cache_.insert(path, fresh);
        }
        const int index = (int)paths_.size();
        paths_.push_back(path);
        const QImage thumb = c->thumbnail;
        addItem(c->name, QStringLiteral("%1@%2").arg(path).arg(modified), [thumb] { return QPixmap::fromImage(thumb); }, index);
    }
    finishList();
}

QByteArray PrefabBrowser::dragPayload(int index, QString* mime) const {
    if (index < 0 || index >= (int)paths_.size()) return {};
    *mime = QString::fromLatin1(Viewport::prefabMime());
    return paths_[index].toUtf8();
}

void PrefabBrowser::contextMenu(const QPoint& pos) {
    QListWidgetItem* it = list()->itemAt(pos);
    if (!it) return;
    const QString path = paths_[it->data(Qt::UserRole).toInt()];
    const QString name = it->text();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Place in Model"), this, [this, path] { emit placeRequested(path); });
    menu.addAction(QStringLiteral("Open to Edit…"), this, [this, path] { emit openRequested(path); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Delete Prefab…"), this, [this, path, name] {
        if (QMessageBox::question(this, QStringLiteral("Delete Prefab"),
                                  QStringLiteral("Delete the file of \"%1\"? Models using it keep their copy.").arg(name)) != QMessageBox::Yes)
            return;
        if (!QFile::remove(path)) QMessageBox::warning(this, QStringLiteral("Delete Prefab"), QStringLiteral("Could not delete %1").arg(path));
        rebuild();
    });
    menu.exec(list()->viewport()->mapToGlobal(pos));
}

} // namespace gs
