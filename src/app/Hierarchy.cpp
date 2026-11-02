// SPDX-License-Identifier: GPL-3.0-or-later
#include "Hierarchy.h"

#include "Icons.h"
#include "Tool.h"
#include "Viewport.h"
#include "core/Components.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <map>

namespace gs {

namespace {
enum Role { KindRole = Qt::UserRole, IdRole, PathRole, KeyRole };
enum Kind { GeometryKind, InstanceKind, ObjectKind };

QString instanceLabel(const ModelState& st, Id i) {
    const Instance& I = st.instances[i];
    const ComponentDef& d = st.defs[I.def];
    QString s = I.entity.targetname.empty() ? QString::fromStdString(d.name) : QString::fromStdString(I.entity.targetname);
    QStringList tags;
    if (I.entity.isEntity()) tags << QString::fromStdString(I.entity.classname);
    if (I.entity.physics) tags << QStringLiteral("physics");
    if (!d.prefab.empty()) tags << QStringLiteral("prefab");
    if (!tags.isEmpty()) s += QStringLiteral("  [%1]").arg(tags.join(QStringLiteral(", ")));
    return s;
}
} // namespace

HierarchyPanel::HierarchyPanel(Document* doc, Viewport* vp, QAction* ungroup, QAction* erase, QAction* properties, QWidget* parent)
    : QWidget(parent), doc_(doc), vp_(vp), ungroup_(ungroup), erase_(erase), properties_(properties) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search…"));
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Show")});
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setExpandsOnDoubleClick(false);
    tree_->installEventFilter(this);
    lay->addWidget(tree_, 1);
    auto* row = new QHBoxLayout();
    row->setSpacing(3);
    group_ = new QPushButton(QStringLiteral("Group"), this);
    group_->setToolTip(QStringLiteral("Put the selected things into a new group, which is a namespace for the names inside it"));
    auto* ungroupBtn = new QPushButton(QStringLiteral("Ungroup"), this);
    ungroupBtn->setToolTip(QStringLiteral("Take the selected group or component apart"));
    auto* propsBtn = new QPushButton(QStringLiteral("Properties"), this);
    row->addWidget(group_);
    row->addWidget(ungroupBtn);
    row->addWidget(propsBtn);
    lay->addLayout(row);

    connect(search_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] { treeSelectionChanged(); });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int c) { itemChanged(it, c); });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) { itemDoubleClicked(it); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) { contextMenu(p); });
    connect(group_, &QPushButton::clicked, this, [this] { groupSelection(); });
    connect(ungroupBtn, &QPushButton::clicked, ungroup_, &QAction::trigger);
    connect(propsBtn, &QPushButton::clicked, properties_, &QAction::trigger);
    refresh();
}

QString HierarchyPanel::structure() const {
    // All a row shows. Unchanged, the tree stays as it is, expanded parts and all.
    const ModelState& st = doc_->state;
    QString s = QStringLiteral("%1,%2;").arg(st.mesh.aliveFaceCount()).arg(st.mesh.aliveEdgeCount());
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        const Instance& I = st.instances[i];
        if (I.alive) s += QStringLiteral("%1:%2:%3:%4:%5;").arg(i).arg(I.def).arg(I.parent).arg(I.hidden).arg(instanceLabel(st, i));
    }
    for (Id i = 0; i < (Id)st.objects.size(); ++i) {
        const SceneObject& o = st.objects[i];
        if (o.alive) s += QStringLiteral("o%1:%2:%3;").arg(i).arg(QString::fromStdString(o.name)).arg(o.hidden);
    }
    bool anyVisible = false;
    for (const Face& f : st.mesh.faces) anyVisible = anyVisible || (f.alive && !f.hidden);
    for (const Edge& e : st.mesh.edges) anyVisible = anyVisible || (e.alive && !e.hidden);
    return s + (anyVisible ? QStringLiteral("v") : QStringLiteral("h"));
}

