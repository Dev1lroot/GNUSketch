// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panels.h"

#include "Browsers.h"
#include "Icons.h"
#include "MaterialEditor.h"
#include "TerrainTools.h"
#include "Viewport.h"
#include "core/Components.h"
#include "core/Solids.h"
#include "io/ArchMaterials.h"
#include "io/ImageUtil.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <random>

namespace gs {

namespace {

QColor toQ(const Color& c) { return QColor::fromRgbF(c.r, c.g, c.b, c.a); }
Color fromQ(const QColor& c) { return Color((float)c.redF(), (float)c.greenF(), (float)c.blueF(), 1.0f); }

void setButtonColor(QPushButton* b, const Color& c) {
    QPixmap pm(28, 14);
    pm.fill(toQ(Color(c.r, c.g, c.b, 1)));
    QPainter p(&pm);
    p.setPen(QColor(80, 80, 80));
    p.drawRect(0, 0, 27, 13);
    b->setIcon(QIcon(pm));
    b->setIconSize(QSize(28, 14));
}

// ---- built-in procedural textures (generated, no external assets) ---------

QImage makeBrick(std::mt19937& rng) {
    QImage img(256, 256, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(img.rect(), QColor(205, 200, 190));
    std::uniform_int_distribution<int> d(-18, 18);
    const int rows = 8, bh = 256 / rows, bw = 64;
    for (int r = 0; r < rows; ++r) {
        int off = (r % 2) ? bw / 2 : 0;
        for (int c = -1; c < 5; ++c) {
            int v = d(rng);
            QColor col(std::clamp(158 + v, 0, 255), std::clamp(70 + v / 2, 0, 255), std::clamp(52 + v / 3, 0, 255));
            p.fillRect(QRect(c * bw + off + 2, r * bh + 2, bw - 4, bh - 4), col);
        }
    }
    std::uniform_int_distribution<int> px(0, 255), n(-12, 12);
    for (int i = 0; i < 9000; ++i) {
        int x = px(rng), y = px(rng);
        QColor c = img.pixelColor(x, y);
        int k = n(rng);
        img.setPixelColor(x, y, QColor(std::clamp(c.red() + k, 0, 255), std::clamp(c.green() + k, 0, 255), std::clamp(c.blue() + k, 0, 255)));
    }
    return img;
}

QImage makeWood(std::mt19937& rng) {
    QImage img(256, 256, QImage::Format_RGB32);
    std::uniform_real_distribution<double> u(0, 1);
    const int planks = 4;
    double phase[planks], tone[planks];
    for (int i = 0; i < planks; ++i) { phase[i] = u(rng) * 10; tone[i] = u(rng) * 30 - 15; }
    for (int y = 0; y < 256; ++y) {
        int pl = y / (256 / planks);
        for (int x = 0; x < 256; ++x) {
            double g = std::sin((y * 0.35 + std::sin(x * 0.03 + phase[pl]) * 6.0) + phase[pl]) * 0.5 + 0.5;
            double ring = std::sin(x * 0.012 + y * 0.09 + phase[pl] * 2) * 0.5 + 0.5;
            double v = 0.65 * g + 0.35 * ring;
            int r = int(150 + tone[pl] + v * 45), gg = int(100 + tone[pl] * 0.7 + v * 32), b = int(55 + v * 18);
            if (y % (256 / planks) < 2) { r -= 50; gg -= 40; b -= 25; }
            img.setPixelColor(x, y, QColor(std::clamp(r, 0, 255), std::clamp(gg, 0, 255), std::clamp(b, 0, 255)));
        }
    }
    return img;
}

QImage makeTiles(std::mt19937& rng) {
    QImage img(256, 256, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(img.rect(), QColor(170, 170, 165));
    std::uniform_int_distribution<int> d(-10, 10);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            int v = d(rng);
            p.fillRect(QRect(c * 64 + 2, r * 64 + 2, 60, 60), QColor(228 + v / 2, 224 + v / 2, 214 + v / 2));
        }
    return img;
}

QImage makeNoise(std::mt19937& rng, QColor base, int amp, int blobs) {
    QImage img(256, 256, QImage::Format_RGB32);
    img.fill(base);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    std::uniform_int_distribution<int> pos(0, 255), rad(2, 9), n(-amp, amp);
    for (int i = 0; i < blobs; ++i) {
        int k = n(rng);
        QColor c(std::clamp(base.red() + k, 0, 255), std::clamp(base.green() + k, 0, 255), std::clamp(base.blue() + k, 0, 255), 120);
        int x = pos(rng), y = pos(rng), r = rad(rng);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int ox : {-256, 0, 256})
            for (int oy : {-256, 0, 256}) p.drawEllipse(QPoint(x + ox, y + oy), r, r);
    }
    return img;
}

QImage makeStone(std::mt19937& rng) {
    QImage img(256, 256, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(img.rect(), QColor(120, 115, 108));
    std::uniform_int_distribution<int> w(30, 70), d(-25, 25);
    for (int y = 0; y < 256; y += 32) {
        int x = (y / 32 % 2) ? -20 : 0;
        while (x < 256) {
            int ww = w(rng), v = d(rng);
            p.fillRect(QRect(x + 2, y + 2, ww - 4, 28), QColor(170 + v, 162 + v, 150 + v));
            x += ww;
        }
    }
    return img;
}

} // namespace

// ===========================================================================
// Entity Info
// ===========================================================================

EntityInfoPanel::EntityInfoPanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 8);
    title_ = new QLabel(this);
    QFont f = title_->font();
    f.setBold(true);
    title_->setFont(f);
    lay->addWidget(title_);

    faceBox_ = new QWidget(this);
    auto* fl = new QGridLayout(faceBox_);
    fl->setContentsMargins(0, 0, 0, 0);
    swatch_ = new QLabel(faceBox_);
    swatch_->setFixedSize(36, 36);
    swatch_->setFrameShape(QFrame::Box);
    matName_ = new QLabel(faceBox_);
    reverse_ = new QPushButton(QStringLiteral("Reverse Faces"), faceBox_);
    fl->addWidget(new QLabel(QStringLiteral("Material:"), faceBox_), 0, 0);
    fl->addWidget(swatch_, 0, 1);
    fl->addWidget(matName_, 0, 2);
    fl->addWidget(reverse_, 1, 0, 1, 3);
    fl->setColumnStretch(2, 1);
    lay->addWidget(faceBox_);

    auto* ml = new QHBoxLayout();
    measureLabel_ = new QLabel(this);
    measure_ = new QLabel(this);
    measure_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ml->addWidget(measureLabel_);
    ml->addWidget(measure_, 1);
    lay->addLayout(ml);

