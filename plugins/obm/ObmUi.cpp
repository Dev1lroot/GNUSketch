// SPDX-License-Identifier: GPL-3.0-or-later
#include "ObmUi.h"

#include "ObmFormat.h"
#include "io/ImageUtil.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace gs::obm {

namespace {

std::vector<std::string> splitTags(const QString& text) {
    std::vector<std::string> out;
    for (QString t : text.split(QRegularExpression(QStringLiteral("[,\\n]")), Qt::SkipEmptyParts))
        if (t = t.trimmed(); !t.isEmpty()) out.push_back(t.toStdString());
    return out;
}
QString joinTags(const std::vector<std::string>& tags, const QString& sep) {
    QStringList l;
    for (const std::string& t : tags) l << QString::fromStdString(t);
    return l.join(sep);
}
QString num(double v) { return QString::number(v, 'g', 6); }

bool loadInto(Document& doc, Id m, const QString& file) {
    Texture t;
    if (!loadTextureFile(file, t)) return false;
    Material& mat = doc.state.materials[m];
    mat.texture = t;
    mat.color = averageColor(decodeTexture(t));
    ++doc.materialsRevision;
    return true;
}

} // namespace

int fixTexture(QWidget* parent, Document& doc, Id material, const QString& modelPath, const std::vector<Id>& others) {
    if (material < 0 || material >= (Id)doc.state.materials.size()) return 0;
    const std::string name = doc.state.materials[material].name;
    // (from where it would be: the textures folder, as near as is known)
    QString folder = texturesFolder(modelPath);
    QString start = folder.isEmpty() ? QFileInfo(modelPath).absolutePath() : QFileInfo(picturePath(folder, name)).absolutePath();
    if (!QDir(start).exists()) start = folder.isEmpty() ? QDir::homePath() : folder;
    const QString file = QFileDialog::getOpenFileName(parent, QStringLiteral("Picture for %1").arg(QString::fromStdString(name)), start,
                                                      QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*)"));
    if (file.isEmpty()) return 0;
    if (!loadInto(doc, material, file)) {
        QMessageBox::warning(parent, QStringLiteral("Missing Texture"), QStringLiteral("%1 could not be read as a picture.").arg(file));
        return 0;
    }
    int fixed = 1;
    // picked where it belongs in a textures folder: that folder, for the rest (and from now on)
    const std::string base = name.substr(0, name.find('@'));
    QString stem = QDir::cleanPath(QFileInfo(file).absolutePath() + "/" + QFileInfo(file).completeBaseName());
    const QString tail = "/" + QString::fromStdString(base);
    if (stem.endsWith(tail)) {
        const QString root = stem.left(stem.size() - tail.size());
        QSettings().setValue(QStringLiteral("obm/texturesDir"), root);
        for (Id o : others)
            if (o != material && o >= 0 && o < (Id)doc.state.materials.size() && !doc.state.materials[o].hasTexture())
                if (QFile::exists(picturePath(root, doc.state.materials[o].name)) && loadInto(doc, o, picturePath(root, doc.state.materials[o].name))) ++fixed;
    }
    return fixed;
}

