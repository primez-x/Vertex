# Vertex user testing checklist

277 practical tasks, grouped by how you use the app. This replaces the earlier engineering-oriented checklist. The old 130-item list is preserved separately as engineering-requirements-checklist.md.

This is a user acceptance list for the intended app, not a claim that every listed action is implemented or working in the current preview. If you cannot find a control, or a feature is absent, mark **Blocked / missing** and describe it. Specialized sections can be skipped if you do not use them or lack the device or sample files.

**Build tested:** __________  **Date:** __________  **Windows display scaling:** __________

**Results:** Pass / Fail / Blocked or missing / Not tested. Check a box only after the task passes. Every result starts Not tested. Use the U-number in feedback; one issue may affect several tasks. Test with copies of projects.

## Suggested first pass

Start with the workspace, draw a room, close it with the mouse, drag-select it, edit a length, change its fill, place and resize furniture, then save/reopen and export. These basic workflows should work before spending time on specialized features.

## Sections

- Start a project and arrange the workspace (17 tasks)
- Buildings, floors, layers and visibility (11 tasks)
- Draw rooms and measured boundaries (26 tasks)
- Select, edit and transform drawings (21 tasks)
- Curves, alignment and constraints (15 tasks)
- Area colors, classifications and calculations (18 tasks)
- Symbols, furniture and component library (17 tasks)
- Text, labels and dimensions (11 tasks)
- Reference plans and tracing (10 tasks)
- Navigation and input (9 tasks)
- Walls, doors and windows in 3D mode (14 tasks)
- Other building objects and levels (21 tasks)
- Remodeling alternatives (6 tasks)
- 3D views, elevations and sections (8 tasks)
- Schedules and quantities (6 tasks)
- Sheets, printing and export (17 tasks)
- Saving, revisions and recovery (10 tasks)
- Import, exchange and connected devices — when available (9 tasks)
- Survey and georeferencing — when used (7 tasks)
- Optional assistance — when available (7 tasks)
- Complete a real job (5 tasks)
- Appraisal square-foot workflow (12 tasks)

## Start a project and arrange the workspace

- [ ] **U001 — Start a blank residential project**
  - Expected: A usable empty drawing opens with a clear active floor and layer.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U002 — Start a blank light-commercial project**
  - Expected: You can start drawing and enter the commercial project's details.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U003 — Enter the property's name, address and reference number**
  - Expected: The values remain after saving and reopening and are available on output sheets.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U004 — Switch between 2D and 3D**
  - Expected: The same project stays open; the mode and available tools are clear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U005 — Work entirely in 2D**
  - Expected: Architectural and 3D controls do not crowd the simple drawing workflow.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U006 — Resize and collapse the left panel**
  - Expected: The canvas gains space and the panel can be restored without losing your work.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U007 — Select an object and inspect its properties**
  - Expected: One click selects without opening a panel. Double-clicking the object or choosing Properties from its right-click menu opens the same compact contextual properties panel once. Double-clicking empty canvas does nothing.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U008 — Use the unified canvas pointer**
  - Expected: There are no Select, Draw First or Define First mode toggles. Click an object to select it, click empty canvas to draw, drag inside a selected object to move it, drag elsewhere to pan, and Ctrl-drag to select a region.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U009 — Use New, Open, Save and Save As**
  - Expected: Each command is identifiable; New/Open ask how to handle unsaved work.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U010 — Use the app at a smaller window size**
  - Expected: Drawing, primary commands and panels remain usable without overlapping or clipped controls.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U011 — Switch light, dark and high-contrast themes**
  - Expected: Text, selection, grid, dimensions and icons stay readable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U012 — Save and restore a workspace layout**
  - Expected: Panel arrangement and chosen view settings return as expected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U013 — Search for a command by name**
  - Expected: You can find and run the command without hunting through unrelated menus.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U014 — Customize a shortcut and use it**
  - Expected: The chosen key runs the intended action; conflicts are explained.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U015 — Choose the Apex shortcut preset**
  - Expected: Familiar commands work and the active preset is identifiable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U016 — Find and run a command**
  - Expected: Commands finds and runs supported actions without duplicating New, Open, Save, Undo, or Redo in another toolbar menu.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U017 — Open local help**
  - Expected: Help explains drawing completion, keyboard entry, symbols and saving without an internet connection.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Buildings, floors, layers and visibility

