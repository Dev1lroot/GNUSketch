// SPDX-License-Identifier: GPL-3.0-or-later
#include "MaterialEditor.h"

#include "Viewport.h"
#include "io/AssetLibrary.h"
#include "io/ImageUtil.h"
#include "render/OffscreenRenderer.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace gs {

namespace {

const QString kImageFilter = QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tga *.tif *.tiff)");

void setButtonColor(QPushButton* b, const Color& c) {
    QPixmap pm(120, 14);
    pm.fill(QColor::fromRgbF(std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f)));
    QPainter p(&pm);
    p.setPen(QColor(120, 120, 120));
    p.drawRect(0, 0, 119, 13);
    b->setIconSize(pm.size());
    b->setIcon(QIcon(pm));
}

Color fromQ(const QColor& c) { return Color((float)c.redF(), (float)c.greenF(), (float)c.blueF()); }

} // namespace

Material* MaterialEditor::material() {
    const Id id = doc_->currentMaterial;
    return id >= 0 && id < (Id)doc_->materials().size() ? &doc_->materials()[id] : nullptr;
}

void MaterialEditor::apply(const char* op, const std::function<void(Material&)>& fn, bool structural) {
    if (updating_ || !material()) return;
    doc_->beginOp(op);
    Material& m = *material();
    const bool wasAdvanced = m.advanced();
    m.pbr = true;
    fn(m);
    if (structural || !wasAdvanced) ++doc_->materialsRevision;
    doc_->commitOp();
    schedulePreview();
    vp_->update();
}

void MaterialEditor::wireSlider(QSlider* slider, const char* op, const std::function<void(Material&, float)>& set, float max, bool structural,
                                QDoubleSpinBox* value) {
    // A drag is one undo step and shows live; lighter than a commit per pixel.
    connect(slider, &QSlider::sliderPressed, this, [this, op] {
        if (!updating_ && material()) doc_->beginOp(op);
    });
    connect(slider, &QSlider::sliderReleased, this, [this] {
        if (doc_->inOp()) doc_->commitOp();
        refresh();
    });
    connect(slider, &QSlider::valueChanged, this, [=, this](int v) {
        if (updating_ || !material()) return;
        const float f = v / 100.0f * max;
        updating_ = true;
        value->setValue(f);
        updating_ = false;
        if (!slider->isSliderDown()) {
            apply(op, [&](Material& m) { set(m, f); }, structural);
            return;
        }
        Material& m = *material();
        const bool wasAdvanced = m.advanced();
        m.pbr = true;
        set(m, f);
        if (structural || !wasAdvanced) ++doc_->materialsRevision;
        schedulePreview();
        vp_->update();
    });
    connect(value, &QDoubleSpinBox::valueChanged, this, [=, this](double d) {
        if (updating_) return;
        updating_ = true;
        slider->setValue(int(std::lround(d / max * 100)));
        updating_ = false;
        apply(op, [&](Material& m) { set(m, (float)d); }, structural);
    });
}

QDoubleSpinBox* MaterialEditor::valueBox(QWidget* parent, float max) {
    auto* b = new QDoubleSpinBox(parent);
    b->setRange(0, max);
    b->setDecimals(2);
    b->setSingleStep(0.05);
    b->setButtonSymbols(QAbstractSpinBox::NoButtons);
    b->setKeyboardTracking(false);
    b->setFixedWidth(44);
    b->setAlignment(Qt::AlignRight);
    return b;
}

