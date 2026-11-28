// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - tool plugins: commands that work on the model.
//
// A tool plugin is a shared library (a Qt plugin) that adds a command to the
// Extensions menu: it is given the document (and a window to put a dialog over)
// and changes the model, as Effects do. Writing one:
//
//   class MyTool : public QObject, public forma::ToolPlugin {
//       Q_OBJECT
//       Q_PLUGIN_METADATA(IID FORMA_TOOL_PLUGIN_IID FILE "mytool.json")
//       Q_INTERFACES(forma::ToolPlugin)
//   public:
//       QString commandName() const override { return "My Tool"; }
//       void run(forma::Document& doc, QWidget* parent) override;
//   };
//
// It is built and found as a format plugin is (see FormatPlugin.h).
//
// run() makes its change in an undo step of its own (doc.beginOp / commitOp, or
// cancelOp to leave the model as it was), marks a changed model as such
// (doc.modified = true) and tells about it: doc.notifyChanged() for the model,
// doc.notifySelection() for the selection. Geometry is in meters,
// Z up; the selection is of the level being edited (Document::selection), where
// faces are in doc.mesh() and component transforms are world coordinates.
#pragma once

#include "core/Document.h"

#include <QString>
#include <QtPlugin>

class QWidget;

namespace forma {

class ToolPlugin {
public:
    virtual ~ToolPlugin() = default;

    // The menu entry: "Packing Solver". ("…" is added, since it asks first.)
    virtual QString commandName() const = 0;
    // One line about it: the menu's status tip and Help > Plugins.
    virtual QString description() const { return {}; }
    // Whether it can run on what is selected now; if not, why (shown to the user).
    virtual bool canRun(const Document& doc, QString* why) const {
        (void)doc, (void)why;
        return true;
    }
    virtual void run(Document& doc, QWidget* parent) = 0;
};

} // namespace forma

#define FORMA_TOOL_PLUGIN_IID "software.dev1lroot.Forma.ToolPlugin/1"
Q_DECLARE_INTERFACE(forma::ToolPlugin, FORMA_TOOL_PLUGIN_IID)