- [ ] **U018 — Add a second building**
  - Expected: It appears with its chosen name and can have its own floors.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U019 — Add a floor to a building**
  - Expected: It appears under the correct building.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U020 — Add a drawing layer to a floor**
  - Expected: It appears under the correct floor and can be selected as the drawing destination.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U021 — Rename a building, floor and layer**
  - Expected: The new names appear consistently in the tree and destination controls.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U022 — Choose where new geometry will be drawn**
  - Expected: The complete destination is understandable and new objects belong there.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U023 — Draw on two different layers**
  - Expected: Objects remain separately organized and selectable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U024 — Hide and show one layer with its eye control**
  - Expected: Only the intended layer's contents disappear and reappear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U025 — Hide and show one floor**
  - Expected: Its drawing disappears and returns without deleting it or changing calculated totals.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U026 — Identify hidden content**
  - Expected: Eye states make it clear what is hidden without redundant status labels.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U027 — Inspect the project tree**
  - Expected: Real drawing objects are accessible; internal storage records do not appear as unexplained objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U028 — Save and reopen a multi-building, multi-floor project**
  - Expected: Names, ownership and geometry remain correct.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Draw rooms and measured boundaries

- [ ] **U029 — Draw a rectangular room using node clicks**
  - Expected: Click the four corners in order, then click the first corner; the final click closes and retains the room.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U030 — Draw an irregular room with more than four corners**
  - Expected: Every clicked node is retained and the final closed shape matches the clicked outline.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U031 — Draw a triangle**
  - Expected: Three clicked corners and a final click on the first corner form a valid closed area.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U032 — Finish an unfinished polygon with Enter**
  - Expected: A valid closing edge is previewed or added and the completed area persists.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U033 — Finish an unfinished polygon with right-click**
  - Expected: You can complete the drawing without adding an unwanted node.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U034 — Approach the first corner while drawing**
  - Expected: A clear closing snap target appears as the pointer approaches the first corner.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U035 — Press Escape after completing a room**
  - Expected: The completed room remains in the project.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U036 — Cancel an unfinished drawing**
  - Expected: Cancellation is understandable and does not silently erase completed objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U037 — Undo the last corner while drawing**
  - Expected: Only the last drawing step is removed; earlier corners remain.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U038 — Redo an undone drawing step**
  - Expected: The same corner and segment return.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U039 — Draw first, then assign an area classification**
  - Expected: The area is retained with the selected classification.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U040 — Choose a classification before drawing**
  - Expected: The completed area receives that classification without a second unrelated workflow.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U041 — Draw a room using keyboard distance and direction**
  - Expected: Lengths and directions match the entered measurements.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U042 — Enter feet and fractional inches**
  - Expected: A value such as 12 ft 6 1/2 in is accepted and retained accurately.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U043 — Enter metric lengths**
  - Expected: Values such as 3.25 m produce the intended measured geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U044 — Switch displayed units**
  - Expected: The physical drawing size remains unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U045 — Use the on-screen measurement keypad while drawing**
  - Expected: It is clear which edge or value receives the entry and how to apply it.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U046 — Enter a rise and run**
  - Expected: The resulting angled segment matches both components.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U047 — Enter an absolute angle**
  - Expected: The line points in the intended direction.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U048 — Enter a turn relative to the previous line**
  - Expected: The turn is applied from the correct preceding direction.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U049 — Jump to another point without drawing a connecting edge**
  - Expected: The next segment begins where intended without a stray line.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U050 — Automatically close a nearly finished boundary**
  - Expected: Closure produces a valid shape and explains an impossible closure.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U051 — Complete a bay-window shape**
  - Expected: The generated segments match the entered bay dimensions.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U052 — Create an area from existing closed linework**
  - Expected: The resulting area matches the existing geometry without duplicate stray edges.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U053 — Reopen an existing area for editing**
  - Expected: You can change its boundary and complete it again.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U054 — Save an unfinished sketch and resume it**
  - Expected: Existing draft segments are available after reopening.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Select, edit and transform drawings

- [ ] **U055 — Click a line or room to select it**
  - Expected: The intended object visibly highlights and its properties appear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U056 — Drag a selection rectangle**
  - Expected: Ctrl-drag shows a visible marquee and objects in the selected region are selected on release.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U057 — Add objects to a selection with the modifier key**
  - Expected: Previously selected objects remain selected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U058 — Clear the selection**
  - Expected: Highlights and object-specific properties clear predictably.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U059 — Move a selected object**
  - Expected: Its geometry follows the intended displacement and measurements remain correct.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U060 — Move several selected objects together**
  - Expected: Their relative arrangement is preserved.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U061 — Change a wall or boundary edge length**
  - Expected: The final measured length matches your entry.
  - Result: Not tested
  - Steps: Select a closed boundary. Double-click it or right-click and choose **Edit boundary geometry**. Choose an edge, enter a visibly different length such as `14 ft`, and apply. Measure the edited edge and confirm it reads 14 ft. Repeat on a curved edge and confirm the displayed arc length, rather than its straight chord, matches the entry.
  - Notes / issues / screenshots: ____________________