void MaterialEditor::addRow(QVBoxLayout* lay, int slot, const QString& title, const QString& hint, float max) {
    Row& r = rows_[slot];
    r.max = max;
    r.widget = new QWidget(content_);
    r.widget->setProperty("mapSlot", slot); // for drops onto this row
    r.widget->setToolTip(hint);
    auto* h = new QHBoxLayout(r.widget);
    h->setContentsMargins(0, 1, 0, 1);
    h->setSpacing(6);
    r.thumb = new QToolButton(r.widget);
    r.thumb->setObjectName("MapThumb");
    r.thumb->setFixedSize(40, 40);
    r.thumb->setIconSize(QSize(36, 36));
    r.thumb->setToolTip(QStringLiteral("Click to choose the %1 image, or drop one here").arg(title));
    h->addWidget(r.thumb);
    auto* col = new QVBoxLayout();
    col->setSpacing(0);
    auto* top = new QHBoxLayout();
    top->setSpacing(2);
    top->addWidget(new QLabel(title, r.widget), 1);
    r.clear = new QToolButton(r.widget);
    r.clear->setText(QStringLiteral("✕"));
    r.clear->setAutoRaise(true);
    r.clear->setFixedSize(18, 18);
    r.clear->setToolTip(QStringLiteral("Remove the image"));
    top->addWidget(r.clear);
    col->addLayout(top);
    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(4);
    r.slider = new QSlider(Qt::Horizontal, r.widget);
    r.slider->setRange(0, 100);
    r.value = valueBox(r.widget, max);
    bottom->addWidget(r.slider, 1);
    bottom->addWidget(r.value);
    col->addLayout(bottom);
    h->addLayout(col, 1);
    lay->addWidget(r.widget);
    connect(r.thumb, &QToolButton::clicked, this, [this, slot] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose Map Image"), QString(), kImageFilter);
        if (!path.isEmpty()) loadMap(slot, path);
    });
    connect(r.clear, &QToolButton::clicked, this, [this, slot] { clearMap(slot); });
    if (slot == MapSlotCount)
        wireSlider(r.slider, "Base Map Strength", [](Material& m, float v) { m.baseStrength = v; }, max, false, r.value);
    else
        wireSlider(r.slider, "Map Strength", [slot](Material& m, float v) { m.maps[slot].strength = v; }, max, false, r.value);
}

