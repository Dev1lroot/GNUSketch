// SPDX-License-Identifier: GPL-3.0-or-later
#include "Icons.h"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

namespace gs::Icons {

namespace {

// All icons use a 24x24 view box. Drawn from scratch for GNUSketch.
const char* svgFor(const QString& n) {
    static const QHash<QString, const char*> map = {
        {"select", R"svg(<path d="M6 2.5 L6 19 L10 15.2 L12.8 21.5 L15.6 20.3 L12.9 14.2 L18.2 14.2 Z" fill="#111" stroke="#fff" stroke-width="1.1" stroke-linejoin="round"/>)svg"},
        {"eraser", R"svg(<g transform="rotate(-38 12 12)"><rect x="3" y="8" width="18" height="8.5" rx="1.6" fill="#f08a96" stroke="#7a2a33" stroke-width="1"/><rect x="3" y="8" width="6.5" height="8.5" rx="1.4" fill="#f4f1ec" stroke="#7a2a33" stroke-width="1"/><path d="M11 9.2 L19.5 9.2" stroke="#fbd0d6" stroke-width="1.2"/></g>)svg"},
        {"paint", R"svg(<path d="M4.5 10.5 L11 4 L18.5 11.5 L12 18 Z" fill="#cfd8e3" stroke="#334" stroke-width="1"/><path d="M4.5 10.5 L11 4 L13 6 L6.5 12.5 Z" fill="#9fb2c8"/><path d="M11 4 C9 1.5 6 2 5.5 4" fill="none" stroke="#334" stroke-width="1"/><path d="M18.5 11.5 C21 14.5 21.5 17 19.8 18.4 C18.2 19.6 16.8 17.6 17.5 15.5 Z" fill="#2f7de1" stroke="#1a4f96" stroke-width=".8"/><rect x="3" y="19.5" width="10" height="2.5" rx="1" fill="#2f7de1"/>)svg"},
        {"line", R"svg(<g transform="rotate(45 12 12)"><rect x="9.6" y="1.5" width="4.8" height="15" fill="#f2c230" stroke="#6b5310" stroke-width=".9"/><rect x="9.6" y="1.5" width="4.8" height="3" fill="#e57b8c" stroke="#6b5310" stroke-width=".9"/><path d="M9.6 16.5 L12 22 L14.4 16.5 Z" fill="#f4dfb0" stroke="#6b5310" stroke-width=".9"/><path d="M11.2 20.2 L12 22 L12.8 20.2 Z" fill="#222"/></g>)svg"},
        {"freehand", R"svg(<path d="M2.5 18 C5 10 8 22 11 14 C13.5 7 16 16 21.5 6" fill="none" stroke="#d6262e" stroke-width="2" stroke-linecap="round"/><circle cx="2.5" cy="18" r="1.4" fill="#222"/>)svg"},
        {"rectangle", R"svg(<rect x="3.5" y="5.5" width="17" height="13" fill="#dfe6ee" stroke="#222" stroke-width="1.4"/><path d="M3.5 18.5 L20.5 5.5" stroke="#d6262e" stroke-width="1.1" stroke-dasharray="2 1.5"/><circle cx="3.5" cy="18.5" r="1.8" fill="#d6262e"/><circle cx="20.5" cy="5.5" r="1.8" fill="#d6262e"/>)svg"},
        {"circle", R"svg(<circle cx="12" cy="12" r="8.5" fill="#dfe6ee" stroke="#222" stroke-width="1.4"/><path d="M12 12 L20.5 12" stroke="#d6262e" stroke-width="1.2"/><circle cx="12" cy="12" r="1.6" fill="#d6262e"/>)svg"},
        {"polygon", R"svg(<path d="M12 3 L19.8 7.5 L19.8 16.5 L12 21 L4.2 16.5 L4.2 7.5 Z" fill="#dfe6ee" stroke="#222" stroke-width="1.4" stroke-linejoin="round"/><circle cx="12" cy="12" r="1.6" fill="#d6262e"/>)svg"},
        {"arc", R"svg(<path d="M3.5 18 A9 9 0 0 1 20.5 18" fill="none" stroke="#222" stroke-width="1.6"/><path d="M3.5 18 L20.5 18" stroke="#d6262e" stroke-width="1" stroke-dasharray="2 1.5"/><circle cx="3.5" cy="18" r="1.8" fill="#d6262e"/><circle cx="20.5" cy="18" r="1.8" fill="#d6262e"/><circle cx="12" cy="9.5" r="1.4" fill="#2f7de1"/>)svg"},
        {"pushpull", R"svg(<path d="M3 13 L12 9 L21 13 L12 17 Z" fill="#f3f3f3" stroke="#222" stroke-width="1"/><path d="M3 13 L3 18 L12 22 L12 17 Z" fill="#b9c4d1" stroke="#222" stroke-width="1"/><path d="M12 17 L12 22 L21 18 L21 13 Z" fill="#8ea0b5" stroke="#222" stroke-width="1"/><path d="M12 13 L12 3.5" stroke="#d6262e" stroke-width="2.2"/><path d="M8.3 6.5 L12 2 L15.7 6.5 Z" fill="#d6262e"/>)svg"},
        {"move", R"svg(<g fill="#d6262e" stroke="#7d1015" stroke-width=".7" stroke-linejoin="round"><path d="M12 1.5 L15.5 5.5 L13.3 5.5 L13.3 10.7 L18.5 10.7 L18.5 8.5 L22.5 12 L18.5 15.5 L18.5 13.3 L13.3 13.3 L13.3 18.5 L15.5 18.5 L12 22.5 L8.5 18.5 L10.7 18.5 L10.7 13.3 L5.5 13.3 L5.5 15.5 L1.5 12 L5.5 8.5 L5.5 10.7 L10.7 10.7 L10.7 5.5 L8.5 5.5 Z"/></g>)svg"},
        {"rotate", R"svg(<path d="M19 12 A7 7 0 1 1 15.5 5.9" fill="none" stroke="#d6262e" stroke-width="2.6" stroke-linecap="round"/><path d="M13.2 2.2 L20 4.5 L15.4 9.8 Z" fill="#d6262e"/><circle cx="12" cy="12" r="1.6" fill="#222"/>)svg"},
        {"scale", R"svg(<rect x="3" y="9" width="12" height="12" fill="#dfe6ee" stroke="#222" stroke-width="1.2"/><rect x="3" y="3" width="18" height="18" fill="none" stroke="#222" stroke-width="1" stroke-dasharray="2 1.5"/><path d="M10 14 L19 5" stroke="#d6262e" stroke-width="2"/><path d="M14.5 4 L20.5 3.5 L20 9.5 Z" fill="#d6262e"/><rect x="1.8" y="19.8" width="2.6" height="2.6" fill="#2fae3a"/><rect x="19.6" y="1.6" width="2.6" height="2.6" fill="#2fae3a"/>)svg"},
        {"offset", R"svg(<rect x="2.5" y="3.5" width="19" height="17" rx="1" fill="#dfe6ee" stroke="#222" stroke-width="1.3"/><rect x="7" y="8" width="10" height="8" fill="#fff" stroke="#d6262e" stroke-width="1.6"/><path d="M4 12 L7 12" stroke="#2f7de1" stroke-width="1.2"/>)svg"},
        {"tape", R"svg(<circle cx="10" cy="12" r="7.5" fill="#f2c230" stroke="#6b5310" stroke-width="1.1"/><circle cx="10" cy="12" r="2.5" fill="#6b5310"/><path d="M10 19.5 L22 19.5 L22 16.5 L14 16.5" fill="#f7e7a6" stroke="#6b5310" stroke-width="1"/><path d="M16 16.5 L16 18 M18 16.5 L18 18.5 M20 16.5 L20 18" stroke="#6b5310" stroke-width=".8"/>)svg"},
        {"orbit", R"svg(<circle cx="12" cy="12" r="5" fill="#cfd8e3" stroke="#334" stroke-width="1"/><path d="M3 13 C3 6 21 6 21 13" fill="none" stroke="#d6262e" stroke-width="1.9"/><path d="M18.4 11 L21.6 14.6 L23 9.8 Z" fill="#d6262e"/><path d="M21 11 C21 18 3 18 3 11" fill="none" stroke="#2f7de1" stroke-width="1.9"/><path d="M5.6 13 L2.4 9.4 L1 14.2 Z" fill="#2f7de1"/>)svg"},
        {"pan", R"svg(<path d="M7 21 C4.5 18 3.5 15 4 13 C4.3 11.8 5.8 11.6 6.6 12.6 L8 14.5 L8 5.2 C8 3.8 10.2 3.8 10.2 5.2 L10.2 11 L10.2 3.6 C10.2 2.2 12.5 2.2 12.5 3.6 L12.5 11 L12.5 4.4 C12.5 3 14.7 3 14.7 4.4 L14.7 11.2 L14.7 6.2 C14.7 4.8 16.9 4.8 16.9 6.2 L16.9 15 C16.9 18 15.6 20 14.8 21 Z" fill="#f6d2b0" stroke="#7a4d28" stroke-width="1" stroke-linejoin="round"/>)svg"},
        {"zoom", R"svg(<circle cx="9.5" cy="9.5" r="6.5" fill="#e8f1fb" stroke="#334" stroke-width="1.8"/><path d="M14.5 14.5 L21.5 21.5" stroke="#334" stroke-width="3" stroke-linecap="round"/><path d="M6.5 9.5 L12.5 9.5 M9.5 6.5 L9.5 12.5" stroke="#d6262e" stroke-width="1.6"/>)svg"},
        {"zoomwindow", R"svg(<rect x="2" y="2" width="13" height="13" fill="none" stroke="#d6262e" stroke-width="1.2" stroke-dasharray="2 1.5"/><circle cx="12" cy="12" r="5" fill="#e8f1fb" stroke="#334" stroke-width="1.6"/><path d="M15.8 15.8 L21.5 21.5" stroke="#334" stroke-width="2.8" stroke-linecap="round"/>)svg"},
        {"zoomextents", R"svg(<circle cx="11" cy="11" r="5" fill="#e8f1fb" stroke="#334" stroke-width="1.6"/><path d="M14.7 14.7 L19 19" stroke="#334" stroke-width="2.6" stroke-linecap="round"/><g fill="#d6262e"><path d="M1.5 1.5 L7 2.5 L2.5 7 Z"/><path d="M22.5 1.5 L21.5 7 L17 2.5 Z"/><path d="M1.5 22.5 L2.5 17 L7 21.5 Z"/><path d="M22.5 22.5 L17 21.5 L21.5 17 Z"/></g>)svg"},
        {"new", R"svg(<path d="M5 2.5 L14.5 2.5 L19.5 7.5 L19.5 21.5 L5 21.5 Z" fill="#fff" stroke="#445" stroke-width="1.1"/><path d="M14.5 2.5 L14.5 7.5 L19.5 7.5" fill="#dde" stroke="#445" stroke-width="1"/><circle cx="17.5" cy="17.5" r="4" fill="#2fae3a"/><path d="M17.5 15.3 L17.5 19.7 M15.3 17.5 L19.7 17.5" stroke="#fff" stroke-width="1.5"/>)svg"},
        {"open", R"svg(<path d="M2 6 L9 6 L11 8 L20 8 L20 19 L2 19 Z" fill="#e7b84b" stroke="#7a5a12" stroke-width="1"/><path d="M2 19 L5.5 11 L23 11 L20 19 Z" fill="#f6d27a" stroke="#7a5a12" stroke-width="1"/>)svg"},
        {"save", R"svg(<path d="M3 3 L18 3 L21 6 L21 21 L3 21 Z" fill="#3b6fb6" stroke="#1d3b66" stroke-width="1"/><rect x="6.5" y="3" width="10" height="6.5" fill="#eef2f7"/><rect x="13" y="4" width="2.2" height="4.4" fill="#3b6fb6"/><rect x="6" y="13" width="12" height="8" fill="#fff"/><path d="M8 15.5 L16 15.5 M8 18 L16 18" stroke="#aab" stroke-width=".9"/>)svg"},
        {"cut", R"svg(<circle cx="6.5" cy="17.5" r="3.2" fill="none" stroke="#334" stroke-width="1.8"/><circle cx="17.5" cy="17.5" r="3.2" fill="none" stroke="#334" stroke-width="1.8"/><path d="M8.6 15 L17 3 M15.4 15 L7 3" stroke="#556" stroke-width="1.8"/>)svg"},
        {"copy", R"svg(<rect x="3" y="3" width="12" height="14" fill="#fff" stroke="#445" stroke-width="1.1"/><rect x="9" y="7" width="12" height="14" fill="#fff" stroke="#445" stroke-width="1.1"/><path d="M11.5 11 L18.5 11 M11.5 14 L18.5 14 M11.5 17 L16 17" stroke="#99a" stroke-width=".9"/>)svg"},
        {"paste", R"svg(<rect x="4" y="4" width="14" height="18" rx="1" fill="#c79a5a" stroke="#6b4a1c" stroke-width="1"/><rect x="8" y="2.5" width="6" height="3.5" rx="1" fill="#ddd" stroke="#555" stroke-width=".8"/><rect x="10" y="9" width="11" height="13" fill="#fff" stroke="#445" stroke-width="1"/>)svg"},
        {"delete", R"svg(<circle cx="12" cy="12" r="9" fill="#fff" stroke="#999" stroke-width="1.2"/><path d="M7.8 7.8 L16.2 16.2 M16.2 7.8 L7.8 16.2" stroke="#d6262e" stroke-width="2.4" stroke-linecap="round"/>)svg"},
        {"undo", R"svg(<path d="M9 5 L3.5 10 L9 15 L9 12 C14 11.5 18 13 20 18.5 C20 11 16 8 9 8 Z" fill="#3b6fb6" stroke="#1d3b66" stroke-width=".8" stroke-linejoin="round"/>)svg"},
        {"redo", R"svg(<path d="M15 5 L20.5 10 L15 15 L15 12 C10 11.5 6 13 4 18.5 C4 11 8 8 15 8 Z" fill="#3b6fb6" stroke="#1d3b66" stroke-width=".8" stroke-linejoin="round"/>)svg"},
        {"info", R"svg(<circle cx="12" cy="12" r="9.5" fill="#fff" stroke="#334" stroke-width="1.4"/><circle cx="12" cy="7" r="1.5" fill="#334"/><path d="M12 10.5 L12 17.5" stroke="#334" stroke-width="2.4" stroke-linecap="round"/>)svg"},
        {"import", R"svg(<path d="M4 14 L4 21 L20 21 L20 14" fill="none" stroke="#445" stroke-width="1.6"/><path d="M12 2.5 L12 15" stroke="#2fae3a" stroke-width="2.4"/><path d="M7.5 11 L12 16.5 L16.5 11 Z" fill="#2fae3a"/>)svg"},
        {"export", R"svg(<path d="M4 14 L4 21 L20 21 L20 14" fill="none" stroke="#445" stroke-width="1.6"/><path d="M12 16 L12 4" stroke="#d6262e" stroke-width="2.4"/><path d="M7.5 7.5 L12 2 L16.5 7.5 Z" fill="#d6262e"/>)svg"},
        {"view_iso", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#f2f2f2" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#c7d2de" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#9fb0c3" stroke="#222" stroke-width="1"/>)svg"},
        {"view_top", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#e33b3b" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#e8e8e8" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/>)svg"},
        {"view_front", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#e8e8e8" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#e33b3b" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/>)svg"},
        {"view_right", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#e8e8e8" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#e33b3b" stroke="#222" stroke-width="1"/>)svg"},
        {"view_back", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#e8e8e8" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#d0d0d0" stroke="#222" stroke-width="1" stroke-dasharray="1.5 1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 3 L12 12.5 L3 17 Z" fill="#e33b3b" fill-opacity=".85" stroke="#222" stroke-width=".8"/>)svg"},
        {"view_left", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#e8e8e8" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#d0d0d0" stroke="#222" stroke-width="1"/><path d="M12 3 L21 7.5 L21 17 L12 12.5 Z" fill="#e33b3b" fill-opacity=".85" stroke="#222" stroke-width=".8"/>)svg"},
        {"style_xray", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#b8d4f2" fill-opacity=".6" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#8fb6e0" fill-opacity=".55" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#6f9fd3" fill-opacity=".55" stroke="#222" stroke-width="1"/><path d="M3 17 L12 12.5 L21 17 M12 12.5 L12 3" stroke="#222" stroke-width=".7" stroke-dasharray="1.2 1"/>)svg"},
        {"style_backedges", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#f2f2f2" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#dcdcdc" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#c4c4c4" stroke="#222" stroke-width="1"/><path d="M3 17 L12 12.5 L21 17 M12 12.5 L12 3" stroke="#555" stroke-width=".9" stroke-dasharray="1.4 1.2" opacity=".8"/>)svg"},
        {"style_wireframe", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z M3 7.5 L3 17 L12 21.5 L21 17 L21 7.5 M12 12 L12 21.5" fill="none" stroke="#222" stroke-width="1.1"/>)svg"},
        {"style_hiddenline", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#fff" stroke="#222" stroke-width="1.1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#fff" stroke="#222" stroke-width="1.1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#fff" stroke="#222" stroke-width="1.1"/>)svg"},
        {"style_shaded", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#9fd08a" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#76b35f" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#5a9446" stroke="#222" stroke-width="1"/>)svg"},
        {"style_textured", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#d9a066" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#b9733c" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#9b5a2c" stroke="#222" stroke-width="1"/><path d="M3 12.2 L12 16.7 L21 12.2 M7.5 9.7 L7.5 19.2 M16.5 9.7 L16.5 19.2 M7.5 5.2 L16.5 9.7 M16.5 5.2 L7.5 9.7" stroke="#f3e2c9" stroke-width=".8"/>)svg"},
        {"style_mono", R"svg(<path d="M12 3 L21 7.5 L12 12 L3 7.5 Z" fill="#fff" stroke="#222" stroke-width="1"/><path d="M3 7.5 L12 12 L12 21.5 L3 17 Z" fill="#e2e2e2" stroke="#222" stroke-width="1"/><path d="M12 12 L21 7.5 L21 17 L12 21.5 Z" fill="#a4b2bb" stroke="#222" stroke-width="1"/>)svg"},
        {"perspective", R"svg(<path d="M3 20 L9 6 L15 6 L21 20 Z" fill="#dfe6ee" stroke="#222" stroke-width="1.1"/><path d="M12 6 L12 20 M6 13 L18 13" stroke="#888" stroke-width=".8"/>)svg"},
        {"parallel", R"svg(<rect x="4" y="5" width="16" height="15" fill="#dfe6ee" stroke="#222" stroke-width="1.1"/><path d="M12 5 L12 20 M4 12.5 L20 12.5" stroke="#888" stroke-width=".8"/>)svg"},
        {"help", R"svg(<circle cx="12" cy="12" r="9.5" fill="#3b6fb6" stroke="#1d3b66" stroke-width="1"/><path d="M9 9.2 C9 6.2 15 6.2 15 9.3 C15 11.6 12 11.6 12 14.2" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round"/><circle cx="12" cy="17.3" r="1.3" fill="#fff"/>)svg"},
        {"materials", R"svg(<circle cx="9" cy="9" r="5.5" fill="#d6262e"/><circle cx="15" cy="9" r="5.5" fill="#2fae3a" fill-opacity=".85"/><circle cx="12" cy="14.5" r="5.5" fill="#2f7de1" fill-opacity=".8"/>)svg"},
        {"geo", R"svg(<circle cx="12" cy="12" r="9" fill="#fff" stroke="#556" stroke-width="1.3"/><path d="M3 12 L21 12 M12 3 C7 8 7 16 12 21 C17 16 17 8 12 3" fill="none" stroke="#556" stroke-width="1"/>)svg"},
        {"credits", R"svg(<circle cx="12" cy="12" r="9" fill="#fff" stroke="#556" stroke-width="1.3"/><path d="M15 9 C13.5 7 9 7.5 9 12 C9 16.5 13.5 17 15 15" fill="none" stroke="#556" stroke-width="1.6"/>)svg"},
    };
    auto it = map.find(n);
    return it == map.end() ? nullptr : it.value();
}

} // namespace

QPixmap pixmap(const QString& name, int size) {
    const char* body = svgFor(name);
    qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    if (!body) return pm;
    QByteArray svg = QByteArray("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>") + body + "</svg>";
    QSvgRenderer r(svg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p, QRectF(0, 0, size, size));
    return pm;
}

QIcon get(const QString& name) {
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it.value();
    QIcon icon;
    for (int s : {16, 20, 24, 32, 48}) icon.addPixmap(pixmap(name, s));
    cache.insert(name, icon);
    return icon;
}

QString toolIconName(ToolId id) {
    switch (id) {
    case ToolId::Select: return "select";
    case ToolId::Eraser: return "eraser";
    case ToolId::Paint: return "paint";
    case ToolId::Line: return "line";
    case ToolId::Freehand: return "freehand";
    case ToolId::Rectangle: return "rectangle";
    case ToolId::Circle: return "circle";
    case ToolId::Polygon: return "polygon";
    case ToolId::Arc: return "arc";
    case ToolId::PushPull: return "pushpull";
    case ToolId::Move: return "move";
    case ToolId::Rotate: return "rotate";
    case ToolId::Scale: return "scale";
    case ToolId::Offset: return "offset";
    case ToolId::Tape: return "tape";
    case ToolId::Orbit: return "orbit";
    case ToolId::Pan: return "pan";
    case ToolId::Zoom: return "zoom";
    case ToolId::ZoomWindow: return "zoomwindow";
    }
    return "select";
}

QCursor toolCursor(ToolId id) {
    // Cursor = small crosshair hotspot plus the tool icon to its lower right,
    // except for pointer-like tools whose icon tip is the hotspot.
    const int S = 32;
    qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(S, S) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    auto icon = [&](const QString& n, int x, int y, int s) { p.drawPixmap(x, y, pixmap(n, s)); };
    int hx = 0, hy = 0;
    switch (id) {
    case ToolId::Select:
        icon("select", 0, 0, 24);
        hx = 6; hy = 2;
        break;
    case ToolId::Line:
        icon("line", 0, 0, 24);
        hx = 3; hy = 21;
        break;
    case ToolId::Eraser:
        icon("eraser", 4, 4, 20);
        hx = 6; hy = 22;
        break;
    case ToolId::Paint:
        icon("paint", 4, 2, 22);
        hx = 22; hy = 20;
        break;
    case ToolId::Orbit:
    case ToolId::Pan:
    case ToolId::Zoom:
    case ToolId::ZoomWindow:
        icon(toolIconName(id), 4, 4, 24);
        hx = 16; hy = 16;
        break;
    default: {
        // Crosshair at (8,8) and the tool icon beside it.
        p.setPen(QPen(Qt::black, 1));
        p.drawLine(QPointF(8, 2), QPointF(8, 14));
        p.drawLine(QPointF(2, 8), QPointF(14, 8));
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(QPointF(9, 3), QPointF(9, 6));
        icon(toolIconName(id), 12, 12, 18);
        hx = 8; hy = 8;
        break;
    }
    }
    p.end();
    return QCursor(pm, hx, hy);
}

} // namespace gs::Icons
