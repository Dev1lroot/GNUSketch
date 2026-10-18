#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the scripted UI self-test on a private virtual X server (Xvfb) so that
# nothing is shown on the real desktop. Usage: tools/headless-selftest.sh [build-dir]
BUILD_DIR="${1:-build}"
exec env -u WAYLAND_DISPLAY -u QT_QPA_PLATFORMTHEME \
    QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a -s "-screen 0 1600x1000x24" "$BUILD_DIR/gnusketch" --selftest
