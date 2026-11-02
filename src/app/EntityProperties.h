// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - the small window of a component's game behaviour: its class, its
// name and target as in Valve Hammer, its animation states and its script.
#pragma once

#include "core/Document.h"

#include <QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace gs {

class Viewport;

class EntityProperties : public QWidget {
    Q_OBJECT
public:
    EntityProperties(Document* doc, Viewport* vp, QWidget* parent = nullptr);
    void setInstance(Id instance);
    void refresh();
    // Stores the text of the script editor if it changed.
    void applyScript();
    // Sets the class as the combo box does, giving it a script if it has none.
    void setClass(const QString& classname);
    void choosePreset(const QString& name);
    void setPhysics(bool on, double mass);
    QPlainTextEdit* scriptEdit() const { return script_; }
    QString message() const;
    // Targets tab.
    void addTarget(const std::string& name, double delay);
    void setTarget(int row, const std::string& name, double delay);
    void removeTarget(int row);
    // Sources tab: another entity starts or stops firing this one.
    void addSource(Id instance);
    void removeSource(Id instance);
    // The entities that fire this one.
    std::vector<Id> sources();
    int targetRows() const;
    int sourceRows() const;

signals:
    void recordRequested();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    Instance* instance();
    ComponentDef* definition();
    void edit(const char* op, const std::function<void(EntityProps&)>& fn);
    void fillPresets();
    void refreshTargets();
    void refreshSources();
    QStringList otherNames();
    Document* doc_;
    Viewport* vp_;
    Id inst_ = kNone;
    QLabel* title_;
    QComboBox* class_;
    QLineEdit* name_;
    QTabWidget* tabs_;
    QTableWidget* targets_;
    QTableWidget* sources_;
    QComboBox* addSource_;
    QLabel* sourcesHint_;
    QComboBox* start_;
    QListWidget* states_;
    QCheckBox* physics_;
    QDoubleSpinBox* mass_;
    QDoubleSpinBox* friction_;
    QDoubleSpinBox* bounce_;
    QPushButton* record_;
    QPushButton* goTo_;
    QPushButton* rename_;
    QPushButton* remove_;
    QComboBox* preset_;
    QPlainTextEdit* script_;
    QLabel* message_;
    bool updating_ = false;
};

} // namespace gs
