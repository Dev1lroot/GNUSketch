GNUSketch is a free and open-source 3D modeller built around direct drawing and
push/pull, for architectural and engineering models, with PBR materials,
animations, scripting, a first-person Game Mode and 3D printing import/export.
See the [README](https://github.com/Dev1lroot/GNUSketch#readme) and the
[documentation](https://github.com/Dev1lroot/GNUSketch/blob/main/DOCUMENTATION.md).

**This is an alpha.** Save often and report problems in the issues.

### Downloads
- **Linux:** `GNUSketch-*-x86_64.AppImage`: make it executable (`chmod +x`) and run it.
  Built on Ubuntu 26.04 (needs a distribution of that age or newer).
- **Windows (64-bit):** `GNUSketch-*-windows-x64-setup.exe` installs it (Start menu,
  desktop shortcut, opens .gsketch files); or unpack `GNUSketch-*-windows-x64.zip`
  anywhere and run `bin\gnusketch.exe`. Needs a graphics driver with OpenGL 3.3.

### 0.3.1
Packaging fixes for the first public builds of 0.3: the AppImage bundles everything
Qt's X11 and SVG support need, and its libraries find each other wherever it runs.

### New in 0.3.0
- Valve Source engine exports (plugins): Hammer maps (.vmf) with brushes, entities and
  lights, and studio models (.smd with a .qc for studiomdl)
- Help > Plugins is a plugin browser: vendor, license, version, credits; turn plugins on
  or off
- New app icon, a fuller About dialog, a Windows installer
- Documentation with pictures of every tool, and an animation tutorial

### Also in this series
- Dark interface with duotone icons, separate panels (Hierarchy, Properties,
  Materials, Instructor, Environment, Animator), font and icon size settings
- Animator: a timeline with layers, keyframes and motion tweens
- Cinematic lighting: ray traced soft shadows and light rays
- 3D printing: import and export 3MF, AMF, STL, PLY, OFF in real units
- Effects: De-triangulate, Hollow, Solidify; Bend, Round Corners, Solid Tools
- Length snapping and exact display precision; typed sizes for Scale
- Many kernel fixes (push/pull ghost faces, healing, lines lying on faces)