    edgeBox_ = new QWidget(this);
    auto* el = new QHBoxLayout(edgeBox_);
    el->setContentsMargins(0, 0, 0, 0);
    soft_ = new QCheckBox(QStringLiteral("Soft"), edgeBox_);
    smooth_ = new QCheckBox(QStringLiteral("Smooth"), edgeBox_);
    hidden_ = new QCheckBox(QStringLiteral("Hidden"), edgeBox_);
    el->addWidget(soft_);
    el->addWidget(smooth_);
    el->addWidget(hidden_);
    el->addStretch(1);
    lay->addWidget(edgeBox_);

    for (QCheckBox* c : {soft_, smooth_, hidden_}) connect(c, &QCheckBox::toggled, this, [this] { applyEdgeFlags(); });
    connect(reverse_, &QPushButton::clicked, this, [this] {
        if (doc_->selection.faces.empty()) return;
        doc_->beginOp("Reverse Faces");
        for (Id f : doc_->selection.faces) doc_->mesh().reverseFace(f);
        doc_->mesh().touch();
        doc_->commitOp();
        vp_->update();
    });
    refresh();
}

void EntityInfoPanel::applyEdgeFlags() {
    if (updating_ || doc_->selection.edges.empty()) return;
    doc_->beginOp("Edge Properties");
    for (Id e : doc_->selection.edges) {
        Edge& E = doc_->mesh().edges[e];
        E.soft = soft_->isChecked();
        E.smooth = smooth_->isChecked();
        E.hidden = hidden_->isChecked();
    }
    doc_->mesh().touch();
    doc_->commitOp();
    vp_->update();
}

