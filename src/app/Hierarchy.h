// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - everything in the model as a tree: select, group, hide, rename.
#pragma once

#include "core/Document.h"

#include <QWidget>

class QAction;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace gs {

class Viewport;

class HierarchyPanel : public QWidget {
    Q_OBJECT
public:
    HierarchyPanel(Document* doc, Viewport* vp, QAction* ungroup, QAction* erase, QAction* properties, QWidget* parent = nullptr);
    void refresh();
    QTreeWidget* tree() const { return tree_; }
    // Puts what is selected at the level being edited into a new group.
    Id groupSelection(const QString& name = QStringLiteral("Group"));
    // The row of an instance at this path, or null.
    QTreeWidgetItem* findInstance(Id instance) const;
    QTreeWidgetItem* findObject(Id object) const;
    QTreeWidgetItem* geometryRow() const;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void rebuild();
    void syncSelection();
    void applyFilter();
    void treeSelectionChanged();
    void itemChanged(QTreeWidgetItem* it, int column);
    void itemDoubleClicked(QTreeWidgetItem* it);
    void contextMenu(const QPoint& pos);
    // Opens the components around the row, so that it can be selected.
    void openPathOf(const QList<int>& path);
    QString structure() const;

    Document* doc_;
    Viewport* vp_;
    QLineEdit* search_;
    QTreeWidget* tree_;
    QPushButton* group_;
    QAction* ungroup_;
    QAction* erase_;
    QAction* properties_;
    QString shown_;
    bool updating_ = false;
};

} // namespace gs