MaterialEditor::MaterialEditor(Document* doc, Viewport* vp, Mode mode, QWidget* parent)
    : QWidget(parent, mode == InWindow ? Qt::Window : Qt::Widget), doc_(doc), vp_(vp) {
    const bool window = mode == InWindow;
    setWindowTitle(QStringLiteral("Material Editor"));
    setAcceptDrops(true);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(window ? 8 : 0, window ? 8 : 0, window ? 8 : 0, 0);
    outer->setSpacing(6);
    if (window) resize(640, 780);

    preview_ = nullptr;
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    name_ = new QLineEdit(this);
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setStyleSheet(QStringLiteral("color: #555;"));
    auto* addMapsBtn = new QPushButton(QStringLiteral("Add Maps…"), this);
    addMapsBtn->setToolTip(QStringLiteral("Pick all the images of a material at once; each goes to the right map by its file name. You can also drop images here."));
    auto* saveBtn = new QPushButton(QStringLiteral("To Library"), this);
    saveBtn->setToolTip(QStringLiteral("Keep this material for other models: it appears in the Material Browser."));
    if (window) {
        auto* head = new QHBoxLayout();
        preview_ = new QLabel(this);
        preview_->setFixedSize(200, 200);
        preview_->setAlignment(Qt::AlignCenter);
        head->addWidget(preview_);
        auto* headCol = new QVBoxLayout();
        headCol->addWidget(title_);
        name_->setPlaceholderText(QStringLiteral("Name"));
        headCol->addWidget(name_);
        headCol->addWidget(addMapsBtn);
        headCol->addWidget(saveBtn);
        headCol->addWidget(message_);
        headCol->addStretch(1);
        head->addLayout(headCol, 1);
        outer->addLayout(head);
    } else {
        // The panel above already shows the globe and the name.
        title_->hide();
        name_->hide();
        auto* row = new QHBoxLayout();
        row->addWidget(addMapsBtn);
        row->addWidget(saveBtn);
        auto* popOut = new QPushButton(QStringLiteral("Window"), this);
        popOut->setToolTip(QStringLiteral("Edit in a larger window, better for shader code"));
        connect(popOut, &QPushButton::clicked, this, [this] { emit popOutRequested(); });
        row->addWidget(popOut);
        outer->addLayout(row);
        outer->addWidget(message_);
    }

    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    outer->addWidget(tabs, 1);

    // --- Surface
    content_ = new QWidget(tabs);
    auto* lay = new QVBoxLayout(content_);
    lay->setContentsMargins(window ? 8 : 2, 6, window ? 8 : 2, 6);
    lay->setSpacing(3);
    auto section = [&](const QString& text) {
        auto* l = new QLabel(text, content_);
        l->setStyleSheet(QStringLiteral("font-weight: 600; color: #3a4a5c; border-bottom: 1px solid #d4d9df; padding: 6px 0 2px 0;"));
        lay->addWidget(l);
    };
    auto* colors = new QFormLayout();
    colors->setContentsMargins(0, 0, 0, 0);
    colors->setVerticalSpacing(4);
    color_ = new QPushButton(content_);
    emission_ = new QPushButton(content_);
    section(QStringLiteral("Base"));
    colors->addRow(QStringLiteral("Color"), color_);
    auto* opacityRow = new QHBoxLayout();
    opacityRow->setSpacing(4);
    opacity_ = new QSlider(Qt::Horizontal, content_);
    opacity_->setRange(0, 100);
    opacityValue_ = valueBox(content_, 1);
    opacityRow->addWidget(opacity_, 1);
    opacityRow->addWidget(opacityValue_);
    colors->addRow(QStringLiteral("Opacity"), opacityRow);
    lay->addLayout(colors);
    addRow(lay, MapSlotCount, QStringLiteral("Color Image"), QStringLiteral("The strength mixes between the color (0) and the image (1)."), 1);
    addRow(lay, MapOpacity, QStringLiteral("Opacity Image"), QStringLiteral("Dark parts of the image are see-through."), 1);
    section(QStringLiteral("Finish"));
    addRow(lay, MapMetallic, QStringLiteral("Metallic"), QStringLiteral("0 is paint, plastic, stone; 1 is bare metal. An image multiplies it."), 1);
    addRow(lay, MapRoughness, QStringLiteral("Roughness"), QStringLiteral("0 is a mirror, 1 is fully matte. An image multiplies it."), 1);
    section(QStringLiteral("Relief"));
    addRow(lay, MapNormal, QStringLiteral("Normal"), QStringLiteral("Fine bumps that catch the light."), 2);
    flipY_ = new QCheckBox(QStringLiteral("DirectX style (green points down)"), content_);
    lay->addWidget(flipY_);
    addRow(lay, MapHeight, QStringLiteral("Height"), QStringLiteral("Real depth seen from an angle; white is high."), 1);
    addRow(lay, MapOcclusion, QStringLiteral("Ambient Occlusion"), QStringLiteral("Darkens cavities in soft light."), 1);
    section(QStringLiteral("Glow"));
    auto* glow = new QFormLayout();
    glow->setContentsMargins(0, 0, 0, 0);
    glow->addRow(QStringLiteral("Color"), emission_);
    lay->addLayout(glow);
    addRow(lay, MapEmission, QStringLiteral("Emission"), QStringLiteral("Light of its own; not affected by the sun. Set the color above."), 5);
    section(QStringLiteral("Tiling"));
    auto* tile = new QHBoxLayout();
    tileW_ = new QLineEdit(content_);
    tileH_ = new QLineEdit(content_);
    tileW_->setToolTip(QStringLiteral("Real-world width of one tile of the images"));
    tileH_->setToolTip(QStringLiteral("Real-world height of one tile of the images"));
    tile->addWidget(tileW_);
    tile->addWidget(new QLabel(QStringLiteral("×"), content_));
    tile->addWidget(tileH_);
    lay->addLayout(tile);
    lay->addStretch(1);
    if (window) {
        // In the sidebar the tray itself scrolls.
        auto* scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content_);
        tabs->addTab(scroll, QStringLiteral("Surface"));
    } else {
        tabs->addTab(content_, QStringLiteral("Surface"));
    }

    // --- Shader
    auto* shader = new QWidget(tabs);
    auto* sl = new QVBoxLayout(shader);
    sl->setContentsMargins(window ? 8 : 2, 6, window ? 8 : 2, 6);
    shaderOn_ = new QCheckBox(QStringLiteral("Use custom shader"), shader);
    blend_ = new QCheckBox(QStringLiteral("Transparent (uses s.alpha)"), shader);
    auto* compile = new QPushButton(QStringLiteral("Compile and Apply"), shader);
    compile->setToolTip(QStringLiteral("Ctrl+Return"));
    if (window) {
        auto* top = new QHBoxLayout();
        top->addWidget(shaderOn_);
        top->addWidget(blend_);
        top->addStretch(1);
        top->addWidget(compile);
        sl->addLayout(top);
    } else {
        sl->addWidget(shaderOn_);
        sl->addWidget(blend_);
        sl->addWidget(compile);
    }
    code_ = new QPlainTextEdit(shader);
    code_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    code_->setLineWrapMode(QPlainTextEdit::NoWrap);
    code_->setTabStopDistance(code_->fontMetrics().horizontalAdvance(' ') * 4);
    if (!window) code_->setMinimumHeight(260);
    sl->addWidget(code_, 3);
    propsBox_ = new QWidget(shader);
    propsForm_ = new QFormLayout(propsBox_);
    propsForm_->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(propsBox_);
    log_ = new QPlainTextEdit(shader);
    log_->setReadOnly(true);
    log_->setFont(code_->font());
    log_->setMaximumHeight(90);
    sl->addWidget(log_, 1);
    tabs->addTab(shader, QStringLiteral("Shader"));

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(60);
    connect(previewTimer_, &QTimer::timeout, this, [this] {
        const Material* m = material();
        if (!m || !preview_ || !isVisible()) return;
        const QImage img = OffscreenRenderer::instance().materialBall(*m, preview_->width());
        if (!img.isNull()) preview_->setPixmap(QPixmap::fromImage(img));
    });

    connect(addMapsBtn, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Choose the Material's Images"), QString(), kImageFilter);
        if (!paths.isEmpty()) addMaps(paths);
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        const Material* m = material();
        if (!m) return;
        QString err;
        const QString path = saveLibraryMaterial(*m, &err);
        message_->setText(path.isEmpty() ? err : QStringLiteral("Saved to the library."));
        if (!path.isEmpty()) emit saved();
    });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const Material* m = material();
        const std::string n = name_->text().trimmed().toStdString();
        if (!m || updating_ || n.empty() || n == m->name) return;
        const std::string unique = doc_->uniqueMaterialName(n);
        doc_->beginOp("Rename Material");
        material()->name = unique;
        ++doc_->materialsRevision;
        doc_->commitOp();
    });
    connect(color_, &QPushButton::clicked, this, [this] {
        const Material* m = material();
        if (!m) return;
        const QColor c = QColorDialog::getColor(QColor::fromRgbF(m->color.r, m->color.g, m->color.b), this, QStringLiteral("Base Color"));
        if (c.isValid()) apply("Material Color", [&](Material& mm) { mm.color = fromQ(c); }, true);
    });
    connect(emission_, &QPushButton::clicked, this, [this] {
        const Material* m = material();
        if (!m) return;
        const QColor c = QColorDialog::getColor(QColor::fromRgbF(m->emission.r, m->emission.g, m->emission.b), this, QStringLiteral("Emission Color"));
        if (c.isValid()) apply("Emission Color", [&](Material& mm) { mm.emission = fromQ(c); }, false);
    });
    wireSlider(opacity_, "Material Opacity", [](Material& m, float v) { m.opacity = v; }, 1, true, opacityValue_);
    connect(flipY_, &QCheckBox::toggled, this, [this](bool on) { apply("Normal Map Style", [&](Material& m) { m.flipNormalY = on; }, false); });
    connect(blend_, &QCheckBox::toggled, this, [this](bool on) { apply("Material Transparency", [&](Material& m) { m.blend = on; }, true); });
    connect(shaderOn_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_ || !material()) return;
        if (on && code_->toPlainText().trimmed().isEmpty()) code_->setPlainText(QString::fromLatin1(surfaceShaderTemplate()));
        if (on) {
            if (!compileShader()) {
                updating_ = true;
                shaderOn_->setChecked(false);
                updating_ = false;
            }
        } else {
            apply("Custom Shader Off", [](Material& m) { m.shaderOn = false; }, true);
        }
    });
    connect(compile, &QPushButton::clicked, this, [this] { compileShader(); });
    auto* run = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), code_);
    connect(run, &QShortcut::activated, this, [this] { compileShader(); });
    auto applyTile = [this](bool width) {
        const Material* m = material();
        if (!m || updating_) return;
        double w, h, v;
        m->tileSize(w, h);
        if (!doc_->units.parseLength((width ? tileW_ : tileH_)->text().toStdString(), v) || v <= 0) {
            refresh();
            return;
        }
        // Keeps the proportions of the images.
        const double aspect = h / std::max(w, 1e-9);
        const double nw = width ? v : v / aspect, nh = width ? v * aspect : v;
        if (std::abs(nw - w) < 1e-9 && std::abs(nh - h) < 1e-9) return;
        apply("Texture Size", [&](Material& mm) { mm.setTileSize(nw, nh); }, true);
    };
    connect(tileW_, &QLineEdit::editingFinished, this, [applyTile] { applyTile(true); });
    connect(tileH_, &QLineEdit::editingFinished, this, [applyTile] { applyTile(false); });
    refresh();
}