void HierarchyPanel::rebuild() {
    const ModelState& st = doc_->state;
    // Keep what was expanded; new groups start open.
    QSet<QString> collapsed;
    std::function<void(QTreeWidgetItem*)> note = [&](QTreeWidgetItem* it) {
        if (!it->isExpanded() && it->childCount() > 0) collapsed.insert(it->data(0, KeyRole).toString());
        for (int k = 0; k < it->childCount(); ++k) note(it->child(k));
    };
    for (int k = 0; k < tree_->topLevelItemCount(); ++k) note(tree_->topLevelItem(k));
    tree_->clear();

    std::map<Id, std::vector<Id>> kids;
    for (Id i = 0; i < (Id)st.instances.size(); ++i) {
        const Instance& I = st.instances[i];
        if (I.alive && I.def >= 0 && I.def < (Id)st.defs.size()) kids[I.parent].push_back(i);
    }
    auto fill = [](QTreeWidgetItem* it, const QString& name, bool visible, Kind kind, int id, const QString& key, const QList<int>& path) {
        it->setText(0, name);
        it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        it->setCheckState(1, visible ? Qt::Checked : Qt::Unchecked);
        it->setData(0, KindRole, kind);
        it->setData(0, IdRole, id);
        it->setData(0, KeyRole, key);
        QVariantList p;
        for (int v : path) p << v;
        it->setData(0, PathRole, p);
    };
    std::function<void(QTreeWidgetItem*, Id, QList<int>, QString, int)> addInst = [&](QTreeWidgetItem* parent, Id i, QList<int> path, QString key, int depth) {
        const Instance& I = st.instances[i];
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        key += QStringLiteral("/%1").arg(i);
        fill(it, instanceLabel(st, i), !I.hidden, InstanceKind, i, key, path);
        if (I.entity.isEntity()) it->setIcon(0, Icons::get("character"));
        if (depth > 32) return;
        path << i;
        for (Id c : kids[I.def]) addInst(it, c, path, key, depth + 1);
        it->setExpanded(!collapsed.contains(key));
    };
    if (st.mesh.aliveFaceCount() > 0 || st.mesh.aliveEdgeCount() > 0) {
        auto* g = new QTreeWidgetItem(tree_);
        bool visible = false;
        for (const Face& f : st.mesh.faces) visible = visible || (f.alive && !f.hidden);
        for (const Edge& e : st.mesh.edges) visible = visible || (e.alive && !e.hidden);
        fill(g, QStringLiteral("Geometry (%1 faces)").arg(st.mesh.aliveFaceCount()), visible, GeometryKind, -1, QStringLiteral("g"), {});
        g->setFlags(g->flags() & ~Qt::ItemIsEditable);
    }
    for (Id i : kids[kNone]) addInst(nullptr, i, {}, QString(), 0);
    for (Id i = 0; i < (Id)st.objects.size(); ++i) {
        const SceneObject& o = st.objects[i];
        if (!o.alive) continue;
        auto* it = new QTreeWidgetItem(tree_);
        fill(it, QString::fromStdString(o.name), !o.hidden, ObjectKind, i, QStringLiteral("o%1").arg(i), {});
        const ToolId tool = o.kind == SceneObject::SpotLight ? ToolId::PlaceSpotLight
                          : o.kind == SceneObject::DirectionalLight ? ToolId::PlaceDirectionalLight
                          : o.kind == SceneObject::ViewCamera ? ToolId::PlaceCamera : ToolId::PlacePointLight;
        it->setIcon(0, Icons::get(Icons::toolIconName(tool)));
    }
    applyFilter();
}

void HierarchyPanel::applyFilter() {
    const QString text = search_->text().trimmed();
    std::function<bool(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        bool show = text.isEmpty() || it->text(0).contains(text, Qt::CaseInsensitive);
        for (int k = 0; k < it->childCount(); ++k) show = walk(it->child(k)) || show;
        it->setHidden(!show);
        return show;
    };
    for (int k = 0; k < tree_->topLevelItemCount(); ++k) walk(tree_->topLevelItem(k));
}

void HierarchyPanel::syncSelection() {
    // The rows of what is selected, at the level being edited.
    QList<int> open;
    for (Id o : doc_->state.outerOpen) open << o;
    if (doc_->state.editing != kNone) open << doc_->state.editing;
    const Selection& s = doc_->selection;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        const int kind = it->data(0, KindRole).toInt(), id = it->data(0, IdRole).toInt();
        bool on = false;
        if (kind == ObjectKind) {
            on = s.objects.count(id) > 0;
        } else if (kind == InstanceKind) {
            QList<int> path;
            for (const QVariant& v : it->data(0, PathRole).toList()) path << v.toInt();
            on = s.instances.count(id) > 0 && path == open;
        }
        if (it->isSelected() != on && kind != GeometryKind) it->setSelected(on);
        if (on) tree_->scrollToItem(it);
        for (int k = 0; k < it->childCount(); ++k) walk(it->child(k));
    };
    for (int k = 0; k < tree_->topLevelItemCount(); ++k) walk(tree_->topLevelItem(k));
}

