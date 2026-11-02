// SPDX-License-Identifier: GPL-3.0-or-later
#include "EntityProperties.h"

#include "Viewport.h"
#include "core/Animation.h"
#include "io/EntityScript.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QFontDatabase>
#include <QCheckBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace gs {

Instance* EntityProperties::instance() {
    auto& insts = doc_->state.instances;
    return inst_ >= 0 && inst_ < (Id)insts.size() && insts[inst_].alive ? &insts[inst_] : nullptr;
}

ComponentDef* EntityProperties::definition() {
    Instance* in = instance();
    return in && in->def >= 0 && in->def < (Id)doc_->state.defs.size() ? &doc_->state.defs[in->def] : nullptr;
}

void EntityProperties::edit(const char* op, const std::function<void(EntityProps&)>& fn) {
    if (updating_ || !instance()) return;
    doc_->beginOp(op);
    fn(instance()->entity);
    doc_->commitOp();
}

EntityProperties::EntityProperties(Document* doc, Viewport* vp, QWidget* parent) : QWidget(parent, Qt::Tool), doc_(doc), vp_(vp) {
    setWindowTitle(QStringLiteral("Properties"));
    resize(460, 620);
    auto* lay = new QVBoxLayout(this);
    title_ = new QLabel(this);
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    lay->addWidget(title_);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    lay->addWidget(tabs_, 1);
    auto* general = new QWidget(tabs_);
    tabs_->addTab(general, QStringLiteral("General"));
    auto* generalLay = new QVBoxLayout(general);
    generalLay->setContentsMargins(0, 8, 0, 0);
    auto* form = new QFormLayout();
    class_ = new QComboBox(this);
    class_->addItem(QStringLiteral("Plain geometry"), QString());
    int nClasses = 0;
    const char* const* classes = entityClasses(&nClasses);
    for (int i = 0; i < nClasses; ++i) class_->addItem(QString::fromLatin1(classes[i]), QString::fromLatin1(classes[i]));
    class_->setToolTip(QStringLiteral("func_door opens by itself or by what targets it, func_button fires its target, func_actor runs its own script."));
    form->addRow(QStringLiteral("Class:"), class_);
    name_ = new QLineEdit(this);
    name_->setPlaceholderText(QStringLiteral("what targets call it; a group's name is a namespace"));
    name_->setToolTip(QStringLiteral("A target finds the nearest of this name: inside the same group or component first, then in the whole model. \"front/door\" means the door inside the component named front."));
    form->addRow(QStringLiteral("Name:"), name_);
    start_ = new QComboBox(this);
    form->addRow(QStringLiteral("Starts in:"), start_);
    generalLay->addLayout(form);

    auto* physicsLabel = new QLabel(QStringLiteral("Physics"), this);
    physicsLabel->setFont(bold);
    generalLay->addWidget(physicsLabel);
    auto* phys = new QGridLayout();
    physics_ = new QCheckBox(QStringLiteral("Falls and slides in Game Mode"), this);
    physics_->setToolTip(QStringLiteral("A rigid body: it falls, slides, tumbles, and the character can push it. Plain geometry can have it too."));
    auto number = [this](double lo, double hi, double step, const QString& suffix) {
        auto* b = new QDoubleSpinBox(this);
        b->setRange(lo, hi);
        b->setSingleStep(step);
        b->setDecimals(2);
        b->setSuffix(suffix);
        b->setKeyboardTracking(false);
        return b;
    };
    mass_ = number(0.01, 1000000, 5, QStringLiteral(" kg"));
    mass_->setToolTip(QStringLiteral("Heavy things resist being pushed and shove light ones aside; what they fall like does not change."));
    friction_ = number(0, 2, 0.05, QString());
    friction_->setToolTip(QStringLiteral("0 slides like ice; 1 grips like rubber"));
    bounce_ = number(0, 1, 0.05, QString());
    phys->addWidget(physics_, 0, 0, 1, 6);
    phys->addWidget(new QLabel(QStringLiteral("Mass:"), this), 1, 0);
    phys->addWidget(mass_, 1, 1);
    phys->addWidget(new QLabel(QStringLiteral("Friction:"), this), 1, 2);
    phys->addWidget(friction_, 1, 3);
    phys->addWidget(new QLabel(QStringLiteral("Bounce:"), this), 1, 4);
    phys->addWidget(bounce_, 1, 5);
    generalLay->addLayout(phys);
    connect(physics_, &QCheckBox::toggled, this, [this](bool on) { edit("Physics", [&](EntityProps& e) { e.physics = on; }); });
    connect(mass_, &QDoubleSpinBox::valueChanged, this, [this](double v) { edit("Mass", [&](EntityProps& e) { e.mass = v; }); });
    connect(friction_, &QDoubleSpinBox::valueChanged, this, [this](double v) { edit("Friction", [&](EntityProps& e) { e.friction = v; }); });
    connect(bounce_, &QDoubleSpinBox::valueChanged, this, [this](double v) { edit("Bounce", [&](EntityProps& e) { e.bounce = v; }); });

    auto* statesLabel = new QLabel(QStringLiteral("Animation states"), this);
    statesLabel->setFont(bold);
    generalLay->addWidget(statesLabel);
    auto* row = new QHBoxLayout();
    states_ = new QListWidget(this);
    states_->setMaximumHeight(110);
    row->addWidget(states_, 1);
    auto* col = new QVBoxLayout();
    record_ = new QPushButton(QStringLiteral("Record (F5)"), this);
    record_->setToolTip(QStringLiteral("Saves the geometry of the open component as a state"));
    goTo_ = new QPushButton(QStringLiteral("Go To"), this);
    goTo_->setToolTip(QStringLiteral("Puts the geometry in the selected state (also double-click)"));
    rename_ = new QPushButton(QStringLiteral("Rename…"), this);
    remove_ = new QPushButton(QStringLiteral("Delete"), this);
    for (QPushButton* b : {record_, goTo_, rename_, remove_}) col->addWidget(b);
    col->addStretch(1);
    row->addLayout(col);
    generalLay->addLayout(row);

    auto* scriptLabel = new QLabel(QStringLiteral("Script (JavaScript)"), this);
    scriptLabel->setFont(bold);
    generalLay->addWidget(scriptLabel);
    auto* pre = new QHBoxLayout();
    preset_ = new QComboBox(this);
    pre->addWidget(preset_, 1);
    auto* savePreset = new QPushButton(QStringLiteral("Save as Preset…"), this);
    pre->addWidget(savePreset);
    auto* check = new QPushButton(QStringLiteral("Check"), this);
    check->setToolTip(QStringLiteral("Looks for syntax errors (Ctrl+Return); also applies the script"));
    pre->addWidget(check);
    generalLay->addLayout(pre);
    script_ = new QPlainTextEdit(this);
    script_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    script_->setLineWrapMode(QPlainTextEdit::NoWrap);
    script_->setTabStopDistance(script_->fontMetrics().horizontalAdvance(' ') * 4);
    generalLay->addWidget(script_, 1);

    // --- Targets: what firing this entity sets off, and when.
    auto* targetsPage = new QWidget(tabs_);
    auto* tl = new QVBoxLayout(targetsPage);
    tl->setContentsMargins(0, 8, 0, 0);
    auto* targetsHint = new QLabel(QStringLiteral("self.fire() triggers each of these, after its delay. A name means the nearest: in the same group first, then the whole model; \"front/door\" reaches into the component named front. A door a target names opens only that way."), targetsPage);
    targetsHint->setWordWrap(true);
    tl->addWidget(targetsHint);
    targets_ = new QTableWidget(0, 3, targetsPage);
    targets_->setHorizontalHeaderLabels({QStringLiteral("Target name"), QStringLiteral("Delay"), QString()});
    targets_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    targets_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    targets_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    targets_->verticalHeader()->hide();
    targets_->setSelectionMode(QAbstractItemView::NoSelection);
    tl->addWidget(targets_, 1);
    auto* addTargetBtn = new QPushButton(QStringLiteral("Add Target"), targetsPage);
    tl->addWidget(addTargetBtn);
    tabs_->addTab(targetsPage, QStringLiteral("Targets"));
    connect(addTargetBtn, &QPushButton::clicked, this, [this] {
        const QStringList names = otherNames();
        addTarget(names.isEmpty() ? std::string() : names.first().toStdString(), 0);
    });

    // --- Sources: who fires this entity.
    auto* sourcesPage = new QWidget(tabs_);
    auto* sl = new QVBoxLayout(sourcesPage);
    sl->setContentsMargins(0, 8, 0, 0);
    sourcesHint_ = new QLabel(sourcesPage);
    sourcesHint_->setWordWrap(true);
    sl->addWidget(sourcesHint_);
    sources_ = new QTableWidget(0, 4, sourcesPage);
    sources_->setHorizontalHeaderLabels({QStringLiteral("Source"), QStringLiteral("Class"), QStringLiteral("Delay"), QString()});
    sources_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    sources_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    sources_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    sources_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    sources_->verticalHeader()->hide();
    sources_->setSelectionMode(QAbstractItemView::NoSelection);
    sources_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sl->addWidget(sources_, 1);
    auto* addRow = new QHBoxLayout();
    addSource_ = new QComboBox(sourcesPage);
    addRow->addWidget(addSource_, 1);
    auto* addSourceBtn = new QPushButton(QStringLiteral("Add Source"), sourcesPage);
    addRow->addWidget(addSourceBtn);
    sl->addLayout(addRow);
    tabs_->addTab(sourcesPage, QStringLiteral("Sources"));
    connect(addSourceBtn, &QPushButton::clicked, this, [this] {
        if (addSource_->currentIndex() >= 0) addSource(addSource_->currentData().toInt());
    });

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setText(QStringLiteral("self.tween(state, seconds), self.play([states], {duration, loop}), self.fire(), on(\"use\" | \"touch\" | \"trigger\" | \"start\" | \"tick\", fn), after(seconds, fn)"));
    message_->setStyleSheet(QStringLiteral("color: #555;"));
    lay->addWidget(message_);

    connect(class_, &QComboBox::activated, this, [this] { setClass(class_->currentData().toString()); });
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        const std::string n = name_->text().trimmed().toStdString();
        if (instance() && n != instance()->entity.targetname) edit("Entity Name", [&](EntityProps& e) { e.targetname = n; });
    });
    connect(start_, &QComboBox::activated, this, [this] {
        const std::string n = start_->currentText().toStdString();
        edit("Start State", [&](EntityProps& e) { e.startState = n; });
    });
    connect(record_, &QPushButton::clicked, this, [this] { emit recordRequested(); });
    auto goTo = [this] {
        const auto* it = states_->currentItem();
        ComponentDef* def = definition();
        if (!it || !def) return;
        doc_->beginOp("Go To State");
        applyState(doc_->state, instance()->def, it->text().toStdString());
        doc_->commitOp();
        vp_->update();
    };
    connect(goTo_, &QPushButton::clicked, this, goTo);
    connect(states_, &QListWidget::itemDoubleClicked, this, goTo);
    connect(states_, &QListWidget::currentRowChanged, this, [this](int r) {
        goTo_->setEnabled(r >= 0);
        rename_->setEnabled(r >= 0);
        remove_->setEnabled(r >= 0);
    });
    connect(rename_, &QPushButton::clicked, this, [this] {
        const auto* it = states_->currentItem();
        ComponentDef* def = definition();
        if (!it || !def) return;
        const QString old = it->text();
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename State"), QStringLiteral("Name:"), QLineEdit::Normal, old, &ok).trimmed();
        if (!ok || name.isEmpty() || name == old || findState(*def, name.toStdString()) >= 0) return;
        const int i = findState(*def, old.toStdString());
        doc_->beginOp("Rename State");
        definition()->states[i].name = name.toStdString();
        if (instance()->entity.startState == old.toStdString()) instance()->entity.startState = name.toStdString();
        doc_->commitOp();
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const auto* it = states_->currentItem();
        ComponentDef* def = definition();
        if (!it || !def) return;
        const int i = findState(*def, it->text().toStdString());
        doc_->beginOp("Delete State");
        definition()->states.erase(definition()->states.begin() + i);
        doc_->commitOp();
    });
    connect(preset_, &QComboBox::activated, this, [this] {
        if (preset_->currentIndex() > 0) choosePreset(preset_->currentText());
        preset_->setCurrentIndex(0);
    });
    connect(savePreset, &QPushButton::clicked, this, [this] {
        applyScript();
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Save as Preset"), QStringLiteral("Preset name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString path = saveScriptPreset(name, class_->currentData().toString(), script_->toPlainText());
        message_->setText(path.isEmpty() ? QStringLiteral("Could not save the preset.") : QStringLiteral("Saved to %1").arg(path));
        fillPresets();
    });
    auto doCheck = [this] {
        applyScript();
        QString err;
        message_->setText(checkEntityScript(script_->toPlainText(), &err) ? QStringLiteral("No syntax errors.") : err);
    };
    connect(check, &QPushButton::clicked, this, doCheck);
    auto* run = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), script_);
    connect(run, &QShortcut::activated, this, doCheck);
}