void MaterialEditor::schedulePreview() { previewTimer_->start(); }

bool MaterialEditor::loadMap(int slot, const QString& path) {
    const Material* m0 = material();
    if (!m0) return false;
    Texture t;
    QString err;
    if (!loadTextureFile(path, t, &err)) {
        message_->setText(err);
        return false;
    }
    const bool hadImages = m0->hasTexture() || m0->hasAnyMap();
    double w, h;
    m0->tileSize(w, h);
    apply("Set Map", [&](Material& m) {
        if (slot == MapSlotCount) {
            m.texture = t;
            if (!hadImages) m.setTileSize(t.worldWidth, t.worldHeight);
            m.color = averageColor(decodeTexture(t));
        } else {
            m.maps[slot].tex = t;
            if (slot == MapMetallic || slot == MapRoughness) m.maps[slot].strength = 1;
            if (slot == MapEmission && m.emission.r <= 0 && m.emission.g <= 0 && m.emission.b <= 0) m.emission = Color(1, 1, 1);
            if (!hadImages) m.setTileSize(t.worldWidth, t.worldHeight);
        }
        if (hadImages) m.setTileSize(w, h); // every map tiles together
    }, true);
    refresh();
    return true;
}

void MaterialEditor::clearMap(int slot) {
    double w = 1, h = 1;
    if (const Material* m = material()) m->tileSize(w, h);
    apply("Clear Map", [&](Material& m) {
        if (slot == MapSlotCount) m.texture = Texture();
        else m.maps[slot].tex = Texture();
        m.setTileSize(w, h); // survives losing the image that defined it
    }, true);
    refresh();
}