void HierarchyPanel::refresh() {
    if (updating_ || doc_->inOp()) return;
    updating_ = true;
    {
        const QSignalBlocker block(tree_);
        const QString now = structure();
        if (now != shown_) {
            shown_ = now;
            rebuild();
        }
        syncSelection();
    }
    updating_ = false;
    group_->setEnabled(doc_->selection.count() >= 1 && doc_->selection.objects.empty());
}

void HierarchyPanel::openPathOf(const QList<int>& path) {
    QList<int> open;
    for (Id o : doc_->state.outerOpen) open << o;
    if (doc_->state.editing != kNone) open << doc_->state.editing;
    if (open == path) return;
    // Out to what the two share, then in.
    int same = 0;
    while (same < open.size() && same < path.size() && open[same] == path[same]) ++same;
    for (int k = (int)open.size(); k > same; --k) doc_->closeComponent();
    for (int k = same; k < path.size(); ++k) doc_->openComponent(path[k]);
}

void HierarchyPanel::treeSelectionChanged() {
    if (updating_ || doc_->inOp()) return;
    const QList<QTreeWidgetItem*> items = tree_->selectedItems();
    updating_ = true;
    if (vp_->tool() && vp_->tool()->busy()) vp_->cancelTool();
    Selection s;
    QList<int> level;
    bool haveLevel = false;
    Mesh* top = nullptr;
    for (QTreeWidgetItem* it : items) {
        const int kind = it->data(0, KindRole).toInt(), id = it->data(0, IdRole).toInt();
        QList<int> path;
        for (const QVariant& v : it->data(0, PathRole).toList()) path << v.toInt();
        if (!haveLevel) {
            // The first row decides the level; a selection cannot span levels.
            haveLevel = true;
            level = path;
            openPathOf(path);
            top = &doc_->state.activeMesh();
        }
        if (path != level) continue;
        if (kind == InstanceKind) s.instances.insert(id);
        else if (kind == ObjectKind && level.isEmpty()) s.objects.insert(id);
        else if (kind == GeometryKind && top) {
            for (Id f = 0; f < (Id)top->faces.size(); ++f)
                if (top->faces[f].alive && !top->faces[f].hidden) s.faces.insert(f);
            for (Id e = 0; e < (Id)top->edges.size(); ++e)
                if (top->edges[e].alive && !top->edges[e].hidden) s.edges.insert(e);
        }
    }
    doc_->selection = s;
    doc_->notifySelection();
    updating_ = false;
    const QSignalBlocker block(tree_);
    updating_ = true;
    syncSelection();
    updating_ = false;
    group_->setEnabled(doc_->selection.count() >= 1 && doc_->selection.objects.empty());
    vp_->update();
}

void HierarchyPanel::itemChanged(QTreeWidgetItem* it, int column) {
    if (updating_) return;
    const int kind = it->data(0, KindRole).toInt(), id = it->data(0, IdRole).toInt();
    ModelState& st = doc_->state;
    if (column == 1) {
        const bool visible = it->checkState(1) == Qt::Checked;
        doc_->beginOp(visible ? "Show" : "Hide");
        if (kind == InstanceKind) {
            st.instances[id].hidden = !visible;
        } else if (kind == ObjectKind) {
            st.objects[id].hidden = !visible;
        } else {
            for (Face& f : st.mesh.faces) f.hidden = !visible;
            for (Edge& e : st.mesh.edges) e.hidden = !visible;
            st.mesh.touch();
        }
        // Not left selected while out of sight.
        if (!visible) {
            if (kind == InstanceKind) doc_->selection.instances.erase(id);
            if (kind == ObjectKind) doc_->selection.objects.erase(id);
            if (kind == GeometryKind && st.editing == kNone) {
                doc_->selection.faces.clear();
                doc_->selection.edges.clear();
            }
        }
        doc_->commitOp();
        doc_->notifySelection();
        vp_->update();
        return;
    }
    // Renamed.
    QString text = it->text(0).trimmed();
    const int tag = text.indexOf(QStringLiteral("  ["));
    if (tag >= 0) text = text.left(tag);
    const std::string name = text.toStdString();
    if (name.empty() || kind == GeometryKind) {
        shown_.clear();
        refresh();
        return;
    }
    doc_->beginOp("Rename");
    if (kind == ObjectKind) {
        st.objects[id].name = name;
    } else {
        Instance& I = st.instances[id];
        // What targets call it, if it has that; or else the component's own name.
        if (!I.entity.targetname.empty() || I.entity.isEntity()) I.entity.targetname = name;
        else st.defs[I.def].name = name;
    }
    doc_->commitOp();
    shown_.clear();
    refresh();
}