void EntityInfoPanel::refresh() {
    updating_ = true;
    const Selection& s = doc_->selection;
    const Mesh& m = doc_->mesh();
    faceBox_->setVisible(!s.faces.empty());
    edgeBox_->setVisible(!s.edges.empty() && s.faces.empty());
    measureLabel_->setVisible(!s.empty());
    measure_->setVisible(!s.empty());
    if (s.empty()) {
        title_->setText(doc_->editingComponent() ? QStringLiteral("Editing Component") : QStringLiteral("No Selection"));
    } else if (!s.objects.empty() && s.faces.empty() && s.edges.empty() && s.instances.empty()) {
        title_->setText(s.objects.size() == 1
                            ? QString::fromLatin1(sceneObjectTypeName(doc_->state.objects[*s.objects.begin()].kind))
                            : QStringLiteral("%1 Lights and Cameras").arg(s.objects.size()));
        measureLabel_->setText(s.objects.size() == 1 ? QString::fromStdString(doc_->state.objects[*s.objects.begin()].name) : QString());
        measure_->setText(QStringLiteral("Its settings are below"));
    } else if (!s.instances.empty() && s.faces.empty() && s.edges.empty()) {
        const ModelState& st = doc_->state;
        Id def = st.instances[*s.instances.begin()].def;
        bool same = true;
        for (Id i : s.instances) same = same && st.instances[i].def == def;
        title_->setText(s.instances.size() == 1 ? QStringLiteral("Component")
                                                : QStringLiteral("%1 Components").arg(s.instances.size()));
        if (same) {
            measureLabel_->setText(QString::fromStdString(st.defs[def].name));
            QString count = QStringLiteral("%1 in model").arg(instanceCount(st, def));
            // A solid can be used with the Solid Tools; show its volume like SketchUp.
            if (s.instances.size() == 1) {
                const Id inst = *s.instances.begin();
                const double v = solidVolume(st.defs[def].mesh);
                if (v > 0) {
                    title_->setText(QStringLiteral("Solid Component"));
                    count += QStringLiteral(", volume %1").arg(QString::fromStdString(
                        doc_->units.formatVolume(v * std::fabs(instanceWorldXf(st, inst).det3()))));
                }
            }
            measure_->setText(count);
        } else {
            measureLabel_->setText(QStringLiteral("Definitions:"));
            measure_->setText(QStringLiteral("mixed"));
        }
    } else if (s.count() == 1 && s.faces.size() == 1) {
        title_->setText(QStringLiteral("Face"));
    } else if (s.count() == 1) {
        title_->setText(QStringLiteral("Edge"));
    } else if (s.edges.empty()) {
        title_->setText(QStringLiteral("%1 Faces").arg(s.faces.size()));
    } else if (s.faces.empty()) {
        title_->setText(QStringLiteral("%1 Edges").arg(s.edges.size()));
    } else {
        title_->setText(QStringLiteral("%1 Entities (%2 faces, %3 edges)").arg(s.count()).arg(s.faces.size()).arg(s.edges.size()));
    }
    if (!s.faces.empty()) {
        double area = 0;
        for (Id f : s.faces) area += m.faceArea(f);
        measureLabel_->setText(QStringLiteral("Area:"));
        measure_->setText(QString::fromStdString(doc_->units.formatArea(area)));
        Id fid = *s.faces.begin();
        Id mat = m.faces[fid].front.material;
        const Material* mm = doc_->material(mat);
        QPixmap pm(34, 34);
        Color c = mm ? mm->color : doc_->style.frontColor;
        pm.fill(toQ(Color(c.r, c.g, c.b, 1)));
        if (mm && mm->hasTexture()) {
            QImage img = decodeTexture(mm->texture);
            if (!img.isNull()) pm = QPixmap::fromImage(img.scaled(34, 34, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
        swatch_->setPixmap(pm);
        matName_->setText(mm ? QString::fromStdString(mm->name) : QStringLiteral("<Default>"));
    } else if (!s.edges.empty()) {
        double len = 0;
        bool soft = true, smooth = true, hidden = true;
        for (Id e : s.edges) {
            len += m.edgeLength(e);
            soft = soft && m.edges[e].soft;
            smooth = smooth && m.edges[e].smooth;
            hidden = hidden && m.edges[e].hidden;
        }
        measureLabel_->setText(QStringLiteral("Length:"));
        measure_->setText(QString::fromStdString(doc_->units.format(len)));
        soft_->setChecked(soft);
        smooth_->setChecked(smooth);
        hidden_->setChecked(hidden);
    }
    updating_ = false;
}

// ===========================================================================
// Materials
// ===========================================================================

MaterialsPanel::MaterialsPanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    setAcceptDrops(true);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    // The current material: its globe, its name, what to do with it.
    auto* top = new QHBoxLayout();
    preview_ = new QLabel(this);
    preview_->setFixedSize(76, 76);
    preview_->setAlignment(Qt::AlignCenter);
    top->addWidget(preview_);
    auto* nameCol = new QVBoxLayout();
    name_ = new QLineEdit(this);
    name_->setPlaceholderText(QStringLiteral("Default"));
    nameCol->addWidget(name_);
    auto* tools = new QHBoxLayout();
    auto* sampleBtn = new QToolButton(this);
    sampleBtn->setIcon(Icons::get("paint"));
    sampleBtn->setText(QStringLiteral("Paint"));
    sampleBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    sampleBtn->setToolTip(QStringLiteral("Paint Bucket: click faces to paint them with this material (Alt+click samples one)"));
    auto* defaultBtn = new QToolButton(this);
    defaultBtn->setText(QStringLiteral("Default"));
    defaultBtn->setToolTip(QStringLiteral("Choose no material: painting then restores the default look"));
    tools->addWidget(sampleBtn);
    tools->addWidget(defaultBtn);
    tools->addStretch(1);
    nameCol->addLayout(tools);
    nameCol->addStretch(1);
    top->addLayout(nameCol, 1);
    lay->addLayout(top);

    auto* createBtn = new QPushButton(Icons::get("new"), QStringLiteral("Create Material"), this);
    createBtn->setObjectName("CreateMaterial");
    createBtn->setToolTip(QStringLiteral("Start a new material and edit it here"));
    lay->addWidget(createBtn);

    auto* tabs = new QHBoxLayout();
    selectTab_ = new QToolButton(this);
    selectTab_->setText(QStringLiteral("Select"));
    editTab_ = new QToolButton(this);
    editTab_->setText(QStringLiteral("Edit"));
    for (QToolButton* t : {selectTab_, editTab_}) {
        t->setCheckable(true);
        t->setAutoRaise(true);
        t->setObjectName("FlatTab");
        tabs->addWidget(t);
    }
    tabs->addStretch(1);
    selectTab_->setChecked(true);
    lay->addLayout(tabs);

    pages_ = new QStackedWidget(this);
    // --- Select page
    auto* sel = new QWidget(pages_);
    auto* sl = new QVBoxLayout(sel);
    sl->setContentsMargins(0, 0, 0, 0);
    library_ = new QComboBox(sel);
    library_->addItems({QStringLiteral("In Model"), QStringLiteral("Architectural"), QStringLiteral("Colors"), QStringLiteral("Textures")});
    auto* libRow = new QHBoxLayout();
    libRow->addWidget(library_, 1);
    auto* browserBtn = new QPushButton(QStringLiteral("Browser…"), sel);
    browserBtn->setToolTip(QStringLiteral("Open the large Material Browser with search"));
    libRow->addWidget(browserBtn);
    sl->addLayout(libRow);
    list_ = new QListWidget(sel);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(58, 58));
    list_->setGridSize(QSize(64, 64));
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setUniformItemSizes(true);
    list_->setSpacing(1);
    list_->setMinimumHeight(210);
    sl->addWidget(list_);
    pages_->addWidget(sel);
    // --- Edit page: the editor itself, right here.
    editor_ = new MaterialEditor(doc_, vp_, MaterialEditor::InSidebar, pages_);
    editor_->setPreviewLabel(preview_);
    pages_->addWidget(editor_);
    lay->addWidget(pages_, 1);

    globeTimer_ = new QTimer(this);
    globeTimer_->setInterval(30);
    connect(globeTimer_, &QTimer::timeout, this, [this] { fillGlobes(); });
    prepareArchitecturalMaterials();

    // Libraries.
    {
        static const int hues[] = {0, 25, 45, 60, 100, 140, 190, 215, 250, 290, 330};
        for (int h : hues)
            for (int k = 0; k < 5; ++k) {
                QColor c = QColor::fromHsv(h, 255 - k * 40, 120 + k * 30);
                Material m;
                m.name = QStringLiteral("Color %1-%2").arg(h).arg(k).toStdString();
                m.color = fromQ(c);
                colorLib_.push_back(m);
            }
        for (int k = 0; k < 9; ++k) {
            Material m;
            int v = k * 255 / 8;
            m.name = QStringLiteral("Gray %1").arg(k).toStdString();
            m.color = Color::rgb8(v, v, v);
            colorLib_.push_back(m);
        }
        std::mt19937 rng(1234);
        struct T { const char* name; QImage img; double w; };
        std::vector<T> tex = {{"Brick", makeBrick(rng), 1.0},
                              {"Wood Planks", makeWood(rng), 1.2},
                              {"Floor Tiles", makeTiles(rng), 1.2},
                              {"Grass", makeNoise(rng, QColor(92, 138, 58), 30, 2500), 2.0},
                              {"Concrete", makeNoise(rng, QColor(178, 176, 170), 14, 1800), 2.0},
                              {"Gravel", makeNoise(rng, QColor(140, 132, 120), 40, 4000), 1.0},
                              {"Water", makeNoise(rng, QColor(70, 120, 170), 20, 1500), 3.0},
                              {"Stone Wall", makeStone(rng), 1.5}};
        for (auto& t : tex) {
            Material m;
            m.name = t.name;
            m.texture = textureFromImage(t.img);
            m.texture.worldWidth = t.w;
            m.texture.worldHeight = t.w;
            m.color = averageColor(t.img);
            textureLib_.push_back(m);
        }
    }

    connect(selectTab_, &QToolButton::clicked, this, [this] { showTab(0); });
    connect(editTab_, &QToolButton::clicked, this, [this] { showTab(1); });
    connect(library_, &QComboBox::currentIndexChanged, this, [this] { rebuildList(); });
    connect(browserBtn, &QPushButton::clicked, this, [this] { emit browserRequested(); });
    connect(editor_, &MaterialEditor::popOutRequested, this, [this] { emit editorRequested(); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { chooseItem(it); });
    connect(createBtn, &QPushButton::clicked, this, [this] { createMaterial(); });
    connect(defaultBtn, &QToolButton::clicked, this, [this] {
        doc_->currentMaterial = kNone;
        refresh();
        emit materialChosen();
    });
    connect(sampleBtn, &QToolButton::clicked, this, [this] { vp_->setTool(ToolId::Paint); });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        Id id = doc_->currentMaterial;
        if (updating_ || id < 0 || id >= (Id)doc_->materials().size()) return;
        std::string n = name_->text().trimmed().toStdString();
        if (n.empty() || n == doc_->materials()[id].name) return;
        doc_->beginOp("Rename Material");
        doc_->materials()[id].name = doc_->uniqueMaterialName(n);
        ++doc_->materialsRevision;
        doc_->commitOp();
        refresh();
    });
    library_->setCurrentIndex(Architectural); // what there is to use from the start
    showTab(0);
    refresh();
}