int MaterialEditor::addMaps(const QStringList& paths) {
    if (!material()) return 0;
    int done = 0;
    QStringList skipped;
    bool baseFree = !material()->hasTexture();
    for (const QString& path : paths) {
        int slot = guessMapSlot(QFileInfo(path).fileName().toStdString());
        if (slot < 0) {
            if (!baseFree) {
                skipped << QFileInfo(path).fileName();
                continue;
            }
            slot = MapSlotCount;
        }
        if (slot == MapSlotCount) baseFree = false;
        done += loadMap(slot, path) ? 1 : 0;
    }
    if (!skipped.isEmpty())
        message_->setText(QStringLiteral("Could not tell which map these are; use Load… on the right row: %1").arg(skipped.join(QStringLiteral(", "))));
    else if (done > 0)
        message_->setText(QStringLiteral("%1 map(s) added.").arg(done));
    return done;
}

bool MaterialEditor::compileShader() {
    if (!material()) return false;
    const std::string src = code_->toPlainText().toStdString();
    QString log;
    if (!OffscreenRenderer::instance().compileShader(src, &log)) {
        log_->setPlainText(log);
        return false;
    }
    log_->setPlainText(QStringLiteral("Compiled."));
    apply("Custom Shader", [&](Material& m) {
        m.shader = src;
        m.shaderOn = true;
        // Values of properties no longer declared go; new ones start at their default.
        std::map<std::string, std::array<float, 4>> props;
        for (const ShaderProp& p : parseShaderProps(src)) {
            auto it = m.shaderProps.find(p.name);
            props[p.name] = it != m.shaderProps.end() ? it->second : p.def;
        }
        m.shaderProps = std::move(props);
    }, true);
    refresh();
    return true;
}