void HierarchyPanel::itemDoubleClicked(QTreeWidgetItem* it) {
    const int kind = it->data(0, KindRole).toInt(), id = it->data(0, IdRole).toInt();
    if (kind == ObjectKind) {
        vp_->lookThrough(id);
    } else if (kind == InstanceKind) {
        doc_->openComponent(id);
        vp_->refreshToolStatus();
    }
}

Id HierarchyPanel::groupSelection(const QString& name) {
    Selection s = doc_->selection;
    s.objects.clear();
    if (s.empty()) return kNone;
    vp_->cancelTool();
    doc_->beginOp("Group");
    const Id group = makeComponent(doc_->state, s, name.toStdString());
    if (group == kNone) {
        doc_->cancelOp();
        return kNone;
    }
    doc_->selection.clear();
    doc_->selection.instances.insert(group);
    doc_->commitOp();
    doc_->notifySelection();
    return group;
}

QTreeWidgetItem* HierarchyPanel::findInstance(Id instance) const {
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        if (!found && it->data(0, KindRole).toInt() == InstanceKind && it->data(0, IdRole).toInt() == instance) found = it;
        for (int k = 0; k < it->childCount(); ++k) walk(it->child(k));
    };
    for (int k = 0; k < tree_->topLevelItemCount(); ++k) walk(tree_->topLevelItem(k));
    return found;
}

QTreeWidgetItem* HierarchyPanel::findObject(Id object) const {
    for (int k = 0; k < tree_->topLevelItemCount(); ++k) {
        QTreeWidgetItem* it = tree_->topLevelItem(k);
        if (it->data(0, KindRole).toInt() == ObjectKind && it->data(0, IdRole).toInt() == object) return it;
    }
    return nullptr;
}

QTreeWidgetItem* HierarchyPanel::geometryRow() const {
    for (int k = 0; k < tree_->topLevelItemCount(); ++k)
        if (tree_->topLevelItem(k)->data(0, KindRole).toInt() == GeometryKind) return tree_->topLevelItem(k);
    return nullptr;
}

void HierarchyPanel::contextMenu(const QPoint& pos) {
    QTreeWidgetItem* it = tree_->itemAt(pos);
    QMenu menu(this);
    menu.addAction(QStringLiteral("Group"), this, [this] { groupSelection(); });
    if (it && it->data(0, KindRole).toInt() == InstanceKind) {
        menu.addAction(ungroup_);
        menu.addAction(properties_);
        menu.addAction(QStringLiteral("Open"), this, [this, it] { itemDoubleClicked(it); });
    }
    if (it && (it->flags() & Qt::ItemIsEditable)) menu.addAction(QStringLiteral("Rename"), this, [this, it] { tree_->editItem(it, 0); });
    if (it) menu.addAction(erase_);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Show Everything"), this, [this] {
        doc_->beginOp("Unhide All");
        for (Instance& i : doc_->state.instances) i.hidden = false;
        for (SceneObject& o : doc_->state.objects) o.hidden = false;
        for (Face& f : doc_->state.mesh.faces) f.hidden = false;
        for (Edge& e : doc_->state.mesh.edges) e.hidden = false;
        doc_->state.mesh.touch();
        doc_->commitOp();
    });
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

bool HierarchyPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == tree_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Delete) {
        erase_->trigger();
        return true;
    }
    return QWidget::eventFilter(o, e);
}

} // namespace gs