- [ ] **U062 — Choose which endpoint stays fixed when changing length**
  - Expected: The chosen endpoint remains stationary.
  - Result: Not tested
  - Steps: Record or dimension both endpoints of one boundary edge. Resize it once with **Keep start fixed**, undo, then resize it with **Keep end fixed**. Confirm the chosen endpoint stays in the same grid position each time and undo/redo restores the exact prior/resulting shape.
  - Notes / issues / screenshots: ____________________

- [ ] **U063 — Choose whether connected geometry moves with an edit**
  - Expected: The preview and final result match your choice.
  - Result: Not tested
  - Steps: Resize one edge with **Move connected boundary chain** off and observe that only the opposite vertex moves. Undo, repeat with the option on, and confirm the complementary boundary chain translates together while the chosen endpoint stays fixed. Coincident geometry in a different object should remain unchanged unless it has an explicit supported relationship.
  - Notes / issues / screenshots: ____________________

- [ ] **U064 — Insert a vertex into an edge**
  - Expected: A new editable corner appears without corrupting the area.
  - Result: Not tested
  - Steps: Select an editable boundary created without retained construction receipts, choose **Insert vertex**, select an edge, enter `0.5`, and apply. Confirm one new handle appears halfway along the edge, the boundary remains closed, and undo removes exactly that vertex. A receipt-backed boundary must reject this operation with a clear message until topology-proof migration is supported.
  - Notes / issues / screenshots: ____________________

- [ ] **U065 — Move a vertex**
  - Expected: Adjacent edges update and the area remains valid or a clear error explains the problem.
  - Result: Not tested
  - Steps: Select a closed boundary, drag one visible corner handle, and release. Confirm both adjacent edges meet at the new point, attached length/angle dimensions still resolve, and one Undo restores the exact original geometry. Press Escape during a second drag and confirm no change is committed. Attempt to cross another edge and confirm Vertex rejects the invalid shape without changing the document.
  - Notes / issues / screenshots: ____________________

- [ ] **U066 — Copy and paste a room or object**
  - Expected: A separate editable copy appears with the expected geometry and properties.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U067 — Cut and paste an object**
  - Expected: The original is removed and the pasted object is retained correctly.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U068 — Duplicate an object**
  - Expected: The copy can be edited independently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U069 — Rotate a selected object around a chosen pivot**
  - Expected: Its angle and pivot behavior match the requested rotation.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U070 — Flip an object horizontally**
  - Expected: The result mirrors across the intended axis.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U071 — Flip an object vertically**
  - Expected: The result mirrors across the intended axis.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U072 — Delete a selected object**
  - Expected: Only the intended object disappears; dependent objects are handled clearly.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U073 — Undo a completed edit**
  - Expected: The prior geometry, properties and calculations return.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U074 — Redo a completed edit**
  - Expected: The same result is restored.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U075 — Try an invalid or self-crossing boundary edit**
  - Expected: A clear message explains rejection and the last valid drawing remains intact.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Curves, alignment and constraints

- [ ] **U076 — Draw a curved boundary using chord and height**
  - Expected: The curve passes through the expected endpoints and has the requested bulge.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U077 — Draw a curve using arc length**
  - Expected: The measured arc length matches your input.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U078 — Draw a curve using an angle**
  - Expected: The curve's sweep matches the entered angle.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U079 — Edit an existing curve**
  - Expected: Geometry, dimensions and area update together.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U080 — Draw and edit a curved architectural wall**
  - Expected: Its curvature and thickness remain consistent in plan and 3D.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U081 — Toggle grid display**
  - Expected: The grid visibly appears or disappears without changing geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U082 — Toggle magnet/grid snap**
  - Expected: New points snap when enabled and can be placed freely when disabled.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U083 — Snap a new point to an existing endpoint**
  - Expected: The points meet without a tiny unintended gap.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U084 — Align objects horizontally or vertically**
  - Expected: The resulting alignment matches the chosen reference.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U085 — Make two lines parallel**
  - Expected: They remain parallel after a supported dimension edit.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U086 — Make two lines perpendicular**
  - Expected: The right angle is maintained.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U087 — Join two endpoints with a coincident relationship**
  - Expected: They remain joined after a supported edit.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U088 — Lock a line's length**
  - Expected: Later connected edits preserve that length or explain a conflict.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U089 — Apply conflicting dimensions or relationships**
  - Expected: The app identifies the conflict without silently changing locked measurements.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U090 — Undo a constraint or dimension change**
  - Expected: The prior shape and relationships return.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Area colors, classifications and calculations