QString MaterialEditor::shaderLog() const { return log_->toPlainText(); }

void MaterialEditor::rebuildProps() {
    while (propsForm_->rowCount() > 0) propsForm_->removeRow(0);
    const Material* m0 = material();
    if (!m0) return;
    for (const ShaderProp& p : parseShaderProps(m0->shader)) {
        const std::string name = p.name;
        const QString label = QString::fromStdString(name);
        auto current = [this, name, def = p.def] {
            const Material* m = material();
            if (!m) return def;
            auto it = m->shaderProps.find(name);
            return it != m->shaderProps.end() ? it->second : def;
        };
        if (p.isColor) {
            auto* b = new QPushButton(QStringLiteral("Color…"), propsBox_);
            const auto v = current();
            setButtonColor(b, Color(v[0], v[1], v[2]));
            connect(b, &QPushButton::clicked, this, [=, this] {
                const auto c0 = current();
                const QColor c = QColorDialog::getColor(QColor::fromRgbF(std::clamp(c0[0], 0.f, 1.f), std::clamp(c0[1], 0.f, 1.f), std::clamp(c0[2], 0.f, 1.f)), this, label);
                if (!c.isValid()) return;
                setButtonColor(b, fromQ(c));
                apply("Shader Property", [&](Material& m) {
                    auto vals = c0;
                    vals[0] = (float)c.redF();
                    vals[1] = (float)c.greenF();
                    vals[2] = (float)c.blueF();
                    m.shaderProps[name] = vals;
                }, false);
            });
            propsForm_->addRow(label, b);
        } else if (p.components == 1) {
            auto* row = new QWidget(propsBox_);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* s = new QSlider(Qt::Horizontal, row);
            s->setRange(0, 1000);
            auto* value = new QLabel(row);
            value->setMinimumWidth(40);
            const float lo = p.min, hi = p.max;
            const float v = current()[0];
            s->setValue(int(std::lround((v - lo) / (hi - lo) * 1000)));
            value->setText(QString::number(v, 'g', 3));
            h->addWidget(s, 1);
            h->addWidget(value);
            connect(s, &QSlider::sliderPressed, this, [this] {
                if (material()) doc_->beginOp("Shader Property");
            });
            connect(s, &QSlider::sliderReleased, this, [this] {
                if (doc_->inOp()) doc_->commitOp();
            });
            connect(s, &QSlider::valueChanged, this, [=, this](int iv) {
                if (updating_) return;
                const float f = lo + (hi - lo) * iv / 1000.0f;
                value->setText(QString::number(f, 'g', 3));
                auto set = [&](Material& m) {
                    auto vals = current();
                    vals[0] = f;
                    m.shaderProps[name] = vals;
                };
                if (s->isSliderDown()) {
                    if (Material* m = material()) set(*m);
                    schedulePreview();
                    vp_->update();
                } else {
                    apply("Shader Property", set, false);
                }
            });
            propsForm_->addRow(label, row);
        } else {
            auto* row = new QWidget(propsBox_);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            const auto v = current();
            for (int i = 0; i < p.components; ++i) {
                auto* s = new QDoubleSpinBox(row);
                s->setRange(-1e6, 1e6);
                s->setDecimals(3);
                s->setSingleStep(0.1);
                s->setValue(v[i]);
                s->setKeyboardTracking(false);
                connect(s, &QDoubleSpinBox::valueChanged, this, [=, this](double d) {
                    apply("Shader Property", [&](Material& m) {
                        auto vals = current();
                        vals[i] = (float)d;
                        m.shaderProps[name] = vals;
                    }, false);
                });
                h->addWidget(s);
            }
            propsForm_->addRow(label, row);
        }
    }
    propsBox_->setVisible(propsForm_->rowCount() > 0);
}

