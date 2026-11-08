# Manual test checklist

Write down anything that looks wrong or is missing next to the item, or at the
bottom. Screenshots help.

## Look and feel
- [ ] Menus (File Edit View Camera Draw Tools Window Help)
- [ ] Top toolbars (Standard, Views, Styles, Camera), left two-column tool set
- [ ] Right "Default Tray" (Entity Info, Materials, Styles, Instructor)
- [ ] Status bar hint text, Measurements box on the right
- [ ] Sky/ground background, red/green/blue axes, edges/profiles, face colours
- [ ] Tool cursors and icons

## Drawing
- [ ] Line: chains, closes faces, splits faces, typed length, arrow-key axis locks
- [ ] Axis + target: draw along the red/green/blue axis and finish on an edge it crosses (exact crossing), on an edge it misses (edge wins), and into a wall face (stops on the surface); same with Shift/arrow lock
- [ ] Magenta inference: from a hexagon corner along each of its edges and their extensions; hover a slanted edge then draw parallel to it; Shift locks it; typed length along it
- [ ] Rectangle: on ground, on faces, arrow keys for vertical planes, `w,h` entry, square/golden hints
- [ ] Circle / Polygon: radius entry, `Ns` segments
- [ ] 2 Point Arc, Freehand
- [ ] Lines crossing each other split; overlapping rectangles split into regions

## Modifying
- [ ] Push/Pull: box from rectangle, taller/shorter box, pocket, push through (hole), Ctrl = new start face, double-click repeat
- [ ] Move / Ctrl copy / `5x` array; Rotate with 15° snaps; Offset
- [ ] Scale grips: corner (uniform), Shift+corner (free), edge midpoint (2 axes), face centre (1 axis), Ctrl about centre, typed `2` / `1.5,2` / `1,2,1` during and right after; a rotated component scales along its own sides; a flat rectangle shows only its 8 in-plane grips
- [ ] Eraser (drag, Shift hide, Ctrl soften); Delete key; Undo/Redo
- [ ] Select: click, window vs crossing, double/triple click, Ctrl/Shift modifiers
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
- [ ] Effects menu is greyed out with nothing selected; select a box, Noise: the dialog previews live, Cancel restores it, OK is one Undo
- [ ] Noise: only the blue axis on a flat plane makes terrain; only red on a wall; Distance and Bump size feel right in meters; New Pattern
- [ ] Noise/Blur on only the top of a box: the sides stay flat and connected
- [ ] Blur on a box: corners and edges round off; Keep volume on and off; on a noisy terrain it calms the bumps
- [ ] Decimate a noisy terrain by percentage and by polygons per m²: the outline stays, the "Polygons: a → b" line is right
- [ ] Quads / Hexagons / Triangles on a plane, a box, a terrain; Rotation; textures and materials stay on
- [ ] Voxelate a box (exact size with a fitting cube size), a sphere, a terrain, a textured face: live preview with the size slider, materials stay; Merge flat sides; very small size says the detail was reduced; result works with Push/Pull and Solid Tools
- [ ] Subdivide, Triangulate, Merge Flat Faces; Ctrl+F repeats; effects on a component with copies change only the selected one

## Sun and culling
- [ ] Window > Sun: Direction moves light and shadows; Height low = warm evening, below 0° = night; Brightness; the disc is visible looking towards it
- [ ] View > Render > Sun off gives the old flat modelling light
- [ ] Backface Culling: a box looks the same; from inside it you see out; a plane is see-through from behind and cannot be picked from behind

## Game Mode
- [ ] Ctrl+G, the green capsule follows the cursor; click on a surface: a separate window opens, standing on that spot, looking the same way as the camera
- [ ] Click in the game window: the cursor hides, the mouse turns without stopping at the window edge (on your Wayland session)
- [ ] WASD (also with a non-Latin keyboard layout), Shift, Space, arrow keys, R, Tab, F11, Esc then Esc
- [ ] Walk into walls and slide along them; up stairs and slopes; off a ledge; under a low ceiling; on Noise terrain
- [ ] Textures, sun, shadows and culling as set in the editor; closing the editor closes the game

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
- [ ] Window > Scene: colour, brightness, range, cone, softness; list follows the selection; Delete; Undo
- [ ] Move, Rotate, Ctrl+Move copy and Eraser on markers; window selection takes them; View > Lights and Cameras hides markers
- [ ] Camera > Add Camera From View, orbit away, double-click its marker: same view; Camera > Look Through; Set From View
- [ ] Copy/paste a light into another window; save and reopen; Game Mode shows the light without the marker

## Files
- [ ] Save/Open `.gsketch`; Export OBJ/glTF/STL and open them in Blender; Import OBJ
- [ ] Ctrl+V of geometry/components: it follows the cursor at once (grabbed by its bottom corner, snaps to points/axes), click places it, typed distance works, Esc or Ctrl+Z cancels the whole paste, one Undo removes a placed paste
- [ ] Clipboard: copy geometry (with a textured face) in one GNUSketch window and paste it into a second instance; paste a screenshot, a browser image and image files copied in Dolphin as planes

## Notes