QString EntityProperties::message() const { return message_->text(); }

void EntityProperties::setInstance(Id instance) {
    if (instance != inst_) applyScript();
    inst_ = instance;
    script_->clear();
    refresh();
}

void EntityProperties::applyScript() {
    if (updating_ || !instance() || !instance()->entity.isEntity()) return;
    const std::string text = script_->toPlainText().toStdString();
    if (text != instance()->entity.script) edit("Edit Script", [&](EntityProps& e) { e.script = text; });
}

void EntityProperties::setClass(const QString& classname) {
    const std::string c = classname.toStdString();
    if (!instance() || c == instance()->entity.classname) return;
    const QString oldDefault = defaultScript(instance()->entity.classname);
    edit("Entity Class", [&](EntityProps& e) {
        // A script nobody touched follows the class; one's own work stays.
        if (e.script.empty() || QString::fromStdString(e.script) == oldDefault) e.script = defaultScript(c).toStdString();
        e.classname = c;
    });
}

void EntityProperties::setPhysics(bool on, double mass) {
    edit("Physics", [&](EntityProps& e) {
        e.physics = on;
        e.mass = mass;
    });
}

void EntityProperties::choosePreset(const QString& name) {
    for (const ScriptPreset& p : scriptPresets())
        if (p.name == name) {
            edit("Choose Preset", [&](EntityProps& e) { e.script = p.code.toStdString(); });
            return;
        }
}