void MaterialEditor::refresh() {
    if (doc_->inOp()) return; // in the middle of a drag: the widgets are the source
    updating_ = true;
    const Material* m = material();
    setEnabled(true);
    title_->setText(m ? QStringLiteral("<b>%1</b>").arg(QString::fromStdString(m->name).toHtmlEscaped())
                      : QStringLiteral("Choose or create a material in the Materials panel or the Material Browser."));
    for (QWidget* w : std::initializer_list<QWidget*>{name_, content_, code_, shaderOn_, blend_}) w->setEnabled(m != nullptr);
    if (!m) {
        if (preview_ && isWindow()) preview_->clear();
        name_->clear();
        propsFor_.clear();
        rebuildProps();
        editing_ = kNone;
        updating_ = false;
        return;
    }
    if (!name_->hasFocus()) name_->setText(QString::fromStdString(m->name));
    setButtonColor(color_, m->color);
    setButtonColor(emission_, m->emission);
    opacity_->setValue(int(std::lround(m->opacity * 100)));
    opacityValue_->setValue(m->opacity);
    flipY_->setChecked(m->flipNormalY);
    double tw, th;
    m->tileSize(tw, th);
    if (!tileW_->hasFocus()) tileW_->setText(QString::fromStdString(doc_->units.format(tw)));
    if (!tileH_->hasFocus()) tileH_->setText(QString::fromStdString(doc_->units.format(th)));
    for (int s = 0; s <= MapSlotCount; ++s) {
        Row& r = rows_[s];
        const bool base = s == MapSlotCount;
        const Texture& t = base ? m->texture : m->maps[s].tex;
        const float strength = base ? m->baseStrength : m->maps[s].strength;
        r.slider->setValue(int(std::lround(strength / r.max * 100)));
        r.value->setValue(strength);
        r.clear->setVisible(t.valid());
        const uint64_t revision = t.valid() ? t.revision : 0;
        if (r.thumbRevision != revision) {
            r.thumbRevision = revision;
            if (t.valid()) {
                const QImage img = decodeTexture(t);
                r.thumb->setIcon(img.isNull() ? QIcon() : QIcon(QPixmap::fromImage(img.scaled(36, 36, Qt::IgnoreAspectRatio, Qt::SmoothTransformation))));
                r.thumb->setText(QString());
            } else {
                r.thumb->setIcon(QIcon());
                r.thumb->setText(QStringLiteral("+"));
            }
        }
    }
    shaderOn_->setChecked(m->shaderOn);
    blend_->setChecked(m->blend);
    // Keeps what is being typed unless another material was chosen.
    if (editing_ != doc_->currentMaterial || !code_->hasFocus()) {
        if (code_->toPlainText().toStdString() != m->shader) code_->setPlainText(QString::fromStdString(m->shader));
    }
    if (editing_ != doc_->currentMaterial || propsFor_ != m->shader) {
        propsFor_ = m->shader;
        rebuildProps();
    }
    if (editing_ != doc_->currentMaterial) log_->clear();
    editing_ = doc_->currentMaterial;
    updating_ = false;
    schedulePreview();
}

void MaterialEditor::dragEnterEvent(QDragEnterEvent* e) {
    if (material() && e->mimeData()->hasUrls()) e->acceptProposedAction();
    else e->ignore();
}

void MaterialEditor::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && isImageFile(u.toLocalFile())) paths << u.toLocalFile();
    if (paths.isEmpty()) return;
    e->acceptProposedAction();
    // Dropped on a map's row: that map. Anywhere else: by the file names.
    for (QWidget* w = childAt(e->position().toPoint()); w; w = w->parentWidget()) {
        const QVariant slot = w->property("mapSlot");
        if (slot.isValid()) {
            loadMap(slot.toInt(), paths.first());
            return;
        }
    }
    addMaps(paths);
}

} // namespace gs