- [ ] **U091 — Change an area's fill color**
  - Expected: The selected area changes color on screen and in exported output.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U092 — Apply a hatch or pattern to an area**
  - Expected: The pattern and scale are readable and remain after reopening.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U093 — Change an area's outline style**
  - Expected: Color and line treatment apply to the intended boundary.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U094 — Classify areas as living, garage or another available class**
  - Expected: Each area appears in the correct totals.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U095 — Measure a 10 ft by 12 ft rectangle**
  - Expected: Area is 120 sq ft and perimeter is 44 ft, allowing only display rounding.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U096 — Measure a 3 m by 4 m rectangle**
  - Expected: Area is 12 square metres and perimeter is 14 metres.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U097 — Inspect the calculation behind an area total**
  - Expected: The relevant boundary, factors and deductions can be understood.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U098 — Apply an area factor**
  - Expected: The displayed adjusted value matches base area multiplied by the factor.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U099 — Subtract a smaller area or opening**
  - Expected: The net area reflects the intended deduction exactly once.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U100 — Use automatic subtraction for an overlapping area**
  - Expected: The deducted region is clear and totals match the chosen rule.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U101 — Combine multiple areas into a total**
  - Expected: Each intended area is counted once.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U102 — Check perimeter after editing an edge**
  - Expected: The perimeter changes by the expected amount.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U103 — Check building and living-area totals**
  - Expected: Grouping and classifications produce the expected separate totals.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U104 — Change display precision or rounding**
  - Expected: Only the displayed precision changes, not the physical geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U105 — Change a measurement profile**
  - Expected: The profile's effect on classifications and totals is visible and understandable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U106 — Undo a classification, factor or deduction**
  - Expected: The previous totals return.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U107 — Find a gap, overlap or duplicate segment**
  - Expected: The app highlights the relevant location and explains the issue.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U108 — Review a proposed geometry repair**
  - Expected: Nothing changes until you accept; accepted repairs can be undone.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Symbols, furniture and component library

- [ ] **U109 — Open the visible component library**
  - Expected: The left-panel Symbols tab opens without a modal. All categories reports 320 placeable components from the supplied architectural SVG library. Old procedural compatibility definitions do not clutter the placement list.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U110 — Search for a toilet, bed, sofa or table**
  - Expected: Relevant human names and recognizable detailed previews appear. Searching `Sofa Three Seat` returns a sofa with visible arms, back and three distinct cushions rather than a rectangle or generic line motif.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U111 — Browse library categories**
  - Expected: Source categories use readable names without numeric filename prefixes. Bathroom, bedroom, living, kitchen, office, electrical, HVAC/plumbing, doors/windows, structure, circulation, site and commercial equipment are easy to distinguish.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U112 — Place a symbol by choosing it and clicking the drawing**
  - Expected: It lands at the chosen location with its declared nominal footprint when supplied, or a clearly editable default size. The selected caption uses its human name.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U113 — Drag a symbol from the library onto the drawing**
  - Expected: A placement preview appears and dropping inserts the intended symbol.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U114 — Place several copies of one symbol**
  - Expected: Each instance can be selected independently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U115 — Change a symbol's width and depth**
  - Expected: The footprint matches the requested dimensions or unsupported resizing is clearly identified.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U116 — Resize a symbol proportionally**
  - Expected: Its proportions remain unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U117 — Rotate a symbol**
  - Expected: The angle and visual orientation match your entry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U118 — Move a symbol after placing it**
  - Expected: It moves without changing size or rotation.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U119 — Flip a symbol**
  - Expected: Its orientation mirrors correctly.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U120 — Duplicate a resized and rotated symbol**
  - Expected: The duplicate retains those settings and is independently editable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U121 — Delete and undo deletion of a symbol**
  - Expected: The correct instance disappears and is restored.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U122 — Place symbols on different floors or layers**
  - Expected: They follow the intended organization and visibility settings.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U123 — Save and reopen a furnished plan**
  - Expected: Symbol type, detailed artwork, placement, size, rotation and visibility survive. Reopened SVG symbols do not degrade to compatibility rectangles.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U124 — Print or export a furnished plan**
  - Expected: PDF, SVG and image exports retain the detailed component artwork and correct physical scale. Compare the three-seat sofa cushions and arms with the canvas preview.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U125 — Create or edit a reusable library item**
  - Expected: The revised item is available for future placement without unexpectedly changing unrelated instances.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Text, labels and dimensions

- [ ] **U126 — Add a room label**
  - Expected: The text appears where you place it.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U127 — Edit label text**
  - Expected: The new wording remains after saving and reopening.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U128 — Change text size, color and style**
  - Expected: The selected label updates and output matches.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U129 — Move and rotate a label**
  - Expected: It stays readable and retains its position and angle.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U130 — Reuse a saved text-library entry**
  - Expected: The expected text is inserted and can be customized for that instance.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U131 — Add a dimension to an edge**
  - Expected: It shows the correct measured value and units.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U132 — Move a dimension away from a wall**
  - Expected: Its association with the measured edge remains clear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U133 — Edit a dimension's appearance**
  - Expected: Text, line style and visibility update as expected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U134 — Hide and show dimensions**
  - Expected: Geometry remains unchanged and the visibility choice is reflected in output.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U135 — Edit geometry that has dimensions**
  - Expected: Associated dimensions update rather than showing stale values.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U136 — Delete and undo a label or dimension**
  - Expected: Only the intended annotation is removed and restored.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Reference plans and tracing