void EntityProperties::fillPresets() {
    const QString cls = class_->currentData().toString();
    preset_->clear();
    preset_->addItem(QStringLiteral("Use a preset…"));
    for (const ScriptPreset& p : scriptPresets())
        if (p.classname.isEmpty() || p.classname == cls) preset_->addItem(p.name);
}

void EntityProperties::refresh() {
    if (doc_->inOp()) return;
    updating_ = true;
    Instance* in = instance();
    ComponentDef* def = definition();
    setEnabled(in != nullptr);
    if (!in || !def) {
        title_->setText(QStringLiteral("The component is gone."));
        updating_ = false;
        return;
    }
    title_->setText(QString::fromStdString(def->name));
    const EntityProps& e = in->entity;
    class_->setCurrentIndex(std::max(0, class_->findData(QString::fromStdString(e.classname))));
    if (!name_->hasFocus()) name_->setText(QString::fromStdString(e.targetname));
    const bool isEntity = e.isEntity();
    for (QWidget* w : std::initializer_list<QWidget*>{start_, preset_, script_}) w->setEnabled(isEntity);
    physics_->setChecked(e.physics);
    mass_->setValue(e.mass);
    friction_->setValue(e.friction);
    bounce_->setValue(e.bounce);
    for (QWidget* w : std::initializer_list<QWidget*>{mass_, friction_, bounce_}) w->setEnabled(e.physics);

    refreshTargets();
    refreshSources();
    start_->clear();
    states_->blockSignals(true);
    const QString currentState = states_->currentItem() ? states_->currentItem()->text() : QString();
    states_->clear();
    for (const AnimState& s : def->states) {
        const QString n = QString::fromStdString(s.name);
        start_->addItem(n);
        states_->addItem(n);
        if (n == currentState) states_->setCurrentRow(states_->count() - 1);
    }
    states_->blockSignals(false);
    const bool sel = states_->currentRow() >= 0;
    goTo_->setEnabled(sel);
    rename_->setEnabled(sel);
    remove_->setEnabled(sel);
    start_->setCurrentText(QString::fromStdString(e.startState));
    if (e.startState.empty()) start_->setCurrentIndex(0);
    const bool open = doc_->state.editing != kNone && doc_->state.instances[doc_->state.editing].def == in->def;
    record_->setEnabled(open);
    record_->setToolTip(open ? QStringLiteral("Saves the geometry of the open component as a state")
                             : QStringLiteral("Open the component first: double-click it in the model"));
    fillPresets();
    if (isEntity && def->states.size() < 2)
        message_->setText(open ? QStringLiteral("Press F5 to save this state, move or rotate the geometry, press F5 again.")
                               : QStringLiteral("It needs two states to move: double-click the component, press F5, move or rotate the geometry, press F5 again."));
    else if (message_->text().startsWith(QStringLiteral("Press F5")) || message_->text().startsWith(QStringLiteral("It needs")))
        message_->setText(QStringLiteral("Ready: try it in Game Mode (Ctrl+G)."));
    if (!script_->hasFocus() && script_->toPlainText().toStdString() != e.script) script_->setPlainText(QString::fromStdString(e.script));
    updating_ = false;
}

