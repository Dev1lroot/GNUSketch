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

### New in 0.3.3
- Gears that fit each other without the engineering: choose a tooth size and roughly
  how big a gear should be, and the number of teeth follows. **Fit to** a gear in the
  model and the new one is placed right next to it, meshing. The module and tooth
  count are still there for engineers.
- Draw > Mechanical > Bevel Gear: straight bevel gears for shafts at right angles
  (45° miter pairs or reductions), placed alone or as a meshing pair
- Gear Notches: a *Standard tooth size* that turns a circle into an exact involute gear
  and straight edges into a rack, meshing with Draw > Mechanical gears
- [GEARS.md](https://github.com/Dev1lroot/GNUSketch/blob/main/GEARS.md): a guide to
  gears with animations of every kind of junction
- Eraser: a round brush you can see, sized in the Properties panel or with [ and ];
  it no longer erases components or edges behind the face you are cleaning
- Faster: snapping while drawing over large models is 50–80× quicker per mouse
  move, and parts out of view or edges turned off no longer cost frame time

### New in 0.3.2
- Draw > Engineering: ISO metric screws and bolts (hex, socket cap, countersunk,
  threaded rod), hex and square nuts and washers, M1 to M36, with real threads that
  fit each other
- Draw > Mechanical: involute spur gears and gear racks by module, tooth count and
  pressure angle
- Effects: Screw Thread (carve or raise a helical thread on a rod or in a hole),
  Gear Notches (teeth along any outline), Slicer (cut a large model into printable
  pieces with pegs, keys or ribs)
- One click on a smooth surface (a cylinder's side, a sphere, a thread) selects all of it

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