- [ ] **U137 — Import a raster plan or photo**
  - Expected: The image appears and can be positioned without blocking drawing.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U138 — Import a selected page from a PDF**
  - Expected: The intended page appears with legible detail.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U139 — Calibrate a reference against a known length**
  - Expected: A second known measurement agrees with the chosen scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U140 — Trace a room over the reference**
  - Expected: New measured geometry remains separate from the image.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U141 — Resize a reference**
  - Expected: Its size changes predictably and the resulting measurement scale is clear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U142 — Rotate and flip a reference**
  - Expected: The image aligns as intended without moving unrelated geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U143 — Adjust reference intensity**
  - Expected: The drawing remains readable over the image.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U144 — Hide and show a reference**
  - Expected: The tracing remains visible and unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U145 — Move a reference**
  - Expected: Only the selected background moves.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U146 — Save and reopen a project containing references**
  - Expected: Images remain available without needing the original external file path.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Navigation and input

- [ ] **U147 — Pan with the mouse**
  - Expected: Ordinary left-drag outside a selected object, middle-drag, and Space-left-drag move the view smoothly without creating or editing geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U148 — Zoom in and out at the pointer**
  - Expected: The intended drawing location stays in view.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U149 — Fit the drawing to the canvas**
  - Expected: All relevant geometry becomes visible at a useful scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U150 — Show and hide the overview map**
  - Expected: There is an obvious visible change.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U151 — Click or drag in the overview map**
  - Expected: The main view moves to the corresponding drawing region.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U152 — Use keyboard focus to reach commands and properties**
  - Expected: Focus is visible and typing affects the intended field.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U153 — Draw with an active pen, if available**
  - Expected: Pen placement behaves predictably without duplicate clicks.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U154 — Pan and zoom with touch, if available**
  - Expected: Navigation does not unintentionally draw or select objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U155 — Use the app at 150% or 200% Windows display scaling**
  - Expected: Controls and text remain legible and usable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Walls, doors and windows in 3D mode

- [ ] **U156 — Draw a straight architectural wall**
  - Expected: Clicking the start and end points on empty Architectural plan canvas creates a wall with editable length, height and thickness in plan and 3D.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U157 — Change wall thickness and height**
  - Expected: Plan and 3D update consistently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U158 — Join two walls at a corner**
  - Expected: The corner has no unintended visible gap or overlap.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U159 — Create a sloped wall**
  - Expected: The entered height/rise is visible in the correct direction.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U160 — Assign a wall material or layered assembly**
  - Expected: The chosen material or construction is reflected in properties and relevant output.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U161 — Insert a door into a wall**
  - Expected: It is hosted in the selected wall at the chosen position.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U162 — Change a door's width and height**
  - Expected: The opening and door update together.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U163 — Change door swing or handing**
  - Expected: Plan graphics and the hosted door agree.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U164 — Move a door along its wall**
  - Expected: It remains hosted and the opening follows it.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U165 — Insert a window into a wall**
  - Expected: The window and opening appear in the intended wall.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U166 — Change window size and sill height**
  - Expected: Plan, elevation and 3D agree.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U167 — Edit a door or window frame, panel or glazing**
  - Expected: The visible assembly and properties update consistently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U168 — Add an opening without a door or window**
  - Expected: The intended wall opening is visible.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U169 — Delete a hosted opening and undo**
  - Expected: Wall and hosted geometry are restored consistently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Other building objects and levels

- [ ] **U170 — Create a floor slab**
  - Expected: Its boundary, thickness and elevation are editable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U171 — Create a ceiling**
  - Expected: It appears at the intended level and has editable properties.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U172 — Create a foundation**
  - Expected: Its shape, dimensions and level match the entered values.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U173 — Create and resize a column**
  - Expected: Plan and 3D reflect the selected section and height.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U174 — Create and resize a beam**
  - Expected: Its span, section and position are correct.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U175 — Create a flat roof**
  - Expected: The roof footprint, elevation and material are editable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U176 — Create a shed roof**
  - Expected: The slope runs in the chosen direction.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U177 — Create a gable roof**
  - Expected: Both slopes and ridge match the intended form.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U178 — Create a hip roof**
  - Expected: The roof form and slopes match the intended footprint.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U179 — Edit roof pitch, overhang or a supported opening**
  - Expected: All affected views update and unsupported edits are explained.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U180 — Create stairs between levels**
  - Expected: The stair dimensions and level connection are understandable and editable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U181 — Edit stair width, rise or run**
  - Expected: The stair updates consistently in plan and 3D.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U182 — Add a landing and railing**
  - Expected: They appear in the intended locations and can be edited.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U183 — Create and edit building levels**
  - Expected: Objects associated with levels move as expected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U184 — Align geometry between floors**
  - Expected: The same reference position aligns across the chosen floors.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U185 — Add and edit reference grids**
  - Expected: Grid lines and labels appear in relevant views.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U186 — Create a basic site or terrain surface**
  - Expected: Entered elevations produce the intended surface.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U187 — Create a reusable building assembly**
  - Expected: It can be placed again with its intended type properties.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U188 — Edit one assembly instance**
  - Expected: Instance changes do not unexpectedly change every copy.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U189 — Duplicate, rotate and delete a building object**
  - Expected: The object and its relationships remain valid; undo restores it.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U190 — Create a room separately from an appraisal area**
  - Expected: Both can represent different boundaries without forcing identical geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Remodeling alternatives

