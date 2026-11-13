# Manual test checklist

Write down anything that looks wrong or is missing next to the item, or at the
bottom. Screenshots help.

## Look and feel
- [ ] Menus (File Edit View Camera Draw Tools Window Help)
- [ ] Top toolbars (Standard, Views, Styles, Camera), left two-column tool set
- [ ] No Default Tray: Hierarchy (left), Properties, Materials, Instructor (right) open at the start; Window > Environment (Sun + Styles) and Window > Animator; every panel can be closed, moved, docked elsewhere and reopened from the Window menu; layout remembered after a restart
- [ ] Properties: Select tool + a face / edge / component shows their info (component: Game Properties… button); a light or camera shows its editor (colour, brightness, range, cone, position…); Mountain/Pit/Flatten/Texture Brush show the brush settings; the light tools show the next light's colour/brightness/range/cone
- [ ] Window > Settings: font size (System, or points) and toolbar icon size change everything live; Cancel restores; OK keeps after a restart
- [ ] Status bar hint text, Measurements box on the right
- [ ] Sky/ground background, red/green/blue axes, edges/profiles, face colours
- [ ] Tool cursors and icons
- [ ] Dark interface everywhere (menus, docks, tray, dialogs, browsers, tooltips, the Animator); hint texts readable
- [ ] Window > Settings: Dark/Light, Duotone/Full colour icons, primary and secondary colours, strength, presets; every icon changes live; Cancel/Esc puts it all back; OK keeps it after a restart
- [ ] Window > Toolbars lists every toolbar (Standard, Views, Styles, Camera, Scene, Solid Tools, Large Tool Set) and shows/hides it; the Scene toolbar has the light/camera tools, Add Camera From View, Look Through and a Properties button
- [ ] The Rotate icon is a protractor
- [ ] Axes: red/green/blue solid on the positive side, dotted on the negative side, also looking along them and from below the ground