void MaterialsPanel::showTab(int page) {
    selectTab_->setChecked(page == 0);
    editTab_->setChecked(page == 1);
    pages_->setCurrentIndex(page);
    // Only the page on show takes up height (a stack is as tall as its tallest page).
    for (int i = 0; i < pages_->count(); ++i)
        pages_->widget(i)->setSizePolicy(QSizePolicy::Preferred, i == page ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    pages_->updateGeometry();
    updateGeometry();
    if (page == 1) editor_->refresh();
}

void MaterialsPanel::showLibrary(int index) { library_->setCurrentIndex(index); }

QPixmap MaterialsPanel::swatch(const Material* m, int size) const {
    QPixmap pm(size, size);
    if (!m) {
        // Default material: front/back split like SketchUp.
        pm.fill(toQ(doc_->style.frontColor));
        QPainter p(&pm);
        QPolygon tri;
        tri << QPoint(size, 0) << QPoint(size, size) << QPoint(0, size);
        p.setBrush(toQ(doc_->style.backColor));
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
        return pm;
    }
    const QPixmap ball = materialGlobe(*m);
    if (!ball.isNull()) return ball.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    pm.fill(toQ(Color(m->color.r, m->color.g, m->color.b, 1)));
    return pm;
}

void MaterialsPanel::rebuildList() {
    list_->clear();
    const int lib = library_->currentIndex();
    auto add = [&](const QPixmap& pm, const QString& name, int index) {
        auto* it = new QListWidgetItem(QIcon(pm), QString());
        it->setToolTip(name);
        it->setData(Qt::UserRole, index);
        it->setData(Qt::UserRole + 1, lib);
        list_->addItem(it);
        return it;
    };
    if (lib == InModel) {
        add(swatch(nullptr, 58), QStringLiteral("Default"), -1);
        for (Id i = 0; i < (Id)doc_->materials().size(); ++i) {
            const Material& m = doc_->materials()[i];
            auto* it = add(swatch(&m, 58), QString::fromStdString(m.name), i);
            if (i == doc_->currentMaterial) it->setSelected(true);
        }
    } else if (lib == Architectural) {
        // Their maps take a moment: the globes come in as they get ready.
        QPixmap blank(58, 58);
        blank.fill(QColor(236, 236, 236));
        for (int i = 0; i < architecturalMaterialCount(); ++i) add(blank, QString::fromStdString(architecturalMaterialName(i)), i);
        globeTimer_->start();
    } else {
        const auto& src = lib == Colors ? colorLib_ : textureLib_;
        for (int i = 0; i < (int)src.size(); ++i) {
            QPixmap pm(58, 58);
            pm.fill(toQ(src[i].color));
            if (src[i].hasTexture()) {
                const QImage img = decodeTexture(src[i].texture);
                if (!img.isNull()) pm = QPixmap::fromImage(img.scaled(58, 58, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            add(pm, QString::fromStdString(src[i].name), i);
        }
    }
    listRevision_ = doc_->materialsRevision;
}

void MaterialsPanel::fillGlobes() {
    if (library_->currentIndex() != Architectural) {
        globeTimer_->stop();
        return;
    }
    // One per tick; architecturalMaterial() waits for a background job at most briefly.
    for (int r = 0; r < list_->count(); ++r) {
        QListWidgetItem* it = list_->item(r);
        if (it->data(Qt::UserRole + 2).toBool()) continue;
        it->setIcon(QIcon(swatch(&architecturalMaterial(it->data(Qt::UserRole).toInt()), 58)));
        it->setData(Qt::UserRole + 2, true);
        return;
    }
    globeTimer_->stop();
}

Id MaterialsPanel::ensureInModel(const Material& m) {
    for (Id i = 0; i < (Id)doc_->materials().size(); ++i)
        if (sameMaterial(doc_->materials()[i], m)) return i;
    doc_->beginOp("Add Material");
    Id id = doc_->addMaterial(m);
    doc_->commitOp();
    return id;
}

void MaterialsPanel::chooseRow(int row) { chooseItem(list_->item(row)); }

void MaterialsPanel::chooseItem(QListWidgetItem* item) {
    if (!item) return;
    const int idx = item->data(Qt::UserRole).toInt();
    const int lib = item->data(Qt::UserRole + 1).toInt();
    switch (lib) {
    case InModel: doc_->currentMaterial = idx; break;
    case Architectural: doc_->currentMaterial = ensureInModel(architecturalMaterial(idx)); break;
    case Colors: doc_->currentMaterial = ensureInModel(colorLib_[idx]); break;
    default: doc_->currentMaterial = ensureInModel(textureLib_[idx]); break;
    }
    // The active tool stays: the Texture Brush and others use this material too.
    refresh();
    emit materialChosen();
}

Id MaterialsPanel::createMaterial() {
    Material m;
    m.name = "Material";
    m.pbr = true;
    doc_->beginOp("Create Material");
    doc_->currentMaterial = doc_->addMaterial(m);
    doc_->commitOp();
    refresh();
    showTab(1);
    emit materialChosen();
    return doc_->currentMaterial;
}

Id MaterialsPanel::addImageMaterial(const QString& path) {
    Texture t;
    if (!loadTextureFile(path, t)) return kNone;
    Material m;
    m.name = QFileInfo(path).completeBaseName().toStdString();
    m.texture = t;
    m.color = averageColor(decodeTexture(t));
    doc_->beginOp("Add Material");
    Id id = doc_->addMaterial(m);
    doc_->currentMaterial = id;
    doc_->commitOp();
    library_->setCurrentIndex(InModel);
    refresh();
    return id;
}

void MaterialsPanel::refresh() {
    updating_ = true;
    Id id = doc_->currentMaterial;
    if (id >= (Id)doc_->materials().size()) id = doc_->currentMaterial = kNone;
    const Material* m = doc_->material(id);
    if (!doc_->inOp()) preview_->setPixmap(swatch(m, 76));
    if (!name_->hasFocus()) name_->setText(m ? QString::fromStdString(m->name) : QString());
    name_->setEnabled(m != nullptr);
    updating_ = false;
    if (library_->currentIndex() == InModel && !doc_->inOp()) rebuildList();
    if (pages_->currentIndex() == 1) editor_->refresh();
}

void MaterialsPanel::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) {
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile() && isImageFile(u.toLocalFile())) {
                e->acceptProposedAction();
                return;
            }
    }
    e->ignore();
}

void MaterialsPanel::dropEvent(QDropEvent* e) {
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && isImageFile(u.toLocalFile())) addImageMaterial(u.toLocalFile());
    e->acceptProposedAction();
}

// ===========================================================================
// Styles
// ===========================================================================