QStringList EntityProperties::otherNames() {
    QStringList names;
    for (Id i = 0; i < (Id)doc_->state.instances.size(); ++i) {
        const Instance& o = doc_->state.instances[i];
        const QString n = QString::fromStdString(o.entity.targetname);
        if (i != inst_ && o.alive && !n.isEmpty() && !names.contains(n)) names << n;
    }
    names.sort();
    return names;
}

void EntityProperties::addTarget(const std::string& name, double delay) {
    edit("Add Target", [&](EntityProps& e) { e.targets.push_back({name, std::max(delay, 0.0)}); });
}

void EntityProperties::setTarget(int row, const std::string& name, double delay) {
    Instance* in = instance();
    if (!in || row < 0 || row >= (int)in->entity.targets.size()) return;
    const EntityTarget next{name, std::max(delay, 0.0)};
    if (in->entity.targets[row] == next) return;
    edit("Edit Target", [&](EntityProps& e) { e.targets[row] = next; });
}

void EntityProperties::removeTarget(int row) {
    Instance* in = instance();
    if (!in || row < 0 || row >= (int)in->entity.targets.size()) return;
    edit("Remove Target", [&](EntityProps& e) { e.targets.erase(e.targets.begin() + row); });
}

std::vector<Id> EntityProperties::sources() {
    std::vector<Id> out;
    Instance* in = instance();
    if (!in || in->entity.targetname.empty()) return out;
    for (Id i = 0; i < (Id)doc_->state.instances.size(); ++i) {
        const Instance& o = doc_->state.instances[i];
        if (i == inst_ || !o.alive) continue;
        for (const EntityTarget& t : o.entity.targets)
            if (t.name == in->entity.targetname) {
                out.push_back(i);
                break;
            }
    }
    return out;
}