- [ ] **U191 — Create an alternative to the existing design**
  - Expected: The original existing design remains available.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U192 — Mark objects existing, demolished or proposed**
  - Expected: The selected phase is visibly meaningful.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U193 — Switch between two remodeling alternatives**
  - Expected: Only the intended alternative's changes appear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U194 — Compare alternatives**
  - Expected: Differences are identifiable without losing the shared baseline.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U195 — Check phase-specific quantities and schedules**
  - Expected: Counts and totals correspond to the displayed phase or alternative.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U196 — Save and reopen alternatives**
  - Expected: Their names, membership and active choice are retained.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## 3D views, elevations and sections

- [ ] **U197 — Orbit, pan and zoom the 3D view**
  - Expected: Navigation is predictable and does not edit objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U198 — Select an object in 3D**
  - Steps: Select a supported wall, room, slab, column, beam, stair, railing or roof from the plan or navigator, then select a different object directly in 3D. Drag an axis handle, the vertical rotation ring and a scale handle in separate undoable edits. Open the object's right-click **Transform…** action and enter an exact value.
  - Expected: The same object is selected in every view and receives visible move, vertical-rotation and uniform-scale controls. Each released drag changes the shared semantic object once; plan, 3D and applicable elevation/section/schedule/calculation views refresh. Undo and redo restore each state. Exact numeric entry matches the handle behavior, while exported 3D imagery contains no editing controls.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U199 — Resize a room from the 3D view**
  - Steps: Create a rectangular room volume, open 3D, then double-click the room. Change its width, depth, height and base elevation; choose **Keep center**; confirm the live preview; then select **Apply**.
  - Expected: One **Room dimensions** dialog opens for the room under the pointer. The preview reports updated floor area and volume. The room retains its center, and plan, elevation, section, 3D and the room schedule all show the new dimensions. One Undo restores every prior value; Redo reapplies them. Save and reopen preserves the edit.
  - Additional check: Enter an invalid width, verify **Apply** is disabled, then select **Cancel**. No geometry, schedule value or undo-history entry changes.
  - Level check: Bind the room's floor to a nonzero building level and reopen the editor. The elevation field identifies that it is local to the bound level, while the preview reports the resolved project base including any placement offset.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U200 — Switch between plan, elevation and section**
  - Expected: Views show the same project from the intended direction.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U201 — Move a section or change its cut depth**
  - Expected: The visible cut changes as expected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U202 — Change section line treatment and material hatching**
  - Expected: The section remains legible and output matches.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U203 — Add annotations or detail to a view**
  - Expected: They belong to the intended view and persist after reopening.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U204 — Use side-by-side plan and 3D views**
  - Expected: Edits remain coordinated and both views remain usable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Schedules and quantities

- [ ] **U205 — Open a door and window schedule**
  - Expected: Marks, counts and dimensions match the placed objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U206 — Open a room schedule**
  - Expected: Room names and areas match the project.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U207 — Open material quantities**
  - Expected: Gross values, deductions and net values are understandable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U208 — Change an editable schedule value**
  - Expected: The corresponding model object changes.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U209 — Try editing a calculated schedule value**
  - Expected: It is read-only or directs you to the source input.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U210 — Change a model object and revisit its schedule**
  - Expected: The row updates without stale dimensions or quantities.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Sheets, printing and export

