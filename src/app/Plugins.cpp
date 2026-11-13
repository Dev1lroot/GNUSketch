// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Plugins.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPluginLoader>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>

namespace gs {

namespace {
std::vector<LoadedPlugin> gPlugins;
std::vector<PluginInfo> gInstalled;
QStringList gProblems;
bool gLoaded = false;
} // namespace

QStringList Plugins::folders() {
    QStringList out;
    for (const QString& p : qEnvironmentVariable("GNUSKETCH_PLUGIN_PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts)) out << p;
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!data.isEmpty()) out << data + "/plugins";
    const QString exe = QCoreApplication::applicationDirPath();
    out << QDir::cleanPath(exe + "/../lib/gnusketch/plugins");
    out << exe + "/plugins";
    return out;
}

void Plugins::load() {
    if (gLoaded) return;
    gLoaded = true;
    QSet<QString> seen; // (one plugin of each name: the first folder wins)
    for (const QString& folder : folders()) {
        const QDir dir(folder);
        if (!dir.exists()) continue;
#ifdef Q_OS_WIN
        const QStringList pattern = {QStringLiteral("*.dll")};
#elif defined(Q_OS_MACOS)
        const QStringList pattern = {QStringLiteral("*.dylib"), QStringLiteral("*.so")};
#else
        const QStringList pattern = {QStringLiteral("*.so")};
#endif
        for (const QFileInfo& fi : dir.entryInfoList(pattern, QDir::Files, QDir::Name)) {
            if (seen.contains(fi.fileName())) continue;
            QPluginLoader loader(fi.absoluteFilePath());
            // Its details come from the metadata, read without loading it.
            const QJsonObject meta = loader.metaData().value(QStringLiteral("MetaData")).toObject();
            if (loader.metaData().value(QStringLiteral("IID")).toString() != QLatin1String(GNUSKETCH_FORMAT_PLUGIN_IID)) {
                gProblems << QStringLiteral("%1: not a GNUSketch format plugin").arg(fi.absoluteFilePath());
                continue;
            }
            seen.insert(fi.fileName());
            PluginInfo info;
            info.file = fi.absoluteFilePath();
            info.name = meta.value(QStringLiteral("name")).toString(fi.completeBaseName());
            info.vendor = meta.value(QStringLiteral("vendor")).toString(QStringLiteral("unknown"));
            info.license = meta.value(QStringLiteral("license")).toString(QStringLiteral("unknown"));
            info.version = meta.value(QStringLiteral("version")).toString();
            info.description = meta.value(QStringLiteral("description")).toString();
            info.credits = meta.value(QStringLiteral("credits")).toString();
            info.url = meta.value(QStringLiteral("url")).toString();
            for (const auto& e : meta.value(QStringLiteral("extensions")).toArray()) info.extensions << e.toString();
            info.enabled = isEnabled(fi.fileName());
            if (info.enabled) {
                QObject* obj = loader.instance();
                auto* f = obj ? qobject_cast<FormatPlugin*>(obj) : nullptr;
                if (!f) {
                    info.problem = obj ? QStringLiteral("not a GNUSketch format plugin") : loader.errorString();
                    gProblems << QStringLiteral("%1: %2").arg(fi.absoluteFilePath(), info.problem);
                } else {
                    info.loaded = true;
                    info.canImport = f->canImport();
                    info.canExport = f->canExport();
                    if (info.extensions.isEmpty()) info.extensions = f->extensions();
                    if (info.description.isEmpty()) info.description = f->description();
                    gPlugins.push_back({fi.absoluteFilePath(), f});
                }
            }
            gInstalled.push_back(info);
        }
    }
}

const std::vector<PluginInfo>& Plugins::installed() {
    load();
    return gInstalled;
}

bool Plugins::isEnabled(const QString& file) {
    return !QSettings().value(QStringLiteral("plugins/disabled")).toStringList().contains(QFileInfo(file).fileName());
}

void Plugins::setEnabled(const QString& file, bool on) {
    QSettings s;
    QStringList off = s.value(QStringLiteral("plugins/disabled")).toStringList();
    const QString name = QFileInfo(file).fileName();
    off.removeAll(name);
    if (!on) off << name;
    s.setValue(QStringLiteral("plugins/disabled"), off);
}

const std::vector<LoadedPlugin>& Plugins::all() {
    load();
    return gPlugins;
}

const QStringList& Plugins::problems() {
    load();
    return gProblems;
}

FormatPlugin* Plugins::forFile(const QString& path, bool forExport) {
    const QString ext = QFileInfo(path).suffix().toLower();
    for (const LoadedPlugin& p : all())
        if ((forExport ? p.format->canExport() : p.format->canImport()) && p.format->extensions().contains(ext))
            return p.format;
    return nullptr;
}

QString Plugins::fileFilters(bool forExport) {
    QStringList out;
    for (const LoadedPlugin& p : all()) {
        if (!(forExport ? p.format->canExport() : p.format->canImport())) continue;
        QStringList pats;
        for (const QString& e : p.format->extensions()) pats << "*." + e;
        out << QStringLiteral("%1 (%2)").arg(p.format->formatName(), pats.join(' '));
    }
    return out.join(";;");
}

} // namespace gs