void askMissing(QWidget* parent, Document& doc, std::vector<Id> missing, const QString& modelPath, QStringList* messages) {
    const QString folder = texturesFolder(modelPath);
    for (size_t i = 0; i < missing.size(); ++i) {
        const Id m = missing[i];
        if (doc.state.materials[m].hasTexture()) continue; // (found with another one's folder)
        const QString name = QString::fromStdString(doc.state.materials[m].name);
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Missing Texture"), QStringLiteral("Missing texture: <b>%1</b>").arg(name.toHtmlEscaped()),
                        QMessageBox::NoButton, parent);
        box.setInformativeText(folder.isEmpty()
                                   ? QStringLiteral("The game's textures folder wasn't found beside the model's \"models\" folder. Pick the picture, and "
                                                    "if it is in the textures folder (as %1.png) the rest are found there too.")
                                         .arg(name.section('@', 0, 0))
                                   : QStringLiteral("It isn't in %1 (as %2.png). Pick the picture to use.").arg(folder, name.section('@', 0, 0)));
        QPushButton* fix = box.addButton(QStringLiteral("Fix it…"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Skip"), QMessageBox::RejectRole);
        QPushButton* skipAll = box.addButton(QStringLiteral("Skip all"), QMessageBox::DestructiveRole);
        box.setDefaultButton(fix);
        box.exec();
        if (box.clickedButton() == skipAll) break;
        if (box.clickedButton() == fix) fixTexture(parent, doc, m, modelPath, std::vector<Id>(missing.begin() + long(i) + 1, missing.end()));
    }
    int still = 0;
    for (Id m : missing) still += !doc.state.materials[m].hasTexture();
    if (still && messages)
        messages->append(QStringLiteral("%1 pictures are still missing: shown magenta. Properties > OBM lists them, with Fix it.").arg(still));
}

// ---- the tab ---------------------------------------------------------------------
ObmTab::ObmTab(Document* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 8, 0, 0);
    group_ = new QLabel(this);
    group_->setWordWrap(true);
    lay->addWidget(group_);
    lay->addWidget(new QLabel(QStringLiteral("Group tags (one a line: animation:head, spin:y, hidden, …)"), this));
    tags_ = new QPlainTextEdit(this);
    tags_->setMaximumHeight(80);
    lay->addWidget(tags_);
    applyTags_ = new QPushButton(QStringLiteral("Apply"), this);
    applyTags_->setEnabled(false);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(applyTags_);
    lay->addLayout(row);
    connect(tags_, &QPlainTextEdit::textChanged, this, [this] {
        if (!filling_) applyTags_->setEnabled(true);
    });
    connect(applyTags_, &QPushButton::clicked, this, [this] {
        const std::vector<std::string> t = splitTags(tags_->toPlainText());
        edit("Group Tags", [&] { setGroupTags(*doc_, def_, t); });
        applyTags_->setEnabled(false);
    });

    lay->addWidget(new QLabel(QStringLiteral("Cubes (tags separated by commas: emissive, overlay, state:lit=on, …)"), this));
    cubes_ = new QTableWidget(0, 2, this);
    cubes_->setHorizontalHeaderLabels({QStringLiteral("Cube"), QStringLiteral("Tags")});
    cubes_->horizontalHeader()->setStretchLastSection(true);
    cubes_->verticalHeader()->hide();
    lay->addWidget(cubes_, 1);
    connect(cubes_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (filling_ || it->column() != 1) return;
        const int cube = it->row();
        const std::vector<std::string> t = splitTags(it->text());
        edit("Cube Tags", [&] { setCubeTags(*doc_, def_, cube, t); });
    });

    auto* box = new QGroupBox(QStringLiteral("Model"), this);
    auto* form = new QFormLayout(box);
    model_ = new QLabel(box);
    model_->setWordWrap(true);
    model_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(model_);
    modelTags_ = new QLineEdit(box);
    modelTags_->setPlaceholderText(QStringLiteral("tags, separated by commas"));
    form->addRow(QStringLiteral("Tags"), modelTags_);
    connect(modelTags_, &QLineEdit::editingFinished, this, [this] {
        if (filling_) return;
        ModelInfo mi;
        const std::vector<std::string> t = splitTags(modelTags_->text());
        if (modelInfo(*doc_, mi) && t != mi.tags) edit("Model Tags", [&] { setModelTags(*doc_, t); });
    });
    pictures_ = new QTableWidget(0, 3, box);
    pictures_->setHorizontalHeaderLabels({QStringLiteral("Picture"), QStringLiteral(""), QStringLiteral("")});
    pictures_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    pictures_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    pictures_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    pictures_->verticalHeader()->hide();
    pictures_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pictures_->setMaximumHeight(140);
    form->addRow(pictures_);
    lay->addWidget(box);
}

void ObmTab::edit(const char* what, const std::function<void()>& fn) {
    doc_->beginOp(what);
    fn();
    doc_->commitOp();
    doc_->modified = true;
    doc_->notifyChanged();
}

