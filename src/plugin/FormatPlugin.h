// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - file format plugins.
//
// A plugin is a shared library (a Qt plugin) that teaches GNUSketch another file
// format: opening it (File > Open, drag and drop, the command line), importing it
// into the model (File > Import) and exporting the model to it (File > Export).
//
// Writing one:
//
//   class MyFormat : public QObject, public gs::FormatPlugin {
//       Q_OBJECT
//       Q_PLUGIN_METADATA(IID GNUSKETCH_FORMAT_PLUGIN_IID FILE "myformat.json")
//       Q_INTERFACES(gs::FormatPlugin)
//   public:
//       QString formatName() const override { return "My Format"; }
//       QStringList extensions() const override { return {"myf"}; }
//       bool canImport() const override { return true; }
//       bool importFile(gs::Document& doc, const QString& path, QStringList* messages) override;
//   };
//
// link it against gs_io (which brings gs_core and Qt), build it as a MODULE
// library and put the .so in one of the folders GNUSketch looks in (Help >
// Plugins lists them, and what was found there):
//
//   * GNUSKETCH_PLUGIN_PATH (folders separated by ':')
//   * ~/.local/share/GNUSketch/GNUSketch/plugins (the user's own)
//   * <prefix>/lib/gnusketch/plugins (installed with GNUSketch)
//   * plugins/ beside the gnusketch executable (a build tree)
//
// The host opens an undo step around an import ("Import <format>"), so a plugin
// only adds to the document; for File > Open it imports into an empty one.
// Geometry is in meters, Z up. What a format keeps that GNUSketch has no place for
// can go in ModelState::extra and ComponentDef::extra (JSON text, saved with the
// model), so that a model opened and exported again loses nothing.
#pragma once

#include "core/Document.h"

#include <QString>
#include <QStringList>
#include <QtPlugin>

class QWidget;

namespace gs {

class FormatPlugin {
public:
    virtual ~FormatPlugin() = default;

    // Shown in menus and file dialogs: "Open Block Model".
    virtual QString formatName() const = 0;
    // File extensions without the dot, lower case: {"obm"}. The first is the one
    // added to an exported file's name.
    virtual QStringList extensions() const = 0;
    // One line about it (Help > Plugins).
    virtual QString description() const { return {}; }

    virtual bool canImport() const { return false; }
    virtual bool canExport() const { return false; }

    // Adds the file's contents to doc. `messages` collects warnings (shown to the
    // user afterwards); on failure, return false with the reason in messages.
    virtual bool importFile(Document& doc, const QString& path, QStringList* messages) {
        (void)doc, (void)path;
        if (messages) messages->append(QStringLiteral("This plugin cannot import"));
        return false;
    }
    // Writes the model (all of it, as File > Export does for other formats).
    virtual bool exportFile(const Document& doc, const QString& path, QStringList* messages) {
        (void)doc, (void)path;
        if (messages) messages->append(QStringLiteral("This plugin cannot export"));
        return false;
    }

    // A tab of its own in a component's Properties window (what the format keeps
    // with it: its tags, say). Its title ("" for none), the page (made once, owned
    // by the window), and what it shows: the component instance shown changed, or
    // the model did. Return false when it has nothing for that one (the tab hides).
    // A page that changes the document does so in an undo step of its own
    // (doc.beginOp / commitOp) and tells it (doc.notifyChanged()).
    virtual QString propertiesTabTitle() const { return {}; }
    virtual QWidget* createPropertiesTab(Document& doc, QWidget* parent) {
        (void)doc, (void)parent;
        return nullptr;
    }
    virtual bool showInPropertiesTab(QWidget* page, Document& doc, Id instance) {
        (void)page, (void)doc, (void)instance;
        return false;
    }
};

} // namespace gs

#define GNUSKETCH_FORMAT_PLUGIN_IID "org.gnusketch.FormatPlugin/2"
Q_DECLARE_INTERFACE(gs::FormatPlugin, GNUSKETCH_FORMAT_PLUGIN_IID)
