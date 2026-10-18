// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - original vector icon set (inline SVG) and tool cursors.
#pragma once

#include "Tool.h"

#include <QCursor>
#include <QIcon>
#include <QString>

namespace gs {

namespace Icons {

QIcon get(const QString& name);
QPixmap pixmap(const QString& name, int size);
QCursor toolCursor(ToolId id);
QString toolIconName(ToolId id);

} // namespace Icons

} // namespace gs
