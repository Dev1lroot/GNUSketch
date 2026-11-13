// SPDX-License-Identifier: GPL-3.0-or-later
#include "Theme.h"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyleFactory>
#include <QToolButton>
#include <QVBoxLayout>

#include "Icons.h"

#include <algorithm>
#include <cmath>

namespace gs {

ThemeSettings ThemeSettings::load() {
    QSettings s;
    ThemeSettings t;
    t.dark = s.value("ui/dark", t.dark).toBool();
    t.duotone = s.value("ui/duotone", t.duotone).toBool();
    const QColor p(s.value("ui/iconPrimary", t.primary.name()).toString());
    const QColor q(s.value("ui/iconSecondary", t.secondary.name()).toString());
    if (p.isValid()) t.primary = p;
    if (q.isValid()) t.secondary = q;
    t.secondaryOpacity = std::clamp(s.value("ui/iconSecondaryOpacity", t.secondaryOpacity).toDouble(), 0.0, 1.0);
    t.fontSize = std::clamp(s.value("ui/fontSize", t.fontSize).toInt(), 0, 32);
    t.iconSize = std::clamp(s.value("ui/iconSize", t.iconSize).toInt(), 12, 64);
    return t;
}

void ThemeSettings::save() const {
    QSettings s;
    s.setValue("ui/dark", dark);
    s.setValue("ui/duotone", duotone);
    s.setValue("ui/iconPrimary", primary.name());
    s.setValue("ui/iconSecondary", secondary.name());
    s.setValue("ui/iconSecondaryOpacity", secondaryOpacity);
    s.setValue("ui/fontSize", fontSize);
    s.setValue("ui/iconSize", iconSize);
}

namespace Theme {

namespace {

ThemeSettings& state() {
    static ThemeSettings s = ThemeSettings::load();
    return s;
}
int& rev() {
    static int r = 0;
    return r;
}

void applyPalette(QApplication& app, bool dark) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    if (dark) {
        const QColor window(43, 45, 49), base(30, 32, 35), text(222, 225, 229), button(52, 55, 60);
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, QColor(37, 39, 43));
        p.setColor(QPalette::ToolTipBase, QColor(24, 26, 29));
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, button);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, QColor(255, 100, 100));
        p.setColor(QPalette::Highlight, QColor(53, 116, 204));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, QColor(110, 170, 255));
        p.setColor(QPalette::PlaceholderText, QColor(130, 134, 140));
        p.setColor(QPalette::Light, QColor(70, 73, 79));
        p.setColor(QPalette::Midlight, QColor(60, 63, 68));
        p.setColor(QPalette::Mid, QColor(36, 38, 41));
        p.setColor(QPalette::Dark, QColor(25, 26, 29));
        p.setColor(QPalette::Shadow, QColor(10, 10, 12));
        for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
            p.setColor(QPalette::Disabled, role, QColor(115, 118, 124));
        p.setColor(QPalette::Disabled, QPalette::Base, QColor(38, 40, 44));
        app.setPalette(p);
        app.setStyleSheet(R"(
            QMainWindow::separator { background: #1e2023; width: 4px; height: 4px; }
            QToolBar { background: #2b2d31; border: none; spacing: 1px; padding: 2px; }
            QToolBar::separator { background: #44474d; width: 1px; height: 1px; margin: 4px; }
            QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 2px; }
            QToolButton:hover { border: 1px solid #4a6f9e; background: #34404f; }
            QToolButton:checked { border: 1px solid #4f8fe0; background: #2c4566; }
            QMenuBar { background: #2b2d31; }
            QMenuBar::item:selected { background: #3b3f45; }
            QMenu { background: #2f3236; border: 1px solid #1c1e21; }
            QMenu::item:selected { background: #3574cc; color: white; }
            QMenu::separator { height: 1px; background: #44474d; margin: 3px 6px; }
            QToolTip { background: #181a1d; color: #dee1e5; border: 1px solid #44474d; }
            QToolButton#TrayHeader { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #383b40, stop:1 #303337);
                                     border: 1px solid #222428; border-radius: 0; text-align: left; padding: 3px 4px; font-weight: 600; }
            QToolButton#TrayHeader:hover { background: #3a4552; }
            QToolButton#FlatTab { border: none; border-bottom: 2px solid transparent; padding: 2px 8px; }
            QToolButton#FlatTab:checked { background: transparent; border-bottom: 2px solid #4f8fe0; }
            QDockWidget::title { background: #303337; padding: 4px; border-bottom: 1px solid #1e2023; }
            QStatusBar { background: #2b2d31; border-top: 1px solid #1e2023; }
            QStatusBar::item { border: none; }
            QToolButton#MapThumb { border: 1px dashed #5a6068; background: #26282c; color: #8a929c; font-size: 16px; }
            QToolButton#MapThumb:hover { border: 1px solid #4f8fe0; background: #2c4566; }
            QPushButton#CreateMaterial { font-weight: 600; padding: 5px; }
            QLineEdit#Measurements { border: 1px solid #50545a; padding: 1px 4px; background: #1e2023; color: #e8eaed; }
            QLabel[hint="true"] { color: #9aa1aa; }
            QLabel#SectionHeader { font-weight: 600; color: #aeb8c4; border-bottom: 1px solid #44474d; padding: 6px 0 2px 0; }
        )");
    } else {
        p.setColor(QPalette::Window, QColor(240, 240, 240));
        p.setColor(QPalette::WindowText, Qt::black);
        p.setColor(QPalette::Base, Qt::white);
        p.setColor(QPalette::AlternateBase, QColor(246, 246, 246));
        p.setColor(QPalette::ToolTipBase, QColor(255, 255, 225));
        p.setColor(QPalette::ToolTipText, Qt::black);
        p.setColor(QPalette::Text, Qt::black);
        p.setColor(QPalette::Button, QColor(240, 240, 240));
        p.setColor(QPalette::ButtonText, Qt::black);
        p.setColor(QPalette::BrightText, Qt::red);
        p.setColor(QPalette::Highlight, QColor(0, 120, 215));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, QColor(0, 102, 204));
        p.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor(150, 150, 150));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(150, 150, 150));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(150, 150, 150));
        app.setPalette(p);
        app.setStyleSheet(R"(
            QMainWindow::separator { background: #d8d8d8; width: 4px; height: 4px; }
            QToolBar { background: #f0f0f0; border: none; spacing: 1px; padding: 2px; }
            QToolBar::separator { background: #c8c8c8; width: 1px; height: 1px; margin: 4px; }
            QToolButton { border: 1px solid transparent; border-radius: 2px; padding: 2px; }
            QToolButton:hover { border: 1px solid #9cc4ea; background: #e5f1fb; }
            QToolButton:checked { border: 1px solid #569de5; background: #cce4f7; }
            QToolButton#TrayHeader { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #fafafa, stop:1 #e6e6e6);
                                     border: 1px solid #cfcfcf; border-radius: 0; text-align: left; padding: 3px 4px; font-weight: 600; }
            QToolButton#TrayHeader:hover { background: #e9f2fb; }
            QToolButton#FlatTab { border: none; border-bottom: 2px solid transparent; padding: 2px 8px; }
            QToolButton#FlatTab:checked { background: transparent; border-bottom: 2px solid #3b6fb6; }
            QDockWidget::title { background: #e9e9e9; padding: 4px; border-bottom: 1px solid #d0d0d0; }
            QStatusBar { background: #f0f0f0; border-top: 1px solid #d0d0d0; }
            QStatusBar::item { border: none; }
            QToolButton#MapThumb { border: 1px dashed #a9b3bd; background: #f7f8f9; color: #8a95a0; font-size: 16px; }
            QToolButton#MapThumb:hover { border: 1px solid #569de5; background: #e5f1fb; }
            QPushButton#CreateMaterial { font-weight: 600; padding: 5px; }
            QLineEdit#Measurements { border: 1px solid #a8a8a8; padding: 1px 4px; background: white; }
            QLabel[hint="true"] { color: #555; }
            QLabel#SectionHeader { font-weight: 600; color: #3a4a5c; border-bottom: 1px solid #d4d9df; padding: 6px 0 2px 0; }
        )");
    }
}

QString fmtNum(double v) { return QString::number(std::clamp(v, 0.0, 1.0), 'f', 3); }

} // namespace

const ThemeSettings& current() { return state(); }

ThemeNotifier* notifier() {
    static ThemeNotifier* n = new ThemeNotifier;
    return n;
}
int revision() { return rev(); }

QByteArray recolorSvg(const QByteArray& body, const ThemeSettings& s) {
    if (!s.duotone) return body;
    // Every painted part of an icon becomes one of two colours. Dark outlines
    // and light details drawn on top of a filled shape (strokes) take the
    // primary colour; filled areas take the secondary one, darker/more
    // saturated originals more strongly so the shapes stay readable.
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    static const QRegularExpression attrRe(QStringLiteral("\\b(fill|stroke)=\"#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\""));
    const QString src = QString::fromUtf8(body);
    QString out;
    out.reserve(src.size() + 256);
    qsizetype last = 0;
    auto it = tagRe.globalMatch(src);
    while (it.hasNext()) {
        const auto tm = it.next();
        out += src.mid(last, tm.capturedStart() - last);
        last = tm.capturedEnd();
        QString tag = tm.captured(0);
        // Existing opacities of this element, multiplied into ours.
        auto takeOpacity = [&](const QString& name) {
            const QRegularExpression re(QStringLiteral("\\s%1=\"([0-9.]+)\"").arg(name));
            const auto m = re.match(tag);
            if (!m.hasMatch()) return 1.0;
            tag.remove(m.capturedStart(), m.capturedLength());
            return m.captured(1).toDouble();
        };
        const double fillOp = takeOpacity(QStringLiteral("fill-opacity"));
        const double strokeOp = takeOpacity(QStringLiteral("stroke-opacity"));
        QString rebuilt;
        qsizetype pos = 0;
        auto am = attrRe.globalMatch(tag);
        bool sawFill = false, sawStroke = false;
        while (am.hasNext()) {
            const auto m = am.next();
            rebuilt += tag.mid(pos, m.capturedStart() - pos);
            pos = m.capturedEnd();
            const bool isFill = m.captured(1) == QLatin1String("fill");
            QColor c(QLatin1Char('#') + m.captured(2));
            const double l = c.lightnessF();
            const double sat = c.hslSaturationF();
            QColor target;
            double op;
            if (isFill) {
                sawFill = true;
                if (l < 0.3) { target = s.primary; op = 1.0; }
                else {
                    target = s.secondary;
                    // white body ≈ 0.35, mid tones ≈ 0.7, dark/saturated ≈ 1
                    op = s.secondaryOpacity * std::clamp(0.35 + 0.8 * (1.0 - l) + 0.35 * sat, 0.3, 1.0);
                }
                op *= fillOp;
            } else {
                sawStroke = true;
                // Strokes are lines: always the primary colour, light ones too
                // (they are details drawn over a filled shape).
                target = (l > 0.55 && sat > 0.35) ? s.secondary : s.primary;
                op = strokeOp;
            }
            rebuilt += QStringLiteral("%1=\"%2\" %1-opacity=\"%3\"").arg(m.captured(1), target.name(), fmtNum(op));
        }
        rebuilt += tag.mid(pos);
        // An opacity we took off but did not re-add (attribute without a
        // colour on this element) goes back unchanged.
        if (!sawFill && fillOp != 1.0) rebuilt.insert(rebuilt.indexOf(QLatin1Char(' ')), QStringLiteral(" fill-opacity=\"%1\"").arg(fmtNum(fillOp)));
        if (!sawStroke && strokeOp != 1.0) rebuilt.insert(rebuilt.indexOf(QLatin1Char(' ')), QStringLiteral(" stroke-opacity=\"%1\"").arg(fmtNum(strokeOp)));
        out += rebuilt;
    }
    out += src.mid(last);
    return out.toUtf8();
}

void apply(const ThemeSettings& s) {
    state() = s;
    ++rev();
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        applyPalette(*app, s.dark);
        // Font size: the system's font, or the same at the chosen size.
        static const QFont systemFont = QApplication::font();
        QFont f = systemFont;
        if (s.fontSize > 0) f.setPointSize(s.fontSize);
        if (QApplication::font() != f) QApplication::setFont(f);
        for (QWidget* w : QApplication::allWidgets()) w->update();
    }
    emit notifier()->changed();
}

void applyStored() { apply(ThemeSettings::load()); }

} // namespace Theme

// ---- Settings dialog ----------------------------------------------------------

namespace {

class IconPreview : public QWidget {
public:
    explicit IconPreview(QWidget* parent) : QWidget(parent) { setMinimumHeight(44); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        int x = 6;
        for (const char* n : {"select", "line", "rectangle", "pushpull", "move", "rotate", "paint", "eraser", "orbit", "materials", "solid_union", "light_point"}) {
            p.drawPixmap(x, 8, Icons::get(QString::fromLatin1(n)).pixmap(28, 28));
            x += 34;
        }
    }
};

void colorButton(QToolButton* b, const QColor& c) {
    QPixmap pm(36, 16);
    pm.fill(c);
    b->setIcon(QIcon(pm));
    b->setIconSize(pm.size());
    b->setText(c.name());
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
}

} // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Settings"));
    original_ = edit_ = Theme::current();
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    lay->addLayout(form);

    theme_ = new QComboBox(this);
    theme_->addItems({QStringLiteral("Dark"), QStringLiteral("Light")});
    form->addRow(QStringLiteral("Interface:"), theme_);

    iconStyle_ = new QComboBox(this);
    iconStyle_->addItems({QStringLiteral("Duotone"), QStringLiteral("Full colour")});
    form->addRow(QStringLiteral("Icons:"), iconStyle_);

    primaryBtn_ = new QToolButton(this);
    secondaryBtn_ = new QToolButton(this);
    form->addRow(QStringLiteral("Primary colour (lines):"), primaryBtn_);
    form->addRow(QStringLiteral("Secondary colour (fills):"), secondaryBtn_);
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(10, 100);
    form->addRow(QStringLiteral("Secondary strength:"), opacity_);

    fontSize_ = new QSpinBox(this);
    fontSize_->setRange(0, 32);
    fontSize_->setSpecialValueText(QStringLiteral("System"));
    fontSize_->setSuffix(QStringLiteral(" pt"));
    form->addRow(QStringLiteral("Font size:"), fontSize_);
    iconSize_ = new QComboBox(this);
    for (int px : {16, 20, 24, 28, 32, 40, 48}) iconSize_->addItem(QStringLiteral("%1 px").arg(px), px);
    form->addRow(QStringLiteral("Toolbar icons:"), iconSize_);

    auto* presets = new QHBoxLayout;
    struct Preset { const char* name; QColor a, b; };
    const Preset list[] = {
        {"Blue", QColor(226, 230, 236), QColor(79, 156, 255)},
        {"Amber", QColor(232, 228, 220), QColor(255, 170, 40)},
        {"Green", QColor(222, 232, 224), QColor(70, 200, 120)},
        {"Violet", QColor(230, 226, 238), QColor(170, 120, 255)},
        {"Mono", QColor(230, 230, 230), QColor(150, 150, 150)},
    };
    for (const Preset& pr : list) {
        auto* b = new QPushButton(QString::fromLatin1(pr.name), this);
        connect(b, &QPushButton::clicked, this, [this, pr] {
            edit_.primary = pr.a;
            edit_.secondary = pr.b;
            edit_.duotone = true;
            refresh();
        });
        presets->addWidget(b);
    }
    form->addRow(QStringLiteral("Presets:"), presets);

    preview_ = new IconPreview(this);
    lay->addWidget(preview_);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this] { edit_.save(); accept(); });
    connect(box, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(box->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] { edit_ = ThemeSettings(); refresh(); });

    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int i) { edit_.dark = i == 0; refresh(); });
    connect(iconStyle_, &QComboBox::currentIndexChanged, this, [this](int i) { edit_.duotone = i == 0; refresh(); });
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) { edit_.secondaryOpacity = v / 100.0; refresh(); });
    connect(fontSize_, &QSpinBox::valueChanged, this, [this](int v) { edit_.fontSize = v; refresh(); });
    connect(iconSize_, &QComboBox::currentIndexChanged, this, [this](int i) { edit_.iconSize = iconSize_->itemData(i).toInt(); refresh(); });
    connect(primaryBtn_, &QToolButton::clicked, this, [this] { pick(&edit_.primary); });
    connect(secondaryBtn_, &QToolButton::clicked, this, [this] { pick(&edit_.secondary); });
    refresh();
}

void SettingsDialog::pick(QColor* target) {
    const QColor c = QColorDialog::getColor(*target, this, QStringLiteral("Icon colour"));
    if (!c.isValid()) return;
    *target = c;
    refresh();
}

void SettingsDialog::reject() {
    if (!(Theme::current() == original_)) Theme::apply(original_);
    QDialog::reject();
}

void SettingsDialog::setPrimary(const QColor& c) { edit_.primary = c; refresh(); }
void SettingsDialog::setSecondary(const QColor& c) { edit_.secondary = c; refresh(); }
void SettingsDialog::setDuotone(bool on) { edit_.duotone = on; refresh(); }
void SettingsDialog::setDark(bool on) { edit_.dark = on; refresh(); }
void SettingsDialog::setFontSize(int points) { edit_.fontSize = points; refresh(); }
void SettingsDialog::setIconSize(int pixels) { edit_.iconSize = pixels; refresh(); }

void SettingsDialog::refresh() {
    const QSignalBlocker b1(theme_), b2(iconStyle_), b3(opacity_), b4(fontSize_), b5(iconSize_);
    fontSize_->setValue(edit_.fontSize);
    int iconIndex = iconSize_->findData(edit_.iconSize);
    if (iconIndex < 0) {
        iconSize_->addItem(QStringLiteral("%1 px").arg(edit_.iconSize), edit_.iconSize);
        iconIndex = iconSize_->count() - 1;
    }
    iconSize_->setCurrentIndex(iconIndex);
    theme_->setCurrentIndex(edit_.dark ? 0 : 1);
    iconStyle_->setCurrentIndex(edit_.duotone ? 0 : 1);
    opacity_->setValue(int(std::lround(edit_.secondaryOpacity * 100)));
    colorButton(primaryBtn_, edit_.primary);
    colorButton(secondaryBtn_, edit_.secondary);
    for (QWidget* w : {static_cast<QWidget*>(primaryBtn_), static_cast<QWidget*>(secondaryBtn_), static_cast<QWidget*>(opacity_)})
        w->setEnabled(edit_.duotone);
    if (!(edit_ == Theme::current())) Theme::apply(edit_); // live preview everywhere
    preview_->update();
}

} // namespace gs
