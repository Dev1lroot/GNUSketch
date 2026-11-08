// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - finding and loading plugins (see plugin/FormatPlugin.h).
#pragma once

#include "plugin/FormatPlugin.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace gs {

struct LoadedPlugin {
    QString file;                     // the shared library it came from
    FormatPlugin* format = nullptr;   // owned by Qt's plugin loader (lives as long as the program)
};

class Plugins {
public:
    // Looks in every plugin folder once; later calls return at once.
    static void load();
    static const std::vector<LoadedPlugin>& all();
    // The folders looked in, in order, and what went wrong (files that aren't plugins of ours).
    static QStringList folders();
    static const QStringList& problems();
    // The format plugin that reads/writes files with this name's extension (nullptr: none).
    static FormatPlugin* forFile(const QString& path, bool forExport);
    // "Open Block Model (*.obm)" for each that can import (or export); joined by ";;".
    static QString fileFilters(bool forExport);
};

} // namespace gs
