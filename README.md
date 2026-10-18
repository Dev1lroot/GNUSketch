# GNUSketch

[![CI](https://github.com/Dev1lroot/GNUSketch/actions/workflows/ci.yml/badge.svg)](https://github.com/Dev1lroot/GNUSketch/actions/workflows/ci.yml)
[![License: GPL v3+](https://img.shields.io/badge/License-GPLv3%2B-blue.svg)](LICENSE)

A free, native Linux 3D sketching tool with a push/pull workflow, written from
scratch in C++20 with Qt 6 Widgets and OpenGL 3.3 (core profile). Clean-room design:
no proprietary formats, code or artwork. All icons are original inline SVGs.

> **Status: early alpha (0.1).** The core modelling tools work. Expect rough edges
> and see "Not implemented yet" below.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/gnusketch            # or: ./build/gnusketch model.gsketch
```

Requirements: a C++20 compiler, CMake ≥ 3.21, Qt ≥ 6.4 (Core, Gui, Widgets, OpenGL,
OpenGLWidgets, Svg) and an OpenGL 3.3 capable driver. Works on Wayland and X11.

Debian/Ubuntu packages:

```sh
sudo apt install build-essential cmake qt6-base-dev libqt6opengl6-dev libqt6svg6-dev qt6-image-formats-plugins
```

Fedora: `sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtsvg-devel qt6-qtimageformats`
Arch: `sudo pacman -S cmake qt6-base qt6-svg qt6-imageformats`

Install system-wide with `sudo cmake --install build`.

Tests (none of them open a window on your desktop):

```sh
cd build && ctest                 # geometry kernel + file format tests
tools/headless-selftest.sh        # drives the real UI with synthetic input on Xvfb
./build/gnusketch --gl-probe      # renders one frame offscreen on your GPU, prints driver info
```

If the window does not open, run `./build/gnusketch --gl-probe` from a terminal and
include its output in the bug report.

## Geometry model

* **Edges are first-class; faces are planar n-gons** with an outer loop and any
  number of holes. Nothing is triangulated in the model. Triangulation happens only
  on the fly for display and for triangle-based exporters (glTF, STL).
* **Drawing a line** welds its endpoints to nearby vertices, **splits every edge it
  crosses** (and itself) at the intersections, then runs face inference.
* **Face inference** builds the planar arrangement of all edges in each affected plane:
  * a closed planar loop gets a face;
  * a line across a face splits it;
  * a loop inside a face turns into a hole plus an inner face;
  * a loop drawn around an existing face makes that face a hole.

  Materials and texture placement carry over to the split faces. Re-tracing an
  existing edge heals a deleted face.
* **Erasing** an edge between two coplanar faces merges them, and collinear edge
  pieces are re-joined.
* **Push/Pull** extrudes faces. When a face's edges all border perpendicular faces,
  it stretches the solid instead. Coplanar side faces are healed, pushing through a
  solid cuts a hole, and the sides of circles and arcs become soft and smooth-shaded.

## Tools (SketchUp-compatible shortcuts)

| Key | Tool | Key | Tool |
|---|---|---|---|
| Space | Select | E | Eraser |
| B | Paint Bucket | L | Line |
| R | Rectangle | C | Circle |
| G | Polygon | A | 2 Point Arc |
| P | Push/Pull | M | Move / Copy (Ctrl) / Array (`5x`, `/5`) |
| Q | Rotate (Ctrl = copy) | S | Scale (uniform or `x,y,z`) |
| F | Offset | T | Tape Measure (guides, model resize) |
| O / H / Z | Orbit / Pan / Zoom | Shift+Z | Zoom Extents |

Also Freehand and Zoom Window. Middle mouse orbits and Shift+middle pans with any
tool. The wheel zooms towards the cursor; double-clicking the middle button
re-centres the view.

**Inference:** endpoint (green), midpoint (cyan), on edge (red), on face (blue),
origin, red/green/blue axis lines, "from point" alignment, guides.
The **arrow keys** lock the red, green or blue axis. In Rectangle, Circle and Rotate
they choose the drawing plane instead. **Shift** locks the current inference.

**Measurements box:** start typing numbers at any time, then press Enter. It accepts
`1500`, `1.5m`, `150cm`, `5'6"`, `w,h` for rectangles, `24s` for segment counts,
`<dx,dy,dz>` relative and `[x,y,z]` absolute coordinates. Values typed right after a
Push/Pull or Move adjust that operation.

## Textures: drag and drop

Drag an image file (PNG, JPEG, WebP, BMP, TIFF, …) from your file manager onto a
face. It becomes a new material and the image is **fitted to that face**. Hold
**Ctrl** while dropping to **tile it at real-world size** instead (default tile
1 m wide; change it in Materials → Edit). If the face is part of the selection, every
selected face receives the texture. Dropping onto empty space or onto the Materials
panel just adds the material. Right-click → Texture → Fit to Face / Reset Position
re-maps selected faces. The Materials panel also ships generated textures (brick,
wood, tiles, grass, concrete, gravel, water, stone) and a colour palette.

## File formats

* **`.gsketch` (native)** is plain, documented JSON (see `src/io/NativeFormat.h`). It
  stores vertices, edges (soft/smooth/hidden/curve flags), n-gon faces with holes,
  per-side materials and planar UV mappings, guides, camera and style. Textures are
  embedded as their original PNG/JPEG bytes (base64).
* **OBJ + MTL** (export/import) keeps n-gon faces; only faces with holes are
  triangulated, because OBJ cannot express holes. Loose edges are written as `l`.
  Y-up on disk.
* **glTF 2.0** (export) produces a single self-contained `.gltf`, Y-up, with PBR
  materials and textures.
* **STL** (export) is binary, in millimetres.

**To get FBX:** export OBJ or glTF, then convert with Blender (File → Import, then
File → Export → FBX) or with assimp (`assimp export model.gltf model.fbx`).

## Code layout

```
src/core    Math, Mesh (B-rep kernel + face inference), Triangulate, Units, Document (undo)
src/io      NativeFormat, ObjIO, GltfExport, StlExport, ImageUtil
src/render  Camera, Renderer (GL 3.3: faces, geometry-shader wide/dashed lines, sky/ground)
src/app     Viewport, Inference, tools (Draw/Modify/Camera), Panels (tray), MainWindow, Icons
tests       kernel and I/O tests
```

## Not implemented yet

Groups/components, Follow Me, 3-point arc and pie, rotated rectangle, text,
dimensions, section planes, tags/layers, scenes, shadows, intersect faces, the
texture position editor, and folding of faces that move out of plane.

## Contributing

Bug reports and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md)
for the clean-room policy and code style. `TESTING.md` is a manual test checklist.

## License

GNUSketch is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).

SketchUp is a trademark of Trimble Inc. GNUSketch is an independent project and is
not affiliated with, endorsed by, or connected to Trimble.