StylesPanel::StylesPanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    auto* faceRow = new QHBoxLayout();
    faceGroup_ = new QButtonGroup(this);
    struct F { const char* icon; const char* tip; RenderStyle::FaceStyle s; };
    const F faces[] = {{"style_wireframe", "Wireframe", RenderStyle::Wireframe},
                       {"style_hiddenline", "Hidden Line", RenderStyle::HiddenLine},
                       {"style_shaded", "Shaded", RenderStyle::Shaded},
                       {"style_textured", "Shaded With Textures", RenderStyle::ShadedTextures},
                       {"style_mono", "Monochrome", RenderStyle::Monochrome}};
    for (const F& f : faces) {
        auto* b = new QToolButton(this);
        b->setIcon(Icons::get(f.icon));
        b->setIconSize(QSize(24, 24));
        b->setToolTip(f.tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        faceGroup_->addButton(b, (int)f.s);
        faceRow->addWidget(b);
    }
    xray_ = new QToolButton(this);
    xray_->setIcon(Icons::get("style_xray"));
    xray_->setIconSize(QSize(24, 24));
    xray_->setToolTip(QStringLiteral("X-ray"));
    xray_->setCheckable(true);
    xray_->setAutoRaise(true);
    faceRow->addWidget(xray_);
    faceRow->addStretch(1);
    lay->addLayout(faceRow);

    auto* grid = new QGridLayout();
    edges_ = new QCheckBox(QStringLiteral("Edges"), this);
    backEdges_ = new QCheckBox(QStringLiteral("Back Edges"), this);
    profiles_ = new QCheckBox(QStringLiteral("Profiles"), this);
    profileW_ = new QDoubleSpinBox(this);
    profileW_->setRange(1.0, 10.0);
    profileW_->setSingleStep(0.5);
    profileW_->setDecimals(1);
    axes_ = new QCheckBox(QStringLiteral("Axes"), this);
    guides_ = new QCheckBox(QStringLiteral("Guides"), this);
    hiddenGeom_ = new QCheckBox(QStringLiteral("Hidden Geometry"), this);
    sky_ = new QCheckBox(QStringLiteral("Sky"), this);
    ground_ = new QCheckBox(QStringLiteral("Ground"), this);
    skyColor_ = new QPushButton(this);
    groundColor_ = new QPushButton(this);
    bgColor_ = new QPushButton(this);
    frontColor_ = new QPushButton(this);
    backColor_ = new QPushButton(this);
    int r = 0;
    grid->addWidget(edges_, r, 0);
    grid->addWidget(backEdges_, r++, 1);
    grid->addWidget(profiles_, r, 0);
    grid->addWidget(profileW_, r++, 1);
    grid->addWidget(axes_, r, 0);
    grid->addWidget(guides_, r++, 1);
    grid->addWidget(hiddenGeom_, r++, 0, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("Background:"), this), r, 0);
    grid->addWidget(bgColor_, r++, 1);
    grid->addWidget(sky_, r, 0);
    grid->addWidget(skyColor_, r++, 1);
    grid->addWidget(ground_, r, 0);
    grid->addWidget(groundColor_, r++, 1);
    grid->addWidget(new QLabel(QStringLiteral("Front face:"), this), r, 0);
    grid->addWidget(frontColor_, r++, 1);
    grid->addWidget(new QLabel(QStringLiteral("Back face:"), this), r, 0);
    grid->addWidget(backColor_, r++, 1);
    lay->addLayout(grid);

    connect(faceGroup_, &QButtonGroup::idClicked, this, [this] { apply(); });
    connect(xray_, &QToolButton::toggled, this, [this] { apply(); });
    for (QCheckBox* c : {edges_, backEdges_, profiles_, axes_, guides_, hiddenGeom_, sky_, ground_})
        connect(c, &QCheckBox::toggled, this, [this] { apply(); });
    connect(profileW_, &QDoubleSpinBox::valueChanged, this, [this] { apply(); });
    auto colorPick = [this](QPushButton* b, Color RenderStyle::*field, bool affectsFaces) {
        connect(b, &QPushButton::clicked, this, [this, field, affectsFaces] {
            QColor c = QColorDialog::getColor(toQ(doc_->style.*field), this);
            if (!c.isValid()) return;
            doc_->style.*field = fromQ(c);
            if (affectsFaces) ++doc_->materialsRevision;
            refresh();
            vp_->update();
            emit styleChanged();
        });
    };
    colorPick(skyColor_, &RenderStyle::skyColor, false);
    colorPick(groundColor_, &RenderStyle::groundColor, false);
    colorPick(bgColor_, &RenderStyle::background, false);
    colorPick(frontColor_, &RenderStyle::frontColor, true);
    colorPick(backColor_, &RenderStyle::backColor, true);
    refresh();
}

void StylesPanel::apply() {
    if (updating_) return;
    RenderStyle& s = doc_->style;
    s.faceStyle = (RenderStyle::FaceStyle)faceGroup_->checkedId();
    s.xray = xray_->isChecked();
    s.edges = edges_->isChecked();
    s.backEdges = backEdges_->isChecked();
    s.profiles = profiles_->isChecked();
    s.profileWidth = (float)profileW_->value();
    s.showAxes = axes_->isChecked();
    s.showGuides = guides_->isChecked();
    s.showHidden = hiddenGeom_->isChecked();
    s.sky = sky_->isChecked();
    s.ground = ground_->isChecked();
    vp_->update();
    emit styleChanged();
}

void StylesPanel::refresh() {
    updating_ = true;
    const RenderStyle& s = doc_->style;
    if (auto* b = faceGroup_->button((int)s.faceStyle)) b->setChecked(true);
    xray_->setChecked(s.xray);
    edges_->setChecked(s.edges);
    backEdges_->setChecked(s.backEdges);
    profiles_->setChecked(s.profiles);
    profileW_->setValue(s.profileWidth);
    axes_->setChecked(s.showAxes);
    guides_->setChecked(s.showGuides);
    hiddenGeom_->setChecked(s.showHidden);
    sky_->setChecked(s.sky);
    ground_->setChecked(s.ground);
    setButtonColor(skyColor_, s.skyColor);
    setButtonColor(groundColor_, s.groundColor);
    setButtonColor(bgColor_, s.background);
    setButtonColor(frontColor_, s.frontColor);
    setButtonColor(backColor_, s.backColor);
    updating_ = false;
}

// ===========================================================================
// Sun
// ===========================================================================