void EntityProperties::addSource(Id source) {
    Instance* in = instance();
    if (!in || source < 0 || source >= (Id)doc_->state.instances.size() || source == inst_) return;
    doc_->beginOp("Add Source");
    // A source refers to this one by name, so it needs one.
    if (instance()->entity.targetname.empty()) {
        std::set<std::string> taken;
        for (const Instance& o : doc_->state.instances) taken.insert(o.entity.targetname);
        std::string base = instance()->entity.classname.empty() ? std::string("entity") : instance()->entity.classname;
        std::string name;
        for (int k = 1; name.empty() || taken.count(name); ++k) name = base + std::to_string(k);
        instance()->entity.targetname = name;
    }
    EntityProps& from = doc_->state.instances[source].entity;
    bool has = false;
    for (const EntityTarget& t : from.targets) has = has || t.name == instance()->entity.targetname;
    if (!has) from.targets.push_back({instance()->entity.targetname, 0});
    doc_->commitOp();
}

void EntityProperties::removeSource(Id source) {
    Instance* in = instance();
    if (!in || source < 0 || source >= (Id)doc_->state.instances.size()) return;
    const std::string name = in->entity.targetname;
    doc_->beginOp("Remove Source");
    std::erase_if(doc_->state.instances[source].entity.targets, [&](const EntityTarget& t) { return t.name == name; });
    doc_->commitOp();
}

int EntityProperties::targetRows() const { return targets_->rowCount(); }
int EntityProperties::sourceRows() const { return sources_->rowCount(); }

