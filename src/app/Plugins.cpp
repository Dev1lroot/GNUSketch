// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Plugins.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>
#include <QSet>
#include <QStandardPaths>

namespace gs {

namespace {
std::vector<LoadedPlugin> gPlugins;
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
            QObject* obj = loader.instance();
            if (!obj) {
                gProblems << QStringLiteral("%1: %2").arg(fi.absoluteFilePath(), loader.errorString());
                continue;
            }
            auto* f = qobject_cast<FormatPlugin*>(obj);
            if (!f) {
                gProblems << QStringLiteral("%1: not a GNUSketch format plugin").arg(fi.absoluteFilePath());
                continue;
            }
            seen.insert(fi.fileName());
            gPlugins.push_back({fi.absoluteFilePath(), f});
        }
    }
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