- [ ] **U211 — Create a second drawing sheet**
  - Expected: It belongs to the same project and can show different views.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U212 — Edit sheet title, number and project information**
  - Expected: Title-block values appear correctly.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U213 — Choose Letter, Legal, Tabloid, A4, A3 or an architectural page size**
  - Expected: Preview and export use the selected dimensions.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U214 — Place a plan view on a sheet**
  - Expected: It appears within the intended viewport.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U215 — Place an elevation, section or 3D view on a sheet**
  - Expected: The chosen view appears with the correct content.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U216 — Set a viewport's print scale**
  - Expected: Its scale changes independently of canvas zoom.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U217 — Place a schedule on a sheet**
  - Expected: The intended table fits and remains readable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U218 — Add a revision entry**
  - Expected: Its date and description appear on the intended sheet.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U219 — Add a callout to another sheet/view**
  - Expected: It points to the correct destination.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U220 — Open print preview**
  - Expected: The preview matches the selected sheet, orientation and scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U221 — Export PDF**
  - Expected: Text, lines, dimensions, fills and symbols are present and readable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U222 — Export an image**
  - Expected: The complete intended drawing appears at the chosen size.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U223 — Export SVG**
  - Expected: The drawing opens with the expected lines, text and styles.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U224 — Print a measured line on paper**
  - Expected: The physical length agrees with the chosen print scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U225 — Export multiple sheets**
  - Expected: Every intended page is included in the correct order.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U226 — Zoom the canvas and export again**
  - Expected: Print scale and page layout remain unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U227 — Remove or invalidate a required reference before output**
  - Expected: The app explains the issue instead of silently producing misleading final output.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Saving, revisions and recovery

- [ ] **U228 — Save, close and reopen your drawing**
  - Expected: Geometry, labels, symbols, colors and measurements remain intact.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U229 — Use Save As to create a separate copy**
  - Expected: The new file opens correctly and the original is unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U230 — Save to a long folder path**
  - Expected: The project saves and reopens without losing the previous file on failure.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U231 — Create a named revision**
  - Expected: You can identify and return to the intended saved state.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U232 — Compare two revisions**
  - Expected: Changes to geometry and relevant properties are visible.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U233 — Recover an autosaved copy after an interrupted session**
  - Expected: You can identify and open the recovered work without overwriting the original unknowingly.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U234 — Open the same project twice**
  - Expected: The app clearly offers safe read-only or independent-copy behavior.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U235 — Create a reusable project template**
  - Expected: A new project starts with the intended settings and resources.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U236 — Package a project and open it from another folder or PC**
  - Expected: Included references, symbols and settings remain available.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U237 — Try opening a damaged or unsupported file copy**
  - Expected: The app explains the problem without destroying or replacing your current work.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Import, exchange and connected devices — when available

- [ ] **U238 — Import an Apex v5 project copy**
  - Expected: Supported geometry, curves, labels, symbols, classifications and images are retained; losses are reported.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U239 — Import an Apex v7 project copy**
  - Expected: The drawing is editable and compares correctly with the original output.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U240 — Export a supported legacy Apex version**
  - Expected: The target Apex application opens it and any omitted content is identified.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U241 — Import a DXF drawing**
  - Expected: Lines, arcs, text and supported blocks arrive at the correct scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U242 — Export DXF and open it in another viewer**
  - Expected: Supported geometry and annotations appear at the correct scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U243 — Import an IFC model**
  - Expected: Supported objects are editable or clearly identified as reference-only.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U244 — Export IFC and open it in another viewer**
  - Expected: The supported model content and units are correct.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U245 — Receive a DISTO measurement, if the device is available**
  - Expected: The value enters the explicitly chosen field with correct units.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U246 — Send project information to a supported appraisal application**
  - Expected: Chosen fields and sketch output arrive correctly and failures are understandable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Survey and georeferencing — when used

- [ ] **U247 — Enter survey bearings and distances**
  - Expected: The traverse follows the entered course sequence.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U248 — Close a survey traverse**
  - Expected: Closure error is shown and a bad traverse is not silently adjusted.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U249 — Calculate acreage**
  - Expected: The value agrees with the closed survey area.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U250 — Export or print the survey**
  - Expected: Courses, labels and acreage are readable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U251 — Choose a coordinate reference system**
  - Expected: The selected system and units are clear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U252 — Add control points and align a drawing**
  - Expected: The resulting position and any residual errors can be inspected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U253 — Use georeferencing without an internet connection**
  - Expected: Installed coordinate resources work; missing resources produce a clear message.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Optional assistance — when available

- [ ] **U254 — Request suggested tracing from a reference**
  - Steps: Import and calibrate a high-contrast plan reference containing an L-shaped outline and an enclosed void. Open Assistance, choose Edge tracing, and request suggestions.
  - Expected: The unverified preview follows the L-shaped recess instead of filling its bounding rectangle. The enclosed void is visible as a separate interior contour. No editable drawing geometry exists before acceptance.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U255 — Accept a suggested trace**
  - Steps: Accept the topology-preserving suggestion from U254, inspect the resulting area calculation, then use Undo and Redo.
  - Expected: The outer contour and its interior void become editable boundaries in one history step. The outer boundary lists the void as a deduction, its net area excludes the void, one Undo removes both, and one Redo restores both. Other unaccepted suggestions remain previews.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U256 — Reject a suggested trace**
  - Expected: The existing drawing remains unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U257 — Extract dimensions from a reference**
  - Expected: Proposed measurements can be checked before acceptance.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U258 — Request assisted label placement**
  - Expected: Suggested positions can be reviewed, accepted or rejected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U259 — Try a supported plain-language drawing command**
  - Expected: The proposed action is understandable and changes can be undone.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U260 — Turn assistance off**
  - Expected: Manual drawing, editing, saving and output still work normally.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Complete a real job

