// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch plugin: Open Block Model (.obm), GNUBlocks' model format (ObmFormat.h).
#include "ObmFormat.h"
#include "ObmUi.h"
#include "plugin/FormatPlugin.h"

#include <QApplication>
#include <QObject>

class ObmPlugin : public QObject, public gs::FormatPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID GNUSKETCH_FORMAT_PLUGIN_IID FILE "obm.json")
    Q_INTERFACES(gs::FormatPlugin)

public:
    QString formatName() const override { return QStringLiteral("Open Block Model"); }
    QStringList extensions() const override { return {QStringLiteral("obm")}; }
    QString description() const override {
        return QStringLiteral("GNUBlocks' model format: groups as components, cubes as textured boxes; pictures from the game's "
                              "textures folder. Whatever GNUSketch can't show (tags, props, animations) is written back as it came.");
    }
    bool canImport() const override { return true; }
    bool canExport() const override { return true; }
    bool importFile(gs::Document& doc, const QString& path, QStringList* messages) override {
        std::vector<gs::Id> missing;
        // (with a window to ask in: each picture not found asked about, Fix it; else said)
        const bool windows = qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr;
        if (!gs::obm::importObm(doc, path, messages, windows ? &missing : nullptr)) return false;
        if (!missing.empty()) gs::obm::askMissing(QApplication::activeWindow(), doc, missing, path, messages);
        return true;
    }
    bool exportFile(const gs::Document& doc, const QString& path, QStringList* messages) override {
        return gs::obm::exportObm(doc, path, messages);
    }
    // Properties > OBM: the group's tags, its cubes', the model's; its pictures (Fix it for those not found)
    QString propertiesTabTitle() const override { return QStringLiteral("OBM"); }
    QWidget* createPropertiesTab(gs::Document& doc, QWidget* parent) override { return new gs::obm::ObmTab(&doc, parent); }
    bool showInPropertiesTab(QWidget* page, gs::Document&, gs::Id instance) override {
        auto* tab = qobject_cast<gs::obm::ObmTab*>(page);
        return tab && tab->showFor(instance);
    }
};

#include "ObmPlugin.moc"