void EntityProperties::refreshTargets() {
    Instance* in = instance();
    // Not under the cursor of someone typing in it.
    QWidget* focus = focusWidget();
    if (focus && targets_->isAncestorOf(focus) && in && targets_->rowCount() == (int)in->entity.targets.size()) return;
    targets_->setRowCount(0);
    if (!in) return;
    const QStringList names = otherNames();
    for (int r = 0; r < (int)in->entity.targets.size(); ++r) {
        const EntityTarget& t = in->entity.targets[r];
        targets_->insertRow(r);
        auto* name = new QComboBox(targets_);
        name->setEditable(true);
        name->setInsertPolicy(QComboBox::NoInsert);
        name->addItems(names);
        name->setCurrentText(QString::fromStdString(t.name));
        auto* delay = new QDoubleSpinBox(targets_);
        delay->setRange(0, 3600);
        delay->setDecimals(2);
        delay->setSingleStep(0.25);
        delay->setSuffix(QStringLiteral(" s"));
        delay->setKeyboardTracking(false);
        delay->setValue(t.delay);
        auto* remove = new QToolButton(targets_);
        remove->setText(QStringLiteral("✕"));
        remove->setToolTip(QStringLiteral("Remove this target"));
        targets_->setCellWidget(r, 0, name);
        targets_->setCellWidget(r, 1, delay);
        targets_->setCellWidget(r, 2, remove);
        auto commit = [this, r, name, delay] { setTarget(r, name->currentText().trimmed().toStdString(), delay->value()); };
        connect(name, &QComboBox::activated, this, commit);
        connect(name->lineEdit(), &QLineEdit::editingFinished, this, commit);
        connect(delay, &QDoubleSpinBox::valueChanged, this, commit);
        // Later: the table is rebuilt by what this does.
        connect(remove, &QToolButton::clicked, this, [this, r] { QTimer::singleShot(0, this, [this, r] { removeTarget(r); }); });
    }
    tabs_->setTabText(1, in->entity.targets.empty() ? QStringLiteral("Targets") : QStringLiteral("Targets (%1)").arg(in->entity.targets.size()));
}

void EntityProperties::refreshSources() {
    Instance* in = instance();
    sources_->setRowCount(0);
    addSource_->clear();
    if (!in) return;
    auto label = [&](Id i) {
        const Instance& o = doc_->state.instances[i];
        const QString name = QString::fromStdString(o.entity.targetname);
        return name.isEmpty() ? QString::fromStdString(doc_->state.defs[o.def].name) : name;
    };
    const std::vector<Id> list = sources();
    for (Id s : list) {
        const Instance& o = doc_->state.instances[s];
        for (const EntityTarget& t : o.entity.targets) {
            if (t.name != in->entity.targetname) continue;
            const int r = sources_->rowCount();
            sources_->insertRow(r);
            sources_->setItem(r, 0, new QTableWidgetItem(label(s)));
            sources_->setItem(r, 1, new QTableWidgetItem(QString::fromStdString(o.entity.classname)));
            sources_->setItem(r, 2, new QTableWidgetItem(QStringLiteral("%1 s").arg(t.delay)));
            auto* remove = new QToolButton(sources_);
            remove->setText(QStringLiteral("✕"));
            remove->setToolTip(QStringLiteral("This one no longer fires it"));
            sources_->setCellWidget(r, 3, remove);
            connect(remove, &QToolButton::clicked, this, [this, s] { QTimer::singleShot(0, this, [this, s] { removeSource(s); }); });
        }
    }
    for (Id i = 0; i < (Id)doc_->state.instances.size(); ++i) {
        const Instance& o = doc_->state.instances[i];
        if (i != inst_ && o.alive && o.entity.isEntity() && std::find(list.begin(), list.end(), i) == list.end())
            addSource_->addItem(QStringLiteral("%1 (%2)").arg(label(i), QString::fromStdString(o.entity.classname)), i);
    }
    sourcesHint_->setText(list.empty() ? QStringLiteral("Nothing fires this entity. Add a source to have, say, a button fire it; it then needs a name, which is given if missing.")
                                       : QStringLiteral("These fire this entity; it no longer reacts to touch or a press of its own unless it is a button."));
    tabs_->setTabText(2, list.empty() ? QStringLiteral("Sources") : QStringLiteral("Sources (%1)").arg(list.size()));
}

void EntityProperties::closeEvent(QCloseEvent* e) {
    applyScript();
    QWidget::closeEvent(e);
}

} // namespace gs