bool ObmTab::showFor(Id instance) {
    inst_ = instance;
    def_ = instance >= 0 && instance < (Id)doc_->state.instances.size() ? doc_->state.instances[instance].def : kNone;
    GroupInfo g;
    ModelInfo mi;
    const bool hasGroup = groupInfo(*doc_, def_, g), hasModel = modelInfo(*doc_, mi);
    if (!hasGroup && !hasModel) return false;
    filling_ = true;
    if (hasGroup) {
        group_->setText(QStringLiteral("<b>%1</b> &nbsp; pivot %2, %3, %4 &nbsp; rotation %5, %6, %7")
                            .arg(QString::fromStdString(g.name).toHtmlEscaped(), num(g.pivot.x), num(g.pivot.y), num(g.pivot.z), num(g.rotation.x),
                                 num(g.rotation.y), num(g.rotation.z)));
        if (!tags_->hasFocus()) tags_->setPlainText(joinTags(g.tags, "\n")), applyTags_->setEnabled(false);
        cubes_->setRowCount(int(g.cubes.size()));
        for (int r = 0; r < int(g.cubes.size()); ++r) {
            auto* n = new QTableWidgetItem(QString::fromStdString(g.cubes[size_t(r)].name));
            n->setFlags(n->flags() & ~Qt::ItemIsEditable);
            cubes_->setItem(r, 0, n);
            cubes_->setItem(r, 1, new QTableWidgetItem(joinTags(g.cubes[size_t(r)].tags, ", ")));
        }
    } else {
        group_->setText(QStringLiteral("Made here: not a group of the .obm it was read from (written as a new group)."));
        tags_->clear();
        cubes_->setRowCount(0);
    }
    tags_->setEnabled(hasGroup), cubes_->setEnabled(hasGroup);
    if (hasModel) {
        model_->setText(QStringLiteral("%1<br>%2, anchor %3<br><small>%4</small>")
                            .arg(QString::fromStdString(mi.id.empty() ? std::string("(no id)") : mi.id).toHtmlEscaped(),
                                 QString::fromStdString(mi.kind.empty() ? std::string("other") : mi.kind), QString::fromStdString(mi.anchor),
                                 QString::fromStdString(mi.file).toHtmlEscaped()));
        if (!modelTags_->hasFocus()) modelTags_->setText(joinTags(mi.tags, ", "));
        pictures_->setRowCount(int(mi.textures.size()));
        std::vector<Id> missing;
        for (const std::string& p : mi.textures)
            for (Id m = 0; m < (Id)doc_->state.materials.size(); ++m)
                if (doc_->state.materials[m].name == p && !doc_->state.materials[m].hasTexture()) missing.push_back(m);
        for (int r = 0; r < int(mi.textures.size()); ++r) {
            const std::string& p = mi.textures[size_t(r)];
            Id mat = kNone;
            for (Id m = 0; m < (Id)doc_->state.materials.size() && mat == kNone; ++m)
                if (doc_->state.materials[m].name == p) mat = m;
            const bool found = mat != kNone && doc_->state.materials[mat].hasTexture();
            pictures_->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(p)));
            auto* st = new QTableWidgetItem(mat == kNone ? QStringLiteral("unused") : found ? QStringLiteral("found") : QStringLiteral("missing"));
            if (mat != kNone && !found) st->setForeground(QColor(190, 30, 30));
            pictures_->setItem(r, 1, st);
            if (mat != kNone && !found) {
                auto* fix = new QPushButton(QStringLiteral("Fix it…"), pictures_);
                const QString file = QString::fromStdString(mi.file);
                connect(fix, &QPushButton::clicked, this, [this, mat, file, missing] {
                    Document& d = *doc_;
                    d.beginOp("Fix Texture");
                    const int n = fixTexture(this, d, mat, file, missing);
                    if (n) d.commitOp(), d.modified = true, d.notifyChanged();
                    else d.cancelOp();
                });
                pictures_->setCellWidget(r, 2, fix);
            } else {
                pictures_->removeCellWidget(r, 2);
            }
        }
    } else {
        model_->setText(QStringLiteral("(not read from an .obm)"));
        pictures_->setRowCount(0);
    }
    filling_ = false;
    return true;
}

} // namespace gs::obm
