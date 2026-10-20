// SPDX-License-Identifier: GPL-3.0-or-later
#include "Panels.h"

#include "Icons.h"
#include "Viewport.h"
#include "core/Components.h"
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
#include <QSlider>
#include <QStackedWidget>
#include <QTextBrowser>
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
// Tray
// ===========================================================================

TraySection::TraySection(const QString& title, QWidget* content, QWidget* parent) : QWidget(parent), content_(content) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    header_ = new QToolButton(this);
    header_->setText(title);
    header_->setCheckable(true);
    header_->setChecked(true);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setArrowType(Qt::DownArrow);
    header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header_->setObjectName("TrayHeader");
    lay->addWidget(header_);
    lay->addWidget(content_);
    connect(header_, &QToolButton::toggled, this, [this](bool on) { setExpanded(on); });
}

void TraySection::setExpanded(bool on) {
    header_->blockSignals(true);
    header_->setChecked(on);
    header_->blockSignals(false);
    header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    content_->setVisible(on);
}

bool TraySection::expanded() const { return header_->isChecked(); }

Tray::Tray(QWidget* parent) : QWidget(parent) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(2, 2, 2, 2);
    layout_->setSpacing(2);
    layout_->addStretch(1);
}

TraySection* Tray::addSection(const QString& title, QWidget* content, bool expanded) {
    auto* s = new TraySection(title, content, this);
    layout_->insertWidget(layout_->count() - 1, s);
    s->setExpanded(expanded);
    return s;
}

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
    } else if (!s.instances.empty() && s.faces.empty() && s.edges.empty()) {
        const ModelState& st = doc_->state;
        Id def = st.instances[*s.instances.begin()].def;
        bool same = true;
        for (Id i : s.instances) same = same && st.instances[i].def == def;
        title_->setText(s.instances.size() == 1 ? QStringLiteral("Component")
                                                : QStringLiteral("%1 Components").arg(s.instances.size()));
        if (same) {
            measureLabel_->setText(QString::fromStdString(st.defs[def].name));
            measure_->setText(QStringLiteral("%1 in model").arg(instanceCount(st, def)));
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
    lay->setSpacing(4);

    auto* top = new QHBoxLayout();
    preview_ = new QLabel(this);
    preview_->setFixedSize(64, 64);
    preview_->setFrameShape(QFrame::Box);
    top->addWidget(preview_);
    auto* nameCol = new QVBoxLayout();
    name_ = new QLineEdit(this);
    name_->setPlaceholderText(QStringLiteral("Default"));
    nameCol->addWidget(name_);
    nameCol->addStretch(1);
    top->addLayout(nameCol, 1);
    auto* btnCol = new QVBoxLayout();
    auto* createBtn = new QToolButton(this);
    createBtn->setIcon(Icons::get("new"));
    createBtn->setToolTip(QStringLiteral("Create Material"));
    auto* defaultBtn = new QToolButton(this);
    defaultBtn->setIcon(Icons::get("delete"));
    defaultBtn->setToolTip(QStringLiteral("Paint with Default material"));
    auto* sampleBtn = new QToolButton(this);
    sampleBtn->setIcon(Icons::get("paint"));
    sampleBtn->setToolTip(QStringLiteral("Paint Bucket (Alt+click in the model samples a material)"));
    btnCol->addWidget(createBtn);
    btnCol->addWidget(defaultBtn);
    btnCol->addWidget(sampleBtn);
    btnCol->addStretch(1);
    top->addLayout(btnCol);
    lay->addLayout(top);

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
    library_->addItems({QStringLiteral("In Model"), QStringLiteral("Colors"), QStringLiteral("Textures")});
    sl->addWidget(library_);
    list_ = new QListWidget(sel);
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(44, 44));
    list_->setGridSize(QSize(50, 50));
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setUniformItemSizes(true);
    list_->setSpacing(2);
    list_->setMinimumHeight(170);
    sl->addWidget(list_);
    pages_->addWidget(sel);
    // --- Edit page
    auto* ed = new QWidget(pages_);
    auto* fl = new QFormLayout(ed);
    fl->setContentsMargins(0, 4, 0, 0);
    colorBtn_ = new QPushButton(QStringLiteral("Color…"), ed);
    fl->addRow(QStringLiteral("Color:"), colorBtn_);
    opacity_ = new QSlider(Qt::Horizontal, ed);
    opacity_->setRange(0, 100);
    fl->addRow(QStringLiteral("Opacity:"), opacity_);
    useTexture_ = new QCheckBox(QStringLiteral("Use texture image"), ed);
    browse_ = new QPushButton(QStringLiteral("Browse…"), ed);
    auto* trow = new QHBoxLayout();
    trow->addWidget(useTexture_);
    trow->addWidget(browse_);
    fl->addRow(trow);
    texW_ = new QLineEdit(ed);
    texH_ = new QLineEdit(ed);
    lockAspect_ = new QCheckBox(QStringLiteral("Lock aspect"), ed);
    lockAspect_->setChecked(true);
    fl->addRow(QStringLiteral("Tile width:"), texW_);
    fl->addRow(QStringLiteral("Tile height:"), texH_);
    fl->addRow(lockAspect_);
    pages_->addWidget(ed);
    lay->addWidget(pages_, 1);

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

    connect(selectTab_, &QToolButton::clicked, this, [this] {
        selectTab_->setChecked(true);
        editTab_->setChecked(false);
        pages_->setCurrentIndex(0);
    });
    connect(editTab_, &QToolButton::clicked, this, [this] {
        editTab_->setChecked(true);
        selectTab_->setChecked(false);
        pages_->setCurrentIndex(1);
        refreshEditor();
    });
    connect(library_, &QComboBox::currentIndexChanged, this, [this] { rebuildList(); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { chooseItem(it); });
    connect(createBtn, &QToolButton::clicked, this, [this] { createMaterial(); });
    connect(defaultBtn, &QToolButton::clicked, this, [this] {
        doc_->currentMaterial = kNone;
        vp_->setTool(ToolId::Paint);
        refresh();
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
    connect(colorBtn_, &QPushButton::clicked, this, [this] { setColorFromDialog(); });
    connect(opacity_, &QSlider::sliderReleased, this, [this] {
        Id id = doc_->currentMaterial;
        if (updating_ || id < 0 || id >= (Id)doc_->materials().size()) return;
        doc_->beginOp("Material Opacity");
        doc_->materials()[id].opacity = opacity_->value() / 100.0f;
        ++doc_->materialsRevision;
        doc_->commitOp();
        vp_->update();
    });
    connect(useTexture_, &QCheckBox::toggled, this, [this](bool on) {
        Id id = doc_->currentMaterial;
        if (updating_ || id < 0 || id >= (Id)doc_->materials().size()) return;
        if (on && !doc_->materials()[id].hasTexture()) {
            browseTexture();
        } else if (!on && doc_->materials()[id].hasTexture()) {
            doc_->beginOp("Remove Texture");
            doc_->materials()[id].texture = Texture();
            ++doc_->materialsRevision;
            doc_->commitOp();
            refresh();
            vp_->update();
        }
    });
    connect(browse_, &QPushButton::clicked, this, [this] { browseTexture(); });
    connect(texW_, &QLineEdit::editingFinished, this, [this] { applyTextureSize(true); });
    connect(texH_, &QLineEdit::editingFinished, this, [this] { applyTextureSize(false); });
    refresh();
}

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
    if (m->hasTexture()) {
        QImage img = decodeTexture(m->texture);
        if (!img.isNull()) return QPixmap::fromImage(img.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
    pm.fill(toQ(Color(m->color.r, m->color.g, m->color.b, 1)));
    if (m->opacity < 0.999f) {
        QPainter p(&pm);
        p.setPen(QPen(Qt::white, 2));
        p.drawLine(0, size, size, 0);
    }
    return pm;
}

void MaterialsPanel::rebuildList() {
    list_->clear();
    int lib = library_->currentIndex();
    if (lib == 0) {
        auto* d = new QListWidgetItem(QIcon(swatch(nullptr, 44)), QString());
        d->setToolTip(QStringLiteral("Default"));
        d->setData(Qt::UserRole, -1);
        d->setData(Qt::UserRole + 1, 0);
        list_->addItem(d);
        for (Id i = 0; i < (Id)doc_->materials().size(); ++i) {
            const Material& m = doc_->materials()[i];
            auto* it = new QListWidgetItem(QIcon(swatch(&m, 44)), QString());
            it->setToolTip(QString::fromStdString(m.name));
            it->setData(Qt::UserRole, i);
            it->setData(Qt::UserRole + 1, 0);
            list_->addItem(it);
            if (i == doc_->currentMaterial) it->setSelected(true);
        }
    } else {
        const auto& src = lib == 1 ? colorLib_ : textureLib_;
        for (int i = 0; i < (int)src.size(); ++i) {
            auto* it = new QListWidgetItem(QIcon(swatch(&src[i], 44)), QString());
            it->setToolTip(QString::fromStdString(src[i].name));
            it->setData(Qt::UserRole, i);
            it->setData(Qt::UserRole + 1, lib);
            list_->addItem(it);
        }
    }
    listRevision_ = doc_->materialsRevision;
}

Id MaterialsPanel::ensureInModel(const Material& m) {
    for (Id i = 0; i < (Id)doc_->materials().size(); ++i) {
        const Material& e = doc_->materials()[i];
        if (e.name == m.name && e.texture.data == m.texture.data) return i;
    }
    doc_->beginOp("Add Material");
    Id id = doc_->addMaterial(m);
    doc_->commitOp();
    return id;
}

void MaterialsPanel::chooseItem(QListWidgetItem* item) {
    if (!item) return;
    int idx = item->data(Qt::UserRole).toInt();
    int lib = item->data(Qt::UserRole + 1).toInt();
    if (lib == 0) doc_->currentMaterial = idx;
    else doc_->currentMaterial = ensureInModel(lib == 1 ? colorLib_[idx] : textureLib_[idx]);
    if (vp_->toolId() != ToolId::Paint) vp_->setTool(ToolId::Paint);
    refresh();
    emit materialChosen();
}

void MaterialsPanel::createMaterial() {
    QColor c = QColorDialog::getColor(QColor(200, 200, 200), this, QStringLiteral("Create Material"));
    if (!c.isValid()) return;
    Material m;
    m.name = "Material";
    m.color = fromQ(c);
    doc_->beginOp("Create Material");
    doc_->currentMaterial = doc_->addMaterial(m);
    doc_->commitOp();
    library_->setCurrentIndex(0);
    refresh();
}

void MaterialsPanel::setColorFromDialog() {
    Id id = doc_->currentMaterial;
    if (id < 0 || id >= (Id)doc_->materials().size()) return;
    QColor c = QColorDialog::getColor(toQ(doc_->materials()[id].color), this, QStringLiteral("Material Color"));
    if (!c.isValid()) return;
    doc_->beginOp("Material Color");
    Material& m = doc_->materials()[id];
    m.color = fromQ(c);
    if (m.hasTexture()) m.texture = Texture(); // colourising textures is not supported: switch to colour
    ++doc_->materialsRevision;
    doc_->commitOp();
    refresh();
    vp_->update();
}

void MaterialsPanel::browseTexture() {
    Id id = doc_->currentMaterial;
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose Texture Image"), QString(),
                                                QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tga *.tif *.tiff)"));
    if (path.isEmpty()) {
        refresh();
        return;
    }
    if (id < 0 || id >= (Id)doc_->materials().size()) {
        Id nid = addImageMaterial(path);
        if (nid != kNone) doc_->currentMaterial = nid;
        refresh();
        return;
    }
    Texture t;
    QString err;
    if (!loadTextureFile(path, t, &err)) {
        refresh();
        return;
    }
    doc_->beginOp("Set Texture");
    Material& m = doc_->materials()[id];
    m.texture = t;
    m.color = averageColor(decodeTexture(t));
    ++doc_->materialsRevision;
    doc_->commitOp();
    refresh();
    vp_->update();
}

void MaterialsPanel::applyTextureSize(bool fromWidth) {
    Id id = doc_->currentMaterial;
    if (updating_ || id < 0 || id >= (Id)doc_->materials().size() || !doc_->materials()[id].hasTexture()) return;
    Material& m = doc_->materials()[id];
    double w = m.texture.worldWidth, h = m.texture.worldHeight;
    double v;
    if (!doc_->units.parseLength((fromWidth ? texW_ : texH_)->text().toStdString(), v) || v <= 0) {
        refreshEditor();
        return;
    }
    double aspect = h / std::max(w, 1e-9);
    if (fromWidth) {
        w = v;
        if (lockAspect_->isChecked()) h = w * aspect;
    } else {
        h = v;
        if (lockAspect_->isChecked()) w = h / aspect;
    }
    if (std::fabs(w - m.texture.worldWidth) < 1e-12 && std::fabs(h - m.texture.worldHeight) < 1e-12) return;
    doc_->beginOp("Texture Size");
    doc_->materials()[id].texture.worldWidth = w;
    doc_->materials()[id].texture.worldHeight = h;
    ++doc_->materialsRevision;
    doc_->commitOp();
    refreshEditor();
    vp_->update();
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
    library_->setCurrentIndex(0);
    refresh();
    return id;
}

void MaterialsPanel::refreshEditor() {
    updating_ = true;
    Id id = doc_->currentMaterial;
    bool valid = id >= 0 && id < (Id)doc_->materials().size();
    for (QWidget* w : std::initializer_list<QWidget*>{colorBtn_, opacity_, useTexture_, browse_, texW_, texH_, lockAspect_}) w->setEnabled(valid);
    if (valid) {
        const Material& m = doc_->materials()[id];
        setButtonColor(colorBtn_, m.color);
        opacity_->setValue(int(std::lround(m.opacity * 100)));
        useTexture_->setChecked(m.hasTexture());
        texW_->setEnabled(m.hasTexture());
        texH_->setEnabled(m.hasTexture());
        texW_->setText(m.hasTexture() ? QString::fromStdString(doc_->units.format(m.texture.worldWidth)) : QString());
        texH_->setText(m.hasTexture() ? QString::fromStdString(doc_->units.format(m.texture.worldHeight)) : QString());
    } else {
        useTexture_->setChecked(false);
        texW_->clear();
        texH_->clear();
    }
    updating_ = false;
}

void MaterialsPanel::refresh() {
    updating_ = true;
    Id id = doc_->currentMaterial;
    if (id >= (Id)doc_->materials().size()) id = doc_->currentMaterial = kNone;
    const Material* m = doc_->material(id);
    preview_->setPixmap(swatch(m, 62));
    name_->setText(m ? QString::fromStdString(m->name) : QString());
    name_->setEnabled(m != nullptr);
    updating_ = false;
    if (library_->currentIndex() == 0 || listRevision_ != doc_->materialsRevision) rebuildList();
    refreshEditor();
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
    vp_->setTool(ToolId::Paint);
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
    }
    QString html = QStringLiteral("<h3>%1</h3><p>%2</p><p><b>Tool operation</b><br>%3</p>").arg(i.title, i.what, i.steps);
    if (i.keys && *i.keys) html += QStringLiteral("<p><b>Modifier keys</b><br>%1</p>").arg(i.keys);
    text_->setHtml(html);
}

} // namespace gs