- [ ] **U261 — Draw and furnish a small residential floor plan from start to finish**
  - Expected: You can measure, edit, label, calculate, save, reopen and print without getting stuck.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U262 — Prepare a residential remodeling option**
  - Expected: Existing/proposed work, views, schedules and sheets agree.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U263 — Prepare a small commercial layout with more than one floor**
  - Expected: Organization, symbols, quantities and output remain consistent.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U264 — Repeat your normal job with networking disabled**
  - Expected: Drawing, editing, saving, reopening, printing and exporting remain usable without account prompts.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U265 — Open, edit and save a larger real project**
  - Expected: The app remains responsive enough for practical work and clearly indicates any lengthy operation.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Appraisal square-foot workflow

- [ ] **U266 — Switch an area project from measurement to appraisal workflow**
  - Expected: Select a measured boundary, open its properties and choose Appraisal. The category list changes to appraisal categories and a square-foot summary appears without changing the drawn geometry.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U267 — Derive above-grade finished area automatically**
  - Expected: For a 10 ft × 10 ft boundary, open **Edit appraisal facts**, choose the residential policy, compatible property and measurement basis, Above grade, Finished, Direct interior access, Standard ceiling and Dwelling use. Vertex shows **Qualified**, derives Above-grade finished area, and reports 100.00 ft² without a separate calculate command or manual category choice.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U268 — Derive below-grade finished area from the floor declaration**
  - Expected: Change the selected floor to Below grade while keeping the area finished and otherwise eligible. Vertex derives Below-grade finished, moves its square feet to that separate bucket, and removes them from Above-grade finished area. The floor name and elevation do not override the declaration.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U269 — Derive garage, carport, porch, patio and deck from area use**
  - Expected: Set each boundary's Area use in **Edit appraisal facts**. Each area appears only in its derived named bucket; none silently becomes finished dwelling area.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U270 — Review appraisal totals by floor, building and property**
  - Expected: Create a 100 ft² area in Building 1 and a 200 ft² area in Building 2. Selecting the first shows 100 ft² for its floor and building while the property remains 300 ft²; selecting the second shows 200 ft² for its floor and building while the property remains 300 ft².
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U271 — Apply a typed deduction without double counting**
  - Expected: Declare an internal boundary as Open to below, Stair footprint or Other void and link it as a deduction to its enclosing area. It reduces the enclosing physical area once and has no standalone contribution. An unlinked exclusion keeps the appraisal result visibly Unqualified instead of returning a plausible total.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U272 — Change appraisal facts and undo them**
  - Expected: Change an area's declared use or its floor's grade. Vertex immediately derives the new category and recalculates totals. Undo and Redo restore the prior declarations, derived category, qualification state and totals together.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U273 — Save and reopen a declared appraisal project**
  - Expected: Appraisal workflow, policy, property kind, measurement basis, floor grade, area facts, deduction roles and derived square-foot buckets return unchanged. Vertex recalculates them from the saved facts rather than trusting a cached category.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U274 — Return to measurement workflow**
  - Expected: Switching back restores the project's prior measurement profile and its building/living calculations without reclassifying appraisal categories heuristically.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U275 — Withhold automatic totals when facts are incomplete or incompatible**
  - Expected: Leave a required fact undeclared, choose an incompatible policy/property/basis combination, or apply a factor other than 1. Vertex shows Unqualified with specific reasons. Physical and adjusted area remain inspectable, but the adjusted value is not presented as qualified appraisal square footage.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U276 — Calculate light-commercial area by use**
  - Expected: Select the light-commercial declared policy and assign separate boundaries as Occupiable, Common and Service. Vertex derives each bucket and reports their sum as the property measured total without mixing residential finished-area buckets into the result.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U277 — Keep site and survey outlines outside building appraisal totals**
  - Expected: Add a site or survey boundary to a project with a qualified building. The building remains Qualified and its floor, building and property totals remain unchanged without entering appraisal facts for the site outline. Selecting the site explains that it is excluded. Attempting to use the site boundary as a building deduction is blocked.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

## Issue report template

- Task ID(s):
- What I did:
- What I expected:
- What actually happened:
- Screenshot or project file, if useful:
- Severity: Cannot continue / Major difficulty / Minor issue
