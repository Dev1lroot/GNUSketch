// SPDX-License-Identifier: GPL-3.0-or-later
// Forma plugin: Open Block Model (.obm), GNUBlocks' model format (ObmFormat.h).
#include "ObmFormat.h"
#include "ObmUi.h"
#include "plugin/FormatPlugin.h"

#include <QApplication>
#include <QObject>

class ObmPlugin : public QObject, public forma::FormatPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FORMA_FORMAT_PLUGIN_IID FILE "obm.json")
    Q_INTERFACES(forma::FormatPlugin)

public:
    QString formatName() const override { return QStringLiteral("Open Block Model"); }
    QStringList extensions() const override { return {QStringLiteral("obm")}; }
    QString description() const override {
        return QStringLiteral("GNUBlocks' model format: groups as components, cubes as textured boxes; pictures from the game's "
                              "textures folder. Whatever Forma can't show (tags, props, animations) is written back as it came.");
    }
    bool canImport() const override { return true; }
    bool canExport() const override { return true; }
    bool importFile(forma::Document& doc, const QString& path, QStringList* messages) override {
        std::vector<forma::Id> missing;
        // (with a window to ask in: each picture not found asked about, Fix it; else said)
        const bool windows = qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr;
        if (!forma::obm::importObm(doc, path, messages, windows ? &missing : nullptr)) return false;
        if (!missing.empty()) forma::obm::askMissing(QApplication::activeWindow(), doc, missing, path, messages);
        return true;
    }
    bool exportFile(const forma::Document& doc, const QString& path, QStringList* messages) override {
        return forma::obm::exportObm(doc, path, messages);
    }
    // Properties > OBM: the group's tags, its cubes', the model's; its pictures (Fix it for those not found)
    QString propertiesTabTitle() const override { return QStringLiteral("OBM"); }
    QWidget* createPropertiesTab(forma::Document& doc, QWidget* parent) override { return new forma::obm::ObmTab(&doc, parent); }
    bool showInPropertiesTab(QWidget* page, forma::Document&, forma::Id instance) override {
        auto* tab = qobject_cast<forma::obm::ObmTab*>(page);
        return tab && tab->showFor(instance);
    }
};

#include "ObmPlugin.moc"