SunPanel::SunPanel(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent), doc_(doc), vp_(vp) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(8, 6, 8, 8);
    sun_ = new QCheckBox(QStringLiteral("Sun"), this);
    shadows_ = new QCheckBox(QStringLiteral("Shadows"), this);
    auto* checks = new QHBoxLayout();
    checks->addWidget(sun_);
    checks->addWidget(shadows_);
    cinematic_ = new QCheckBox(QStringLiteral("Cinematic"), this);
    cinematic_->setToolTip(QStringLiteral("Ray traced soft shadows from the sun and every light, and light rays in the air."));
    checks->addWidget(cinematic_);
    checks->addStretch(1);
    form->addRow(checks);
    auto row = [&](const QString& label, const QString& tip, double lo, double hi, QSlider*& slider, QDoubleSpinBox*& spin, const QString& suffix, int scale) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        slider = new QSlider(Qt::Horizontal, w);
        slider->setRange(int(lo * scale), int(hi * scale));
        spin = new QDoubleSpinBox(w);
        spin->setRange(lo, hi);
        spin->setDecimals(scale > 1 ? 2 : 0);
        spin->setSuffix(suffix);
        h->addWidget(slider, 1);
        h->addWidget(spin);
        w->setToolTip(tip);
        form->addRow(label, w);
        QDoubleSpinBox* sp = spin;
        QSlider* sl = slider;
        connect(sl, &QSlider::valueChanged, sp, [sp, scale](int v) { sp->setValue(double(v) / scale); });
        connect(sp, &QDoubleSpinBox::valueChanged, this, [this, sl, scale](double v) {
            QSignalBlocker b(sl);
            sl->setValue((int)std::lround(v * scale));
            apply();
        });
    };
    QSlider* a; QSlider* e; QSlider* i;
    row(QStringLiteral("Direction:"), QStringLiteral("Compass direction of the sun: 0° along the green axis, 90° along the red axis."), 0, 360, a, azimuth_, QStringLiteral("°"), 1);
    row(QStringLiteral("Height:"), QStringLiteral("Height of the sun above the horizon: low for evening light, below 0° for night."), -30, 90, e, altitude_, QStringLiteral("°"), 1);
    row(QStringLiteral("Brightness:"), QStringLiteral("Strength of the direct sunlight."), 0, 2, i, intensity_, QString(), 100);
    QSlider* rs;
    row(QStringLiteral("Light rays:"), QStringLiteral("Cinematic: how much the air glows in the light (sun shafts, beams of lamps). 0 = none."), 0, 1, rs, rays_, QString(), 100);
    connect(sun_, &QCheckBox::toggled, this, [this] { apply(); });
    connect(cinematic_, &QCheckBox::toggled, this, [this] { apply(); });
    connect(shadows_, &QCheckBox::toggled, this, [this] { apply(); });
    refresh();
}

void SunPanel::apply() {
    if (updating_) return;
    RenderStyle& s = doc_->style;
    s.sun = sun_->isChecked();
    s.shadows = shadows_->isChecked();
    s.cinematic = cinematic_->isChecked();
    s.lightRays = rays_->value();
    s.sunAzimuth = azimuth_->value();
    s.sunAltitude = altitude_->value();
    s.sunIntensity = intensity_->value();
    vp_->update();
    emit styleChanged();
}

void SunPanel::refresh() {
    updating_ = true;
    const RenderStyle& s = doc_->style;
    sun_->setChecked(s.sun);
    shadows_->setChecked(s.shadows);
    cinematic_->setChecked(s.cinematic);
    rays_->setValue(s.lightRays);
    azimuth_->setValue(s.sunAzimuth);
    altitude_->setValue(s.sunAltitude);
    intensity_->setValue(s.sunIntensity);
    updating_ = false;
}

// ===========================================================================
// Terraform
// ===========================================================================

TerrainPanel::TerrainPanel(Document* doc, Viewport* vp, const std::vector<QAction*>& tools, QWidget* parent)
    : QWidget(parent), doc_(doc), vp_(vp) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 8);
    auto* row = new QHBoxLayout();
    for (QAction* a : tools) {
        auto* b = new QToolButton(this);
        b->setDefaultAction(a);
        b->setIconSize(QSize(24, 24));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setAutoRaise(true);
        row->addWidget(b);
    }
    row->addStretch(1);
    lay->addLayout(row);

    auto* fl = new QFormLayout();
    radius_ = new QLineEdit(this);
    radius_->setToolTip(QStringLiteral("Brush radius (shown as a circle under the cursor)"));
    force_ = new QDoubleSpinBox(this);
    force_->setRange(0.1, 10000);
    force_->setDecimals(1);
    force_->setSuffix(QStringLiteral(" cm/s"));
    force_->setToolTip(QStringLiteral("How far the centre of the brush moves per second of holding"));
    hardness_ = new QSlider(Qt::Horizontal, this);
    hardness_->setRange(0, 100);
    hardness_->setToolTip(QStringLiteral("0% = soft rounded hill, 100% = flat plateau with steep sides"));
    hardnessValue_ = new QLabel(this);
    hardnessValue_->setMinimumWidth(36);
    auto* hl = new QHBoxLayout();
    hl->addWidget(hardness_, 1);
    hl->addWidget(hardnessValue_);
    fl->addRow(QStringLiteral("Radius:"), radius_);
    fl->addRow(QStringLiteral("Force:"), force_);
    density_ = new QDoubleSpinBox(this);
    density_->setRange(0, 1000);
    density_->setDecimals(1);
    density_->setSingleStep(1);
    density_->setSuffix(QStringLiteral(" / m"));
    density_->setSpecialValueText(QStringLiteral("Auto"));
    density_->setToolTip(QStringLiteral("Most triangle edges per meter: triangles never get smaller than 1 m divided by "
                                        "this, for a lighter surface. Auto = brush radius ÷ 8."));
    fl->addRow(QStringLiteral("Hardness:"), hl);
    fl->addRow(QStringLiteral("Max density:"), density_);
    lay->addLayout(fl);

    connect(radius_, &QLineEdit::editingFinished, this, [this] { apply(); });
    connect(force_, &QDoubleSpinBox::valueChanged, this, [this] { apply(); });
    connect(hardness_, &QSlider::valueChanged, this, [this] { apply(); });
    connect(density_, &QDoubleSpinBox::valueChanged, this, [this] { apply(); });
    connect(&TerrainSettings::instance(), &TerrainSettings::changed, this, [this] { refresh(); });
    refresh();
}

void TerrainPanel::refresh() {
    updating_ = true;
    const TerrainBrush& b = TerrainSettings::instance().brush();
    if (!radius_->hasFocus()) radius_->setText(QString::fromStdString(doc_->units.format(b.radius)));
    force_->setValue(b.force * 100);
    hardness_->setValue((int)std::lround(b.hardness * 100));
    hardnessValue_->setText(QStringLiteral("%1%").arg(hardness_->value()));
    density_->setValue(b.density);
    updating_ = false;
}

