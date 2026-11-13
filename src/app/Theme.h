// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - application look: dark/light palette and the duotone icon colours.
// Stored in QSettings under "ui/…" and edited in Window → Settings.
#pragma once

#include <QColor>
#include <QDialog>
#include <QString>

class QApplication;
class QComboBox;
class QSlider;
class QSpinBox;
class QToolButton;

namespace gs {

struct ThemeSettings {
    bool dark = true;            // dark window chrome (false = the classic light one)
    bool duotone = true;         // icons drawn in the two colours below (false = full colour)
    QColor primary{226, 230, 236};   // outlines and details
    QColor secondary{79, 156, 255};  // filled areas
    double secondaryOpacity = 0.75;  // strength of the filled areas, 0..1
    int fontSize = 0;  // interface font in points (0 = the system's)
    int iconSize = 24; // toolbar icons in pixels

    static ThemeSettings load();
    void save() const;
    bool operator==(const ThemeSettings&) const = default;
};

// Tells windows that the theme changed (connect to resize their icons).
class ThemeNotifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};

namespace Theme {

ThemeNotifier* notifier();

// Current settings (loaded once, then whatever apply() was last given).
const ThemeSettings& current();
// Palette, style sheet and icon colours for the whole application. Icons made
// with Icons::get() repaint themselves; open windows are refreshed.
void apply(const ThemeSettings& s);
// Applies the stored settings (call once at start-up).
void applyStored();
// Bumped by apply(); icon pixmap caches key on it.
int revision();
// Recolours an icon's SVG body for the current settings (unchanged when
// duotone is off). Exposed for tests.
QByteArray recolorSvg(const QByteArray& body, const ThemeSettings& s);

} // namespace Theme

// Window → Settings: theme and icon colours, applied live.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ThemeSettings edited() const { return edit_; }
    // For tests: set and apply without the colour picker.
    void setPrimary(const QColor& c);
    void setSecondary(const QColor& c);
    void setDuotone(bool on);
    void setDark(bool on);
    void setFontSize(int points);
    void setIconSize(int pixels);
    // Esc, the window's close button and Cancel all undo the live preview.
    void reject() override;

private:
    void refresh();
    void pick(QColor* target);
    ThemeSettings original_;
    ThemeSettings edit_;
    QComboBox* theme_ = nullptr;
    QComboBox* iconStyle_ = nullptr;
    QToolButton* primaryBtn_ = nullptr;
    QToolButton* secondaryBtn_ = nullptr;
    QSlider* opacity_ = nullptr;
    QSpinBox* fontSize_ = nullptr;
    QComboBox* iconSize_ = nullptr;
    QWidget* preview_ = nullptr;
};

} // namespace gs
