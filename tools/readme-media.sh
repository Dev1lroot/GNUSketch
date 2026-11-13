#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders the README pictures on a private virtual X server (nothing shows on
# the desktop) and assembles the animated GIFs into docs/.
# Usage: tools/readme-media.sh [build-dir]   (needs python3 with Pillow)
set -e
BUILD_DIR="${1:-build}"
FRAMES="$(mktemp -d)"
CONFIG_DIR="$(mktemp -d)"
trap 'rm -rf "$FRAMES" "$CONFIG_DIR"' EXIT
env -u WAYLAND_DISPLAY -u QT_QPA_PLATFORMTHEME XDG_CONFIG_HOME="$CONFIG_DIR" \
    QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
    xvfb-run -a -s "-screen 0 1600x1000x24" "$BUILD_DIR/gnusketch" --screenshots "$FRAMES"
python3 -I - "$FRAMES" docs <<'PY'
import sys, glob, os
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
def gif(prefix, name, width, ms, hold):
    files = sorted(glob.glob(os.path.join(src, prefix + "_*.png")))
    if not files: return
    frames = []
    for f in files:
        im = Image.open(f).convert("RGB")
        im = im.resize((width, int(im.height * width / im.width)), Image.LANCZOS)
        frames.append(im.convert("P", palette=Image.ADAPTIVE, colors=128))
    frames += [frames[-1]] * hold
    frames[0].save(os.path.join(out, name), save_all=True, append_images=frames[1:], duration=ms, loop=0, optimize=True)
    print("docs/" + name, len(frames), "frames")
gif("animator", "animator.gif", 640, 50, 10)
gif("pushpull", "pushpull.gif", 640, 60, 12)
for name in ("window", "cinematic", "hollow"):
    p = os.path.join(src, name + ".png")
    if os.path.exists(p):
        im = Image.open(p).convert("RGB")
        if im.width > 1400: im = im.resize((1400, int(im.height * 1400 / im.width)), Image.LANCZOS)
        im.save(os.path.join(out, name + ".png"), optimize=True)
        print("docs/" + name + ".png")
PY