void TerrainPanel::apply() {
    if (updating_) return;
    TerrainBrush b = TerrainSettings::instance().brush();
    double r;
    if (doc_->units.parseLength(radius_->text().toStdString(), r) && r > 0) b.radius = r;
    b.force = force_->value() / 100;
    b.hardness = hardness_->value() / 100.0;
    b.density = density_->value();
    TerrainSettings::instance().setBrush(b);
    vp_->refreshToolStatus();
    vp_->update();
}

// ===========================================================================
// Instructor
// ===========================================================================

InstructorPanel::InstructorPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    text_ = new QTextBrowser(this);
    text_->setMinimumHeight(180);
    text_->setOpenLinks(false);
    lay->addWidget(text_);
}

void InstructorPanel::showTool(ToolId id) {
    struct Info { const char* title; const char* what; const char* steps; const char* keys; };
    Info i{};
    switch (id) {
    case ToolId::Select: i = {"Select", "Selects entities to modify with other tools or commands.",
        "Click an entity. Drag left-to-right for a window selection (fully inside), right-to-left for a crossing selection (touching). Double-click a face to add its edges, triple-click for everything connected.",
        "Ctrl: add &nbsp; Shift: toggle &nbsp; Ctrl+Shift: subtract &nbsp; Ctrl+A: select all &nbsp; Ctrl+T: select none"}; break;
    case ToolId::Eraser: i = {"Eraser", "Erases edges. Erasing an edge between two coplanar faces merges them.",
        "Click edges, or press and drag over several edges and release.", "Shift: hide &nbsp; Ctrl: soften/smooth &nbsp; Ctrl+Shift: unsoften"}; break;
    case ToolId::Paint: i = {"Paint Bucket", "Applies the current material to faces.",
        "Choose a material in the Materials panel, then click faces. You can also drag an image file from your file manager straight onto a face: it becomes a texture fitted to that face.",
        "Ctrl: fill connected faces &nbsp; Shift: replace material everywhere &nbsp; Alt: sample material &nbsp; Ctrl while dropping an image: tile at real-world size"}; break;
    case ToolId::Line: i = {"Line", "Draws edges. Edges that close a planar loop create a face automatically; edges drawn across a face split it.",
        "Click the start point, click the end point. Keep clicking to continue the chain. Type a length and press Enter at any time.",
        "Right/Left/Up arrows: lock red/green/blue axis &nbsp; Down: lock parallel &nbsp; Shift: lock current inference &nbsp; Esc: finish. Type &lt;x,y,z&gt; for relative or [x,y,z] for absolute coordinates."}; break;
    case ToolId::Freehand: i = {"Freehand", "Draws a curve of short edges.", "Press, drag and release. End near the start to close the loop.", ""}; break;
    case ToolId::Rectangle: i = {"Rectangle", "Draws a rectangular face.", "Click the first corner, then the opposite corner. Type width,height and press Enter for exact dimensions.",
        "Arrow keys choose the drawing plane: Right = perpendicular to red, Left = green, Up = blue (ground). The pink diagonal shows a square or golden section."}; break;
    case ToolId::Circle: i = {"Circle", "Draws a circle made of edges (one curve).", "Click the center, then a point on the circumference, or type the radius. Type e.g. 48s to change the number of segments.", "Arrow keys choose the plane."}; break;
    case ToolId::Polygon: i = {"Polygon", "Draws a regular polygon.", "Click the center, then the radius. Type e.g. 8s for an octagon.", "Arrow keys choose the plane."}; break;
    case ToolId::Arc: i = {"2 Point Arc", "Draws an arc through two end points with a bulge.", "Click start, click end, then move to set the bulge and click. Type a value for the bulge, or e.g. 24s for segments.", "A half circle snaps automatically."}; break;
    case ToolId::PushPull: i = {"Push/Pull", "Extrudes a face along its normal, creating side faces.",
        "Click a face, move, click again (or drag). Type an exact distance. Hover over another point to push/pull to its height. After finishing, type a new value to adjust.",
        "Ctrl: keep the starting face &nbsp; Double-click: repeat last distance"}; break;
    case ToolId::Move: i = {"Move", "Moves or copies the selection. Connected geometry stretches.",
        "Select entities, click a base point, then the destination. Type a distance for exactness.",
        "Ctrl: toggle copy &nbsp; After copying type 5x for an array of 5 or /5 to divide &nbsp; Arrow keys: lock axes"}; break;
    case ToolId::Rotate: i = {"Rotate", "Rotates or copies the selection around an axis.",
        "Click the center, click a reference direction, then the target angle. Snaps every 15°. Type degrees for exactness.",
        "Arrow keys: lock the rotation plane &nbsp; Ctrl: copy (then 6x for a polar array)"}; break;
    case ToolId::Scale: i = {"Scale", "Scales the selection about its center.", "Click to start, move to scale, click to finish. Type a factor such as 2, 0.5 or 1,1,2.", ""}; break;
    case ToolId::Offset: i = {"Offset", "Creates copies of a face's edges at a uniform distance.", "Click a face, move inward or outward, click. Type a distance.", "Double-click: repeat last distance"}; break;
    case ToolId::FollowMe: i = {"Follow Me", "Sweeps a profile face along a path: pipes, beams, mouldings and lathed shapes.",
        "Select the path edges (or a face for its outline), then click the profile face. Or click the profile first, then click a path edge to follow its connected line, arc or circle.",
        "Alt: follow the outline of the clicked face &nbsp; Esc: cancel"}; break;
    case ToolId::Tape: i = {"Tape Measure", "Measures distances and creates guides.",
        "Click two points to measure. Starting on an edge creates a parallel guide line; starting on a point creates a guide point. Double-click an edge for an infinite guide. Type a value after measuring to resize the whole model.",
        "Ctrl: measure without creating guides"}; break;
    case ToolId::Orbit: i = {"Orbit", "Rotates the camera around the geometry under the cursor (or the selection, or the model centre).", "Drag in the viewport. The middle mouse button orbits with any tool. Camera > Orbit Mode switches to orbiting around the camera target.", "Shift: pan"}; break;
    case ToolId::Pan: i = {"Pan", "Moves the camera sideways.", "Drag in the viewport. Shift + middle mouse pans with any tool.", ""}; break;
    case ToolId::Zoom: i = {"Zoom", "Moves the camera closer or farther.", "Drag up/down, or use the mouse wheel with any tool. Type a field of view in degrees.", "Shift + drag: change field of view"}; break;
    case ToolId::ZoomWindow: i = {"Zoom Window", "Zooms to a rectangular area.", "Drag a window around the area of interest.", ""}; break;
    case ToolId::TexturePosition: i = {"Position Texture", "Moves, resizes, rotates, skews and mirrors the texture of the selected faces on the model, in real time.",
        "Select textured faces (or click one). The orange outline is one repeat of the image. Drag inside to move, a corner to resize (it snaps to the corners of the face), a side square to stretch, a diamond to skew, the circle to rotate. Drag a corner across to mirror. All selected faces change together. To type a value, click the handle instead of dragging it, then type.",
        "Corner: Shift = free proportions, Ctrl = about the centre &nbsp; Rotate snaps every 15° &nbsp; Type: 45 (angle), 2 or 2,1 (factor), 0.5,0.2 (distance) &nbsp; Esc: cancel &nbsp; Texture: Fit / Reset in the right-click menu"}; break;
    case ToolId::Bend: i = {"Bend", "Bends any geometry around an axis, like a strip of soft metal. Before the bend nothing moves; after it everything follows rigidly.",
        "Select what to bend (or click it). Click the start of the bend, then its end. Move the mouse or type the angle, then click. Curves stay as flat pieces wherever they can, so stair treads stay one face each.",
        "Right / Left / Up arrow: bend around the red / green / blue axis (default blue) &nbsp; Type 90: angle in degrees, then Enter &nbsp; Type 24s: number of segments &nbsp; Esc: cancel"}; break;
    case ToolId::PlacePointLight: i = {"Point Light", "Places a light that shines in all directions, like a bare bulb.",
        "Click a surface or the ground: the light goes above that point. Adjust brightness, colour and range in Window &gt; Scene; move it with the Move tool.",
        "Type a value: height above the surface"}; break;
    case ToolId::PlaceSpotLight: i = {"Spot Light", "Places a light that shines in a cone.",
        "Click where the light goes, then the point it aims at. Cone angle and softness are in Window &gt; Scene.",
        "Type a value: height above the surface &nbsp; Esc: cancel"}; break;
    case ToolId::PlaceDirectionalLight: i = {"Directional Light", "Adds parallel light from one direction, like a second sun. It does not fade with distance.",
        "Click where the marker goes, then the point it aims at. Only the direction matters.", "Type a value: height above the surface &nbsp; Esc: cancel"}; break;
    case ToolId::PlaceCamera: i = {"Place Camera", "Puts a camera in the world to come back to.",
        "Click where it stands, then what it looks at. Double-click its marker, or use Window &gt; Scene, to look through it. Camera &gt; Add Camera From View saves the current view instead.",
        "Type a value: height above the surface &nbsp; Esc: cancel"}; break;
    case ToolId::PlaceCharacter: i = {"Game Mode", "Puts a character on the model and walks it around in a separate game window. It is 1.80 m tall and 0.30 m across, and collides with every visible face.",
        "Click the spot to stand on; the green capsule shows the character. In the game window click to capture the mouse, then walk. The game works on a copy of the model: to see later edits, close it and place the character again.",
        "WASD: walk &nbsp; Mouse or arrow keys: look &nbsp; Shift: run &nbsp; Space: jump &nbsp; R: back to the start &nbsp; Tab: edges &nbsp; F11: full screen &nbsp; Esc: free the mouse, then leave"}; break;
    case ToolId::Mountain: i = {"Mountain", "Raises the surface like a 3D brush. Faces under the brush turn into a smooth triangle mesh that keeps subdividing as it stretches.",
        "Press and hold on a face; the longer you hold, the higher it grows (Force = cm per second). Drag to paint along a ridge. The circle shows the radius, the dashed circle the hard core.",
        "Shift: vertical axis only &nbsp; Ctrl+wheel: radius &nbsp; Alt+wheel: hardness &nbsp; Type a value to set the radius &nbsp; Settings: Window &gt; Terraform &nbsp; Esc: cancel the stroke"}; break;
    case ToolId::Pit: i = {"Pit", "Sinks the surface under the brush: the opposite of Mountain.",
        "Press and hold on a face; drag to dig a trench.", "Shift: vertical axis only &nbsp; Ctrl+wheel: radius &nbsp; Alt+wheel: hardness &nbsp; Type a value to set the radius &nbsp; Esc: cancel the stroke"}; break;
    case ToolId::TextureBrush: i = {"Texture Brush", "Paints the material chosen in the Materials panel over any surface, on top of the textures already there, so materials blend into each other with a soft edge.",
        "Choose a material (colour or texture) in Materials, then press and hold on a surface and drag. Paint more of another material to mix it in; choose the Default material to erase painted textures. Faces under the brush are subdivided into a fine smooth mesh to carry the paint.",
        "Ctrl+wheel: radius &nbsp; Alt+wheel: hardness &nbsp; Type a value to set the radius &nbsp; Esc: cancel the stroke"}; break;
    case ToolId::SolidUnion: i = {"Union", "Combines two solid components into one and removes the faces inside.",
        "Click the first solid, then the second. Or select several solids and pick the tool to join them all.",
        "A solid is a component whose faces close up with no holes. Esc: start over"}; break;
    case ToolId::SolidIntersect: i = {"Intersect", "Keeps only the volume two solid components share.",
        "Click the first solid, then the second. Or select several solids and pick the tool.", "Esc: start over"}; break;
    case ToolId::SolidSubtract: i = {"Subtract", "Cuts the first solid out of the second and removes the first: holes, pockets and notches.",
        "Click the solid that cuts (or select it before picking the tool), then the solid to cut it out of.", "Esc: start over"}; break;
    case ToolId::SolidTrim: i = {"Trim", "Cuts the first solid out of the second but keeps the first, e.g. to fit a part into another.",
        "Click the solid that cuts (or select it before picking the tool), then the solid to trim.", "Esc: start over"}; break;
    case ToolId::SolidSplit: i = {"Split", "Splits two overlapping solids into three components: each one's own part and the part they share.",
        "Click the first solid, then the second.", "Esc: start over"}; break;
    case ToolId::Flatten: i = {"Flatten", "Levels the surface around the brush to the height of the point where you start.",
        "Press on the height you want to keep, hold and drag over the bumps to flatten them.", "Shift: vertical axis only &nbsp; Ctrl+wheel: radius &nbsp; Alt+wheel: hardness &nbsp; Type a value to set the radius &nbsp; Esc: cancel the stroke"}; break;
    }
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p><p><b>Tool operation</b><br>%3</p>").arg(i.title, i.what, i.steps);
    if (i.keys && *i.keys) html += QStringLiteral("<p><b>Modifier keys</b><br>%1</p>").arg(i.keys);
    text_->setHtml(html);
}

} // namespace gs
