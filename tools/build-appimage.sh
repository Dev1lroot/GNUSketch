#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packages a built tree as an AppImage and checks that it starts with OpenGL
# on both X11 and Wayland (each on a private, off-screen display: nothing is
# shown on the real desktop). Used by local builds and by CI.
#
#   tools/build-appimage.sh [build-dir] [version]
#
# The AppImage lands in the build dir: <build-dir>/GNUSketch-<version>-x86_64.AppImage
# (version from CMakeLists.txt unless given). Needs xvfb-run, and kwin_wayland
# or weston for the Wayland check; linuxdeploy is downloaded once into the
# build dir.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$SRC/build}" && pwd)"
VERSION="${2:-$(sed -n 's/^project(GNUSketch VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")}"
export VERSION
OUT="$BUILD/GNUSketch-$VERSION-x86_64.AppImage"
LIBDIR="$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$BUILD/CMakeCache.txt")"
LIBDIR="${LIBDIR:-lib}"

QMAKE="${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}"
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"

# --- linuxdeploy (cached) ----------------------------------------------------
TOOLS="$BUILD/appimage-tools"
mkdir -p "$TOOLS"
for t in linuxdeploy linuxdeploy-plugin-qt; do
    f="$TOOLS/$t-x86_64.AppImage"
    if [ ! -x "$f" ]; then
        wget -q -O "$f.part" "https://github.com/linuxdeploy/$t/releases/download/continuous/$t-x86_64.AppImage"
        chmod +x "$f.part" && mv "$f.part" "$f"
    fi
done
export PATH="$TOOLS:$PATH" # linuxdeploy finds its Qt plugin on PATH
export APPIMAGE_EXTRACT_AND_RUN=1

# --- AppDir ------------------------------------------------------------------
APPDIR="$BUILD/AppDir"
rm -rf "$APPDIR" "$OUT"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr >/dev/null
cp "$SRC/packaging/gnusketch.png" "$APPDIR/gnusketch.png"

export QMAKE
export EXTRA_QT_MODULES=svg
export EXTRA_PLATFORM_PLUGINS=libqwayland.so
export LD_LIBRARY_PATH="$APPDIR/usr/$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$BUILD"
linuxdeploy-x86_64.AppImage --appdir "$APPDIR" \
    --desktop-file "$SRC/packaging/gnusketch.desktop" --icon-file "$SRC/packaging/gnusketch.svg" \
    --plugin qt

# The Qt plugin leaves out Wayland's client buffer integrations, without which
# Qt cannot create an OpenGL context on Wayland ("Failed to load client buffer
# integration: wayland-egl") and the 3D view stays dead. Bundle them and
# their libraries.
GFX="wayland-graphics-integration-client"
mkdir -p "$APPDIR/usr/plugins/$GFX"
for p in "$QT_PLUGINS/$GFX"/*.so; do
    case "$(basename "$p")" in *vulkan*) continue ;; esac
    cp "$p" "$APPDIR/usr/plugins/$GFX/"
done
[ -f "$APPDIR/usr/plugins/$GFX/libqt-plugin-wayland-egl.so" ] || { echo "no wayland-egl plugin in $QT_PLUGINS/$GFX" >&2; exit 1; }

linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --deploy-deps-only "$APPDIR/usr/plugins/$GFX" --output appimage
[ -f "$OUT" ] || mv "$BUILD"/GNUSketch-*x86_64.AppImage "$OUT"

# --- smoke tests -------------------------------------------------------------
# The packaged program creates an OpenGL 3.3 context and renders a frame with
# the real renderer, on X11 and on Wayland.
CHECK="$(mktemp -d "${TMPDIR:-/tmp}/gsai.XXXXXX")"
trap 'rm -rf "$CHECK"' EXIT
mkdir -m 700 "$CHECK/run" "$CHECK/cfg"

echo "== AppImage on X11"
env -u WAYLAND_DISPLAY -u QT_QPA_PLATFORMTHEME XDG_CONFIG_HOME="$CHECK/cfg" QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a -s "-screen 0 1600x1000x24" "$OUT" --gl-probe

echo "== AppImage on Wayland"
# (a short runtime dir: Wayland socket paths are limited to 108 bytes)
RT="$CHECK/run"
if command -v kwin_wayland >/dev/null; then
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RT" \
        kwin_wayland --virtual --no-lockscreen --socket gs --width 1600 --height 1000 >"$CHECK/compositor.log" 2>&1 &
elif command -v weston >/dev/null; then
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$RT" \
        weston --backend=headless --socket=gs --width=1600 --height=1000 >"$CHECK/compositor.log" 2>&1 &
else
    echo "no kwin_wayland or weston: cannot check Wayland" >&2
    exit 1
fi
COMP=$!
for _ in $(seq 100); do [ -S "$RT/gs" ] && break; sleep 0.1; done
set +e
env -u DISPLAY -u QT_QPA_PLATFORMTHEME XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY=gs XDG_CONFIG_HOME="$CHECK/cfg" \
    QT_QPA_PLATFORM=wayland LIBGL_ALWAYS_SOFTWARE=1 timeout 60 "$OUT" --gl-probe
rc=$?
set -e
kill $COMP 2>/dev/null; wait $COMP 2>/dev/null || true
if [ $rc -ne 0 ]; then
    echo "AppImage failed on Wayland (exit $rc); compositor log:" >&2
    tail -20 "$CHECK/compositor.log" >&2
    exit 1
fi
echo "== $OUT"
