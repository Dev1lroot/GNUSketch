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
python3 -I - "$FRAMES" docs/media <<'PY'
import sys, glob, os, re
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
seqs, stills = {}, []
for f in sorted(glob.glob(os.path.join(src, "*.png"))):
    m = re.match(r"(.+)_(\d{3})\.png$", os.path.basename(f))
    if m: seqs.setdefault(m.group(1), []).append(f)
    else: stills.append(f)
def small(im, width):
    return im if im.width <= width else im.resize((width, int(im.height * width / im.width)), Image.LANCZOS)
for name, files in seqs.items():
    frames = [small(Image.open(f).convert("RGB"), 560).convert("P", palette=Image.ADAPTIVE, colors=128) for f in files]
    frames += [frames[-1]] * 12  # hold the result
    frames[0].save(os.path.join(out, name + ".gif"), save_all=True, append_images=frames[1:], duration=70, loop=0, optimize=True)
    print("docs/media/%s.gif" % name, len(files), "frames")
for f in stills:
    name = os.path.basename(f)
    small(Image.open(f).convert("RGB"), 1400 if name in ("window.png", "hero.png") else 900).save(os.path.join(out, name), optimize=True)
    print("docs/media/" + name)
PY