## Drawing
- [ ] Window > Model Info (and Properties while a drawing tool is equipped): unit; display precision picked by example (0mm, 0.0mm, 0.00mm... / 0.000m / 0' 0"); every length shown follows it (Measurements box, Entity Info, Scale sizes, Tape)
- [ ] Length snapping Off / To display precision / Every step (1m, 10cm, 5mm, 1/16"): Line (along axes and in any direction), Rectangle (each side), Circle/Polygon radius, Arc, Push/Pull, Move, Offset, Tape come out in whole steps; endpoints, midpoints, edges and guides still snap exactly; typed values are not rounded; remembered for new models and saved with the model
- [ ] Line: chains, closes faces, splits faces, typed length, arrow-key axis locks
- [ ] Axis + target: draw along the red/green/blue axis and finish on an edge it crosses (exact crossing), on an edge it misses (edge wins), and into a wall face (stops on the surface); same with Shift/arrow lock
- [ ] Magenta inference: from a hexagon corner along each of its edges and their extensions; hover a slanted edge then draw parallel to it; Shift locks it; typed length along it
- [ ] Rectangle: on ground, on faces, arrow keys for vertical planes, `w,h` entry, square/golden hints
- [ ] Circle / Polygon: radius entry, `Ns` segments
- [ ] 2 Point Arc, Freehand
- [ ] Lines crossing each other split; overlapping rectangles split into regions

## Modifying
- [ ] Push/Pull: box from rectangle, taller/shorter box, pocket, push through (hole), Ctrl = new start face, double-click repeat, several selected faces (separate and adjacent) move together
- [ ] Move / Ctrl copy / `5x` array; Rotate with 15° snaps; Offset
- [ ] Hold X / Y / Z with any tool (Line, Move, Rectangle, Circle, Rotate, Bend, Tape...): it stays on the red / green / blue axis (or plane/rotation axis) while held and is free again when let go; typing `5x` after a copy still makes an array; Zoom is now Alt+Z
- [ ] Rotate snaps onto the red/green/blue axes, parallel to a hovered edge, into the plane of (or square to) a hovered face; the dashed line takes the colour of what it snapped to; hold Shift for 5° steps
- [ ] Push/Pull: hover a face parallel to the one pushed (also of a component): it goes exactly to that plane; push a pocket's floor near the back wall: it snaps on and leaves a clean hole; push a step flush with its neighbour: the tops and walls become single faces
- [ ] Faces that used to stay as "ghosts" after push/pull (two boxes side by side, a pocket pushed in two steps) are gone; nothing is left that cannot be erased
- [ ] Rotate sticks to the face under the cursor, loose or of a component (tilted/rotated ones too); hold Shift to keep that plane while picking a centre elsewhere; arrow keys still lock axes
- [ ] Inside an open component: endpoints, midpoints, edges and faces of the loose geometry and of other components around it still snap (and hide snaps behind them); draw a line from the component to an outside corner; Rotate sticks to an outside face
- [ ] Intersections: push one component (or loose box) halfway into another; hover where their faces cut through each other ("On Intersection", magenta dashed seam), its ends ("Intersection"); draw a line from end to end, and a line along an axis that stops on the seam
- [ ] Scale: the box shows "W: … L: … H: …" in the model's units, live while dragging; a single (rotated) component shows its own sizes
- [ ] Scale + Properties: W/L/H fields; type a size (120, 12cm, 1.2m, 4') + Enter rescales; Keep proportions on: the other two follow, off: only that one; "Keep in place" bottom centre / corner / centre; one undo per typed size; flat selections grey out the flat size
- [ ] Scale grips: corner (uniform), Shift+corner (free), edge midpoint (2 axes), face centre (1 axis), Ctrl about centre, typed `2` / `1.5,2` / `1,2,1` during and right after; a rotated component scales along its own sides; a flat rectangle shows only its 8 in-plane grips
- [ ] Eraser (drag, Shift hide, Ctrl soften); Delete key; Undo/Redo
- [ ] Erase (Eraser or select + Delete) the edge between two faces in one plane: they become one face, also when one of them was reversed (back colour) and when a wall stands on that edge (the wall goes, the floor joins)
- [ ] Move or Ctrl-copy a line onto a face (across it, or from edge to edge), paste lines onto a face, explode a component lying on a face: the face is split by them like drawn lines; a short line just lying inside a face stays a line
- [ ] Select: click, window vs crossing, double/triple click, Ctrl/Shift modifiers; triple-click a box with a round hole through it: the hole's walls are selected too
- [ ] Follow Me: circle along a line/arc (pipe), ring profile (hollow pipe), T/H beam along an L path, moulding around a box top (Alt-click the top face), profile around a circle (lathe); path selected first vs clicked after the profile
- [ ] Primitives (Draw → Primitive…): every shape; fields change with the shape; face count and error text update live (torus tube ≥ ring radius disables Place); placed shape follows the cursor, click sets it, Esc cancels; one undo; smooth on/off; values remembered next time
- [ ] Round Corners: rectangle corners, one cube edge, four top edges, all cube edges; live preview and too-large radius message
- [ ] Components: select a box, right-click Make Component; Ctrl+Move copies; double-click a copy, push/pull and draw on it (rest fades, other copies change live); Esc / click outside closes; rotate and scale one copy (others unchanged); Make Unique then edit; Explode; undo/redo through all of it; save, reopen, copy/paste between windows

## Position Texture
- [ ] Drop an image on a face, Ctrl+U, click the face: orange outline with handles, no blue selection pattern over the texture
- [ ] Drag inside: the image follows the cursor exactly, live; the faint full image shows past the face; Esc mid-drag restores it; one Undo per drag
- [ ] Corners (uniform, Shift free, Ctrl about the centre, snapping to the face's corner); drag across to mirror
- [ ] Side squares stretch one way; diamonds skew; circle rotates and snaps every 15°, correct with non-square images
- [ ] Click a handle (don't drag), type `45` / `2` / `2,1` / `0.5,0.2`, Enter; or click again to place
- [ ] Several selected faces, also on different sides of a box; a back face; a terrain; save, reopen and export to glTF keep it all
- [ ] A face with no texture says so; Texture: Fit and Reset still work

## Bend
- [ ] Open samples/stairs.gsketch, Ctrl+A, Ctrl+B, click the bottom front corner of the stairs, then a point along their run; move the mouse: the ghost follows both ways, snaps every 15°; type 90, Enter: a quarter-circle staircase with flat treads and smooth curved sides
- [ ] Walk up it in Game Mode; Undo restores it in one step; `24s` while bending
- [ ] Arrow keys change the axis; a box bent around the red axis; a selected component with copies; lines without faces; too sharp a bend is refused with a message

## Effects
- [ ] Round Corners and Trace Image are in the Effects menu (not Tools, not the tool palette); greyed out without a fitting selection
- [ ] Effects menu is greyed out with nothing selected; select a box, Noise: the dialog previews live, Cancel restores it, OK is one Undo
- [ ] Noise: only the blue axis on a flat plane makes terrain; only red on a wall; Distance and Bump size feel right in meters; New Pattern
- [ ] Noise/Blur on only the top of a box: the sides stay flat and connected
- [ ] Blur on a box: corners and edges round off; Keep volume on and off; on a noisy terrain it calms the bumps
- [ ] Decimate a noisy terrain by percentage and by polygons per m²: the outline stays, the "Polygons: a → b" line is right
- [ ] Quads / Hexagons / Triangles on a plane, a box, a terrain; Rotation; textures and materials stay on
- [ ] Voxelate a box (exact size with a fitting cube size), a sphere, a terrain, a textured face: live preview with the size slider, materials stay; Merge flat sides; very small size says the detail was reduced; result works with Push/Pull and Solid Tools
- [ ] De-triangulate an imported STL/3MF (or a Triangulated model): flat sides become single faces, straight outlines lose their extra points; with "Quads on curved surfaces" a sphere/cylinder becomes quads; the tolerance joins slightly uneven sides
- [ ] Hollow a box, a sphere, a torus, a model with thin parts, an imported figurine: the wall thickness is right (Tape Measure / section), thin parts stay solid, the inside faces point into the cavity (back colour inside), slicer sees a hollow part; too thick a wall says so; an open surface is refused
- [ ] Solidify: two overlapping boxes become one; a box with an inner wall, a sealed hollow inside and a loose plane sticking out keeps only the outer skin; a face turned inside out is turned back; an imported model with internal junk prints as one solid
- [ ] Subdivide, Triangulate, Merge Flat Faces; Ctrl+F repeats; effects on a component with copies change only the selected one

## Sun and culling
- [ ] Window > Environment: Direction moves light and shadows; Height low = warm evening, below 0° = night; Brightness; the disc is visible looking towards it
- [ ] View > Render > Sun off gives the old flat modelling light
- [ ] Backface Culling: a box looks the same; from inside it you see out; a plane is see-through from behind and cannot be picked from behind

## Game Mode
- [ ] Ctrl+G, the green capsule follows the cursor; click on a surface: a separate window opens, standing on that spot, looking the same way as the camera
- [ ] Click in the game window: the cursor hides, the mouse turns without stopping at the window edge (on your Wayland session)
- [ ] WASD (also with a non-Latin keyboard layout), Shift, Space, arrow keys, R, Tab, F11, Esc then Esc
- [ ] Walk into walls and slide along them; up stairs and slopes; off a ledge; under a low ceiling; on Noise terrain
- [ ] Textures, sun, shadows and culling as set in the editor; closing the editor closes the game
- [ ] F5 (and View > Render > Cinematic Lighting / Window > Environment > Cinematic in the editor): ray traced soft shadows from the sun and from point/spot/directional lights (lamps behind walls stay dark), light rays (sun shafts, lamp beams; Window > Environment > Light rays sets the strength, 0 = off); frame rate on a big model
- [ ] Shadows are always on: crisp where things touch the ground, softer further from the caster; no flicker or stripes while walking; far away shadows still there (coarser)

## Materials and textures
- [ ] Drag an image onto a face (fit), Ctrl+drop (tile), drop onto Materials panel
- [ ] Paint Bucket with Ctrl/Shift/Alt; colour palette and generated textures
- [ ] Edit tab: colour, opacity, tile size
- [ ] Drop an image from the desktop/Dolphin onto empty space: plane in top view (ground) and front/side view (vertical, on the ground), correct orientation, 1 px = 1 mm
- [ ] Trace Image: alpha cutout of a transparent PNG logo, dark shapes of a black-on-white scan, light shapes; threshold/smoothing/specks live preview; remove background on/off; push/pull the result

## PBR materials
- [ ] Materials panel shows the Architectural globes from the start; click one: it becomes current and the tool does not change; the Paint button takes the Paint Bucket
- [ ] Create Material: opens the Edit tab in the sidebar; click a `+` square to load an image, ✕ removes it, sliders and typed values agree
- [ ] Window > Material Editor: pick a material, change Metallic and Roughness: the ball and the model follow live; one Undo per drag
- [ ] Add Maps with a real set (albedo, normal, roughness, ...): each lands on the right row; drop a file on one row; Clear; strength sliders
- [ ] A normal map on a flat wall: lit bumps move with the Sun; DirectX tick flips them; Height shifts with the view; Opacity map cuts holes
- [ ] Position Texture on a PBR material, also one with only a normal map; save, reopen, Game Mode; export glTF to Blender: maps connected
- [ ] Shader tab: Use custom shader gives the template; type an error: red line number, the model unchanged; fix it; `_Time` animates; properties give sliders and colours
- [ ] Texture Brush with a PBR material

## Browsers (separate windows)
- [ ] Window > Material Browser: resize, Size slider, search, categories; pictures appear without freezing; click paints; **real mouse drag onto a face**; drag onto a component says to open it; double-click opens the editor; Save to Library, Add Texture Folder
- [ ] Right-click a component > Save as Prefab; Window > Prefab Browser lists it; **real mouse drag into the model**: blue box follows, lands on the surface; double-click carries it on the mouse, Esc cancels; Delete; Open to Edit
- [ ] A prefab in another window or a new model comes with its materials; two placed copies edit together

## Animation
- [ ] Right-click a component > Properties: small window; choose func_door: a script appears; message says it needs two states
- [ ] Double-click the door, F5 (name suggested CLOSED), rotate it about its hinge edge, F5 (OPEN), Esc; Go To shows each state; Rename, Delete, Undo
- [ ] Ctrl+G: walking into the door opens it, along a real arc, textures stay on; it closes after 3 s; the HUD says "E press" when aiming at it
- [ ] A second component as func_button (IDLE, PRESSED); door Name `door1`, button Target `door1`: the door no longer opens by touch or E, only by the button
- [ ] A component with a frame and a separate panel: only the panel moves; the frame still collides
- [ ] func_actor with 3+ states, preset "loop through states": runs as keyframes forever; a state that scales the geometry blends
- [ ] Write a script error: Check gives the line; in the game the error shows red in the corner and the rest keeps working
- [ ] Save as Preset, it is offered next time; Save as Prefab keeps states, class and script; save and reopen the model

## Sources, targets, nesting
- [ ] Properties has General, Targets and Sources tabs. Button: Add Target twice, pick names, set delays of 0 and 2 s; in the game the first fires at once and the second 2 s later
- [ ] On the door, Sources lists the button; ✕ stops it firing; Add Source links another entity, naming the door if it had no name
- [ ] Select two components, Make Component; click selects the whole; double-click opens it, click and move a part; double-click again goes a level deeper; Esc comes out one level at a time; other copies follow
- [ ] A door with a handle inside (states recorded with the handle moving along), placed in a frame component, Save as Prefab, drop it twice: both open, with handles, and the frames stay; open it from the Prefab Browser and it still edits
- [ ] Dropping a prefab onto an open component puts it inside; dropping it into itself is refused; undo at each step; save, reopen, export glTF and STL: everything inside is there

## Hierarchy, physics, namespaces
- [ ] Hierarchy panel on the left lists geometry, components (nested), lights, cameras; search; click selects both ways; a nested row opens the way to it, the fading follows
- [ ] Ticks hide and show, undoable; a hidden parent hides what is inside; F2 renames; Delete; Group several rows, Ungroup; right-click menu; Show Everything
- [ ] A crate with physics, mass 20, up in the air: in Game Mode it falls and rests flat; on a slope with low friction it slides, with high friction it stays; walking into it pushes it; mass 5000 does not move
- [ ] Crates stacked; one knocked off a ledge tumbles; a door with physics falls while its script still works; a crate resting on a door falls when the door opens
- [ ] A group with a button and a door targeting `door`, copied twice: each button opens only its own door. Names on the groups, a button outside with target `front/door`
- [ ] Save, reopen, prefab: physics settings and names stay

## Lights and cameras
- [ ] Draw > Lights: point light above the clicked point; `3` before clicking changes the height; spot and directional aim with a second click; Sun at night shows them best
- [ ] Properties of a selected light/camera: colour, brightness, range, cone, softness; Delete; Undo
- [ ] Move, Rotate, Ctrl+Move copy and Eraser on markers; window selection takes them; View > Lights and Cameras hides markers
- [ ] Camera > Add Camera From View, orbit away, double-click its marker: same view; Camera > Look Through; Set From View
- [ ] Copy/paste a light into another window; save and reopen; Game Mode shows the light without the marker

## Animator
- [ ] Window > Animator: select a component, New Layer (+); the layer shows its name, eye, lock, colour square and a keyframe at the playhead
- [ ] Move the playhead (click/drag the ruler), move or rotate the component: a keyframe appears (Auto Key); scrub between: it slides and turns smoothly (motion tween, blue with an arrow)
- [ ] Right-click: Remove Motion Tween (grey, holds), Create Motion Tween, Insert/Clear Keyframe; F6 / Shift+F6; drag a keyframe to another frame; double-click a frame inserts one
- [ ] A light: brightness and colour change over time; a camera moves (Look Through while playing)
- [ ] Play/Pause, loop, first frame, previous/next keyframe; frame rate and length; elapsed time; eye hides, lock stops changes; rename (double-click the name); delete layer
- [ ] Undo/redo of every timeline change and of an auto keyframe; save, reopen, the timeline is there

## Files
- [ ] Save/Open `.gsketch`; Export OBJ/glTF/STL and open them in Blender; Import OBJ
- [ ] File > Import > formats and File > Export > formats (3D Model, 3D Printing sections), plugin formats included
- [ ] File > Open Recent: files opened and saved appear newest first, numbered; a deleted file disappears; Clear Recent Files
- [ ] Import (File > Import > 3D Printing, and File > Open) 3MF/AMF/STL/PLY/OFF from a slicer or Thingiverse: right size (unit question for STL/PLY/OFF from elsewhere, remembered), a component named after the file, flat sides as single faces, curved ones smooth, 3MF colours as materials, multi-part 3MF placed right
- [ ] Export 3MF/AMF/STL/PLY/OFF of a 20 mm box in mm and in inches; open them in a slicer (PrusaSlicer, Cura, OrcaSlicer): right size, closed solid, 3MF/AMF colours
- [ ] Ctrl+V of geometry/components: it follows the cursor at once (grabbed by its bottom corner, snaps to points/axes), click places it, typed distance works, Esc or Ctrl+Z cancels the whole paste, one Undo removes a placed paste
- [ ] Clipboard: copy geometry (with a textured face) in one GNUSketch window and paste it into a second instance; paste a screenshot, a browser image and image files copied in Dolphin as planes

## Notes
