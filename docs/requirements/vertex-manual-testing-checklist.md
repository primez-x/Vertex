# Vertex user testing checklist

450 practical tasks, grouped by how you use the app (U001–U449 plus U100a). This replaces the earlier engineering-oriented checklist. The old 130-item list is preserved separately as engineering-requirements-checklist.md.

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
- Area colors, classifications and calculations (19 tasks)
- Symbols, furniture and component library (17 tasks)
- Text, labels and dimensions (11 tasks)
- Reference plans and tracing (10 tasks)
- Navigation and input (9 tasks)
- Walls, doors and windows in 2D and 3D (14 tasks)
- Other building objects and levels (21 tasks)
- Remodeling alternatives (6 tasks)
- 3D views, elevations and sections (8 tasks)
- Schedules and quantities (6 tasks)
- Sheets, printing and export (17 tasks)
- Saving, revisions and recovery (10 tasks)
- Import, exchange and connected devices — when available (9 tasks)
- Survey and georeferencing — when used (8 tasks)
- Optional assistance — when available (7 tasks)
- Complete a real job (5 tasks)
- Appraisal square-foot workflow (14 tasks)
- Architectural joins and named views (3 tasks)
- Measure a physical wall layout (2 tasks)
- Wall drawing and connected edits (3 tasks)
- Moving walls (3 tasks)
- Wall corners (3 tasks)
- Door mechanisms and readable wall dimensions (3 tasks)
- Window layouts and opening controls (4 tasks)
- Plan label placement (1 tasks)
- Appraisal reports and plan sheets (3 tasks)
- Boundary editing and diagnostic repairs (3 tasks)
- Curved exterior walls and appraisal measurements (1 tasks)
- Read measurements while editing (1 tasks)
- Custom area details (1 tasks)
- Exact keyboard drawing (18 tasks)
- Project appraisal details (1 tasks)
- ANSI-oriented measurements (4 tasks)
- Exact corner corrections and appraisal sheets (2 tasks)
- Exterior corner corrections (4 tasks)
- Building-object appearance (4 tasks)
- Saved-view appearance (6 tasks)
- Editing within styled saved views (3 tasks)
- Appraisal measurements while drawing walls (5 tasks)
- Comparing saved revisions (3 tasks)
- Importing round CAD fixtures and preserving layers (3 tasks)
- Keyboard alignment while drawing (2 tasks)
- Reviewing measured-area sources (1 tasks)
- Measured-line drawing history (1 tasks)
- Precise measured-line input (7 tasks)
- Entering a measured chord directly (4 tasks)
- Defining nested measured areas (4 tasks)
- Sloped-room appraisal calculations (2 tasks)
- Appraisal detail readability and unfinished reports (2 tasks)
- Combine measured regions (3 tasks)
- Constraints on measured lines (4 tasks)

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

Choose **Area** in Draw in the left panel before testing measured-boundary
drawing below. **Wall** draws physical walls and is the default for a new
drawing; switching modes does not change existing geometry.

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

- [ ] **U033 — Cancel pending drawing with right-click**
  - Expected: A stationary right-click cancels the pending segment or unplaced object without adding a node. Committed walls and measured segments persist; a completed area remains. Right-drag pans without cancelling.
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
  - Steps: Press Ctrl+K, choose **Start measured boundary with point input**, then press D. Enter start X `0 ft`, Y `0 ft` and press Enter. Use D for each edge: `12 ft` at `0 deg`, `8 ft` at `90 deg`, and `12 ft` at `180 deg`. Press Enter to close and choose the area classification. Repeat using Ctrl+Shift+D for Define First and choose the classification before placing the start. After each edge, use D to enter its dimension position. Press Enter to add the closing edge, use D to place its dimension, then press Enter to finish.
  - Expected: A 12-by-8-foot room with an area of 96 square feet. No mouse click is needed to place the starting point, edges or dimensions. Reopening D retains the last accepted edge inputs; Escape leaves the drawing unchanged and returns focus to the canvas. Undo removes the finished room in one step, Redo restores it, and saving/reopening preserves it.
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
  - Steps: Choose Measured lines in Draw. Click a start and two endpoints, then finish with Enter. Select the stroke, choose Tools → Jump to measured or boundary vertex and choose a saved vertex. Click a new endpoint and finish. Repeat using Tools → Lift measured pen while drawing, then click a new starting point. Save and reopen; Undo and Redo the accepted edges.
  - Expected: Jump starts the next independent stroke at the exact chosen vertex. Lift measured pen allows a separate starting point. Neither adds a connecting edge, wall thickness or area total. Previously committed edges remain when Enter, stationary right-click or Escape finishes a stroke.
  - Also try: Select a saved measured stroke and drag an endpoint handle. Double-click the stroke, choose Geometry, and enter a new edge length with either its start or end fixed. Try moving all other vertices together. Switch to Vertex position and enter exact X/Y coordinates. Preview, Cancel, Apply, Undo, Redo, save and reopen.
  - Expected: Endpoint handles identify the actual stable vertices. The editor shows original and proposed geometry and edge lengths before Apply. The chosen anchor stays fixed; connected movement is explicit. Cancel, invalid values and stale edits leave the document unchanged. One Undo restores the edit and affected measured areas. Original entered measurements remain retained in project history.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U050 — Automatically close a nearly finished boundary**
  - Steps: In Wall mode, draw three sides of a rectangle and press A. Inspect the closing wall and its dimensions, then Undo and Redo. Repeat in Area mode. In Define First, place each side's dimension before pressing A, then move the pointer to position the closing dimension and press Enter to anchor it. Also try clicking to place a dimension. Try A with only one side or an outline whose closing edge crosses another side.
  - Expected: A adds the remaining side to the original starting point using normal wall or measurement geometry. Define First waits for the closing dimension instead of placing it automatically. Invalid closure is explained without adding geometry. In an Appraisal/Exterior project, closing the wall outline also creates its exterior measurement in the same undoable operation.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U051 — Complete a bay-window shape**
  - Steps: In Wall mode, begin an outline, draw an angled side outward and a straight bay front, then press B. Continue drawing the surrounding outline. Undo and Redo the return before closing. Repeat in Area mode, using Complete bay-window return from Commands. In Define First, place the pending dimensions before pressing B. Save and reopen the completed drawing.
  - Expected: B adds one matching angled return with the same length as the first angled side. Drawing remains active; the application does not close across the bay opening. The return has the normal wall thickness or measurement dimensions. Undo removes only the return and Redo restores it. The finished drawing and dimensions survive reopening.
  - Also try: Press B after only one side, after a curved side, with Ctrl held, and while entering a name in a text field. Try an invalid backwards bay.
  - Expected: Invalid geometry is explained without adding a side. Modified keys and typing do not trigger bay completion.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U052 — Create an area from existing closed linework**
  - Steps: Choose Measured lines in Draw. Draw three sides of a rectangle, entering one of those sides with D (distance and heading) instead of clicking, then press A for the closing side. Select the finished stroke, choose Tools → Detect closed areas and choose its classification. Open the third left-panel tab, Details, and inspect the new area's edge lengths, perimeter and area. Repeat detection, then Undo, Redo, save and reopen. Also try a loose stub, a crossing separator and a smaller closed loop inside the rectangle.
  - Expected: Detection creates measured areas from the closed faces in the selected stroke's floor/layer context while keeping all source strokes. One Undo removes the newly defined areas together. Repeating detection adds no identical area. Loose lines alone add no appraisal total. Inner outlines require explicit reference, definition or deduction choices; ANSI nested partitions are covered in U407. Details shows measurements and appraisal results according to the project's setup and recorded facts.
  - Also try: Draw a closed outline with one crossing separator and define the two areas. Select the separator and drag it to make one area larger. Watch the proposal before releasing, then inspect Details, Undo, Redo and save/reopen. Select a standalone closed stroke and use its rotation handle for a quarter turn.
  - Expected: The source stroke and unambiguous derived areas update together in one Undo step. Declared classifications stay assigned to their areas; their measured sizes and GLA contributions update. A rotated stroke keeps its handle orientation. Deleted sources or ambiguous changed boundaries withhold affected totals and identify the issue instead of continuing to show qualified old measurements.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U053 — Reopen an existing area for editing**
  - Steps: Draw and close a rectangular room. Select it and run Redefine boundary from Commands. Draw a larger replacement and finish it. Repeat with a triangle. Start another redraw, try starting a new drawing, choose Cancel, then finish the retained redraw. Undo, Redo, save and reopen.
  - Expected: The selected area changes in place. Its measurements and area total follow the new shape. Cancelling keeps the current redraw. One Undo restores the prior shape; Redo restores the replacement. Save/reopen retains the result and history.
  - Also try: Add a length dimension and an angle dimension, then redraw the area with a different number of corners. In Review redraw references, keep the angle and map its original edges/corner to the numbered replacement edges/corner. Remove the length dimension. Repeat with an automatically placed angle label and choose Remove for that angle. Cancel once, then finish and Apply the same choices. Undo, Redo, clone the result, save and reopen.
  - Expected: Nothing changes before Apply. Cancel keeps the editable redraw. The kept angle follows the chosen edges; the removed length returns on Undo. Automatic measurements follow the new shape. Incomplete choices or conflicting locked measurements disable Apply with an explanation. Clone and save/reopen retain the accepted result.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U054 — Save an unfinished sketch and resume it**
  - Steps: Start a boundary, enter a precise edge length, and add another edge without closing the area. Save the project, choose New, then reopen the saved project and continue drawing. Undo an edge, save, reopen, and Redo it.
  - Expected: Draft geometry, exact inputs and Undo/Redo position survive. A successful save removes the unsaved marker and allows closing or New without a discard warning. Moving the pointer alone leaves the saved state clean; adding or changing an edge marks it unsaved. Cancelled or failed saves retain the draft and its unsaved warning.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Select, edit and transform drawings

- [ ] **U055 — Click a line or room to select it**
  - Expected: The intended object visibly highlights and its properties appear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U056 — Drag a selection rectangle**
  - Steps: Place furniture and labels inside two rooms. Choose Areas in the selection filter beside the bottom-right canvas controls. Ctrl-drag a window around everything, then repeat with Symbols and Text labels. Return to All items. Try Ctrl-click, double-click and right-click under each filter.
  - Expected: Ctrl-drag shows a visible marquee and objects in the selected region are selected on release.
    Only the chosen item kinds become new selections. Filtering leaves every item visible and does not change totals. Clicking an excluded item does not start a drawing. Existing selections retain their transform controls.
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
  - Steps: Draw and close two measured rooms, then place a sofa and draw a wall. Ctrl-click both rooms, the sofa, and the wall. Drag inside the selected bounds and release. Confirm both rooms and their dimensions move with the sofa and wall. Undo, redo, save, and reopen. Repeat with two joined walls, a text label, and a reference image. For a pair of rooms or walls with a saved coincident-point constraint, select just one owner plus the sofa and try moving it away from the other owner.
  - Expected: The complete group moves together in one undo step. Room areas, entered measurements, dimension placements, wall lengths, door/window positions along their hosts, symbol sizes, image calibration, and relative placement remain correct after reopening. Moving only part of a constrained mixed group reports the conflict and leaves every selected object unchanged. Wall-only connected movement is tested separately in U289.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U061 — Change a wall or boundary edge length**
  - Expected: The proposed shape and analytical length appear before Apply. The final measured length matches your entry; Cancel leaves the original drawing unchanged.
  - Result: Not tested
  - Steps: Draw a 12 ft by 6 ft rectangle and select it. Double-click it or right-click and choose **Edit boundary geometry**. Choose its 12 ft edge and enter `14 ft`. Compare the original and proposed outlines, length, area, and moved points before applying. Cancel once and confirm the rectangle remains 12 ft by 6 ft. Repeat and apply; measure the edited edge and confirm it reads 14 ft. Repeat on a curved edge and confirm the displayed arc length, rather than its straight chord, matches the entry.
  - Notes / issues / screenshots: ____________________

- [ ] **U062 — Choose which endpoint stays fixed when changing length**
  - Expected: The preview identifies the fixed endpoint and shows it stationary; Apply produces the same shape shown in the preview.
  - Result: Not tested
  - Steps: Record or dimension both endpoints of one boundary edge. Enter a new length and compare the previews for **Keep start fixed** and **Keep end fixed** before applying either. Apply with start fixed, undo, then repeat with end fixed. Confirm the chosen endpoint stays in the same grid position each time and undo/redo restores the exact prior/resulting shape.
  - Notes / issues / screenshots: ____________________

- [ ] **U063 — Choose whether connected geometry moves with an edit**
  - Expected: The preview and final result match your choice.
  - Result: Not tested
  - Steps: Enter a new edge length with **Move connected boundary chain** off and inspect the proposed outline and moved-point list. Turn the option on before Apply and compare the movement. With it off, only the opposite vertex moves; with it on, the complementary boundary chain translates together while the chosen endpoint stays fixed. Apply each choice separately with Undo between them. Coincident geometry in a different object should remain unchanged unless it has an explicit supported relationship.
  - Notes / issues / screenshots: ____________________

- [ ] **U064 — Insert a vertex into an edge**
  - Expected: A new editable corner appears without corrupting the area.
  - Result: Not tested
  - Steps: Draw and close a rectangle, select it, choose **Insert boundary vertex**, and identify the edge by its number on the preview. Enter `0.5`; the green mark should appear halfway along that edge. Cancel once and confirm the drawing is unchanged, then reopen and Apply. Confirm the area total is unchanged. Drag the new corner to reshape the area and confirm the dimensions and total update. Undo the move and insertion, redo both, then save and reopen to check that the edited shape and dimensions remain. For a straight rectangle with no saved relationships, the coordinate freedom preview should show `8 → 10 (+2)`; with one saved horizontal relationship it should show `7 → 9 (+2)`.
  - Notes / issues / screenshots: ____________________

  - Older Vertex projects: If the editor asks for a boundary identity upgrade, select that area and run **Upgrade boundary identities** from Commands or the right-click menu. Confirm the drawing and area total stay unchanged, then insert a vertex. Undo the insertion and upgrade separately, redo, save and reopen.

- [ ] **U065 — Move a vertex**
  - Expected: Adjacent edges update and the area remains valid or a clear error explains the problem.
  - Result: Not tested
  - Steps: Select a closed boundary, drag one visible corner handle, and release. Confirm both adjacent edges meet at the new point, attached length/angle dimensions still resolve, and one Undo restores the exact original geometry. Press Escape during a second drag and confirm no change is committed. Attempt to cross another edge and confirm Vertex rejects the invalid shape without changing the document.
  - Also try: In the Architectural workspace, choose a saved horizontal plan with a shifted origin and rotated view axes. Drag a corner joined to a wall containing a door. Check that the corner, wall, door, dimensions and area label match the live preview after release. Repeat with a crop: a door moved into the visible region should appear while dragging. Undo, redo, save and reopen. A boundary clipped by the crop should not show misleading corner handles; expand the crop to edit its full outline.
  - Notes / issues / screenshots: ____________________

- [ ] **U066 — Copy and paste a room or object**
  - Steps: Place two symbols and two text labels. Select one symbol and copy/paste it. Then Ctrl-select one symbol and one label and copy/paste them into another project.
  - Expected: A separate editable copy appears with the expected geometry and properties.
  - Immediately after pasting: The pasted objects should already be selected. Drag inside their selection to move them; for a single symbol, use its resize and rotate handles without clicking to select it again. The rotation handle should retain its angle after release. Try 45, 90 and 180 degrees, then hold Shift for a fine angle and check the live degree readout. Undo each change and confirm nearby unselected items stay unchanged.
  - Also check: Only the selected symbols and labels are copied. Their sizes, rotation, mirroring, artwork and text are preserved; the unselected items remain untouched. Copy a symbol from a custom layer into another project: it should appear on that project's active layer. Undo/redo and save/reopen preserve the copies.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U067 — Cut and paste an object**
  - Steps: Ctrl-select one symbol and one label among several placed items, cut them, and paste into another project. Repeat with a selected wall and its hosted door alongside the annotations.
  - Expected: The original is removed and the pasted object is retained correctly.
  - Also check: Unselected symbols and labels remain. One Undo restores the complete cut selection. A rejected selection or read-only project changes neither the project nor clipboard.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U068 — Duplicate an object**
  - Expected: The copy can be edited independently.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U069 — Rotate a selected object around a chosen pivot**
  - Steps: Rotate an area and a column to 90 degrees, release, then rotate again to 180 degrees and back to the original angle. Hold Shift for an angle between snapping points. Repeat with a circular column and in a saved horizontal architectural plan view.
  - Expected: The handle stays attached to the rotated selection box after release. Common angles snap, Shift permits fine adjustment, and degrees appear while dragging. Undo/redo and save/reopen retain the orientation.
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
  - Steps: Select one symbol and one label among other items and press Delete. Repeat with a wall and its hosted opening selected alongside those annotations.
  - Expected: Only the intended object disappears; dependent objects are handled clearly.
  - Also check: Unselected annotations remain; deleting the wall removes its hosted opening. One Undo restores the entire selection, and Redo removes it again.
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
  - Steps: Draw a curved wall and note its curved length. Select it, open **Dimensions and constraints**, and choose **Change curve length**. Check that the prefilled length matches the arc. Choose **Keep start fixed**, enter a new length, Preview and Apply. Repeat with **Keep end fixed**. Try each connected-movement option on a joined wall or area; cancel an edit once before applying.
  - Expected: Its curvature and thickness remain consistent in plan and 3D.
  - Also check: The chosen endpoint stays fixed and the final arc length matches your entry. No permanent length lock is added unless you explicitly add one. Existing locks or frozen connections that make the edit impossible produce a conflict without changing the project. Undo, redo, save and reopen; verify the resulting wall, hosted openings and entered length.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U081 — Toggle grid display**
  - Steps: Toggle Grid off and on. With Imperial selected, zoom in until the grid cue shows inches or fractions, then zoom out to feet. Repeat in Metric, moving between centimetres and metres. Pan across the origin and compare existing objects before and after changing units and zoom.
  - Expected: The grid and its scale cue appear or disappear together. Divisions use readable units at each zoom. Panning preserves their world positions; saved geometry and exact dimensions remain unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U082 — Toggle magnet/grid snap**
  - Steps: With Snap on, start a wall near a minor grid intersection at several zoom levels in Imperial and Metric. Place its next endpoint along a diagonal and inspect its length. Compare the preview with the committed point. Turn Snap off and place a point between increments. Hide Grid while leaving Snap on and repeat.
  - Expected: Initial world-grid placement follows the displayed grid interval. An active edge snaps its length to useful increments relative to its own start, even on a diagonal. The preview and placed point agree. Snap off permits free placement; hiding the grid preserves the snap preference. Existing geometry is not rounded when the interval changes.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U083 — Snap a new point to an existing endpoint**
  - Steps: In Wall mode, approach an existing wall endpoint from several directions, including slightly above and below an aligned wall. Check the endpoint cue and click. Start a second wall against the middle of an existing wall and check the on-wall cue. Toggle Snap off and repeat.
  - Expected: The points meet without a tiny unintended gap.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U084 — Align objects horizontally or vertically**
  - Expected: The resulting alignment matches the chosen reference.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U085 — Make two lines parallel**
  - Also test: Choose endpoints on curved walls or boundary edges. Their endpoint chords become parallel while their curved outlines remain curved.
  - Expected: They remain parallel after a supported dimension edit.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U086 — Make two lines perpendicular**
  - Also test: Choose endpoint pairs on a curved wall and a straight wall. Their chords form a right angle; the curve retains its sweep rather than becoming a straight line.
  - Expected: The right angle is maintained.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U087 — Join two endpoints with a coincident relationship**
  - Steps: Draw a closed area and a straight architectural wall. Open **Dimensions and constraints** from the selected area and join one of its corners to a wall endpoint. Cancel a preview once and confirm nothing changes, then reopen and Apply. Select the wall, change its length while anchoring the opposite endpoint, and preview with **Allow connected objects to move** enabled. Check that the joined area corner follows the wall endpoint. Repeat with connected movement disabled: the app should explain a conflict when preserving the area makes the requested length impossible. Undo, redo, save and reopen the joined edit.
  - Also test: Select the joined area and open **Edit boundary geometry**. Change the joined edge length with its opposite endpoint fixed. Turn **Move related objects** off and check that an incompatible edit cannot apply. Turn it on and inspect the wall's movement before Apply. Cancel once, then Apply; undo, redo, save and reopen.
  - Also test: Drag the joined corner handle. Check that the joined wall follows and length, angle and area dimensions update during the drag. Check the live area/perimeter readout in Imperial and Metric units. Press Escape once to cancel, then drag and release to commit; measurements must match the preview. Undo, redo, save and reopen; both endpoints must remain joined.
  - Curved check: Repeat with a rounded patio or room boundary joined to a curved wall. Move the joined corner, including in a rotated saved plan view. Both curves should remain curved, move together in the preview, and stay joined after undo/redo and save/reopen.
  - Expected: The joined endpoints stay together whether the edit starts from the wall or area. Preview and cancellation leave the project unchanged; Apply records both movements together.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U088 — Lock a line's length**
  - Steps: In **Dimensions and constraints**, choose **Endpoint distance** and enter a length. On a curved object, compare the straight distance between its endpoints with its longer curved edge measurement.
  - Curved check: The lock controls the straight endpoint distance. Changing that distance preserves the curve's sweep; its physical arc length changes accordingly. This control does not claim to lock physical arc length.
  - Physical curve check: Select a curved wall or rounded area, open **Dimensions and constraints**, choose **Add constraint**, then **Curve length**. Check that the prefilled measurement matches the curved edge, choose the endpoint to keep fixed, enter a new length, Preview and Apply. The curved measurement must match the entry and remain curved. Use **Edit constraint** to change that target again. Undo, redo, save and reopen; the target and geometry must remain. Try a conflicting endpoint-distance lock and confirm the app leaves the project unchanged when it cannot satisfy both.
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
  - Steps: Select a closed room, open its quick properties, then Area attributes > Area appearance. Choose a fill color and Solid, then Apply. Save/reopen and export a PDF. Undo and redo the color edit.
  - Expected: The selected area changes color on screen and in exported output. Its dimensions, classification and appraisal totals stay unchanged; reopening and history retain the appearance.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U092 — Apply a hatch or pattern to an area**
  - Steps: In Area appearance, choose Hatch and change Hatch scale. Apply, save/reopen and export. Uncheck Show area, Apply, then select the area in the layer navigator and restore visibility. Try Reset to defaults followed by Apply, then Undo.
  - Expected: The pattern and scale remain after reopening and appear in output. Hiding affects presentation only. Reset restores the classification's default appearance without changing geometry or calculations, and Undo restores the custom appearance.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U093 — Change an area's outline style**
  - Steps: In Area appearance, change Outline color and Line width. Apply, zoom in/out and export a PDF. Apply without changing any fields, then cancel a separate edit.
  - Expected: Color and paper line width apply only to the intended boundary and stay consistent while zooming and in output. Unchanged Apply and Cancel add no edit; Undo/Redo preserve the accepted style.
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

- [ ] **U100 — Subtract a drawn area from a chosen parent**
  - Steps: Draw a 10-by-10-foot finished area and a contained 5-by-5-foot garage. Select the garage, right-click, choose Subtract from area, choose the finished parent and apply. Reopen this action and remove the deduction. Undo and redo each change. In declared Appraisal, also start with both areas assigned Above-grade finished, then edit their facts to Dwelling for the parent and Garage for the child. Repeat the subtraction and save/reopen. Finally, give both areas equal Dwelling facts despite different original categories and try linking them.
  - Expected: The parent net changes from 100 to 75 square feet and back to 100. The garage remains independently selectable and contributes once to its garage category in Appraisal. Current declared facts determine compatible types even when original categories differ. Equal current area types cannot be linked; incomplete declarations require correction. Cancel changes nothing.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U100a — Choose a parent before drawing a subtracting area**
  - Steps: In declared Appraisal, draw and qualify a 10-by-10-foot parent. Open Commands, choose Define area and subtract from, choose Open to below and the parent. Draw a contained 5-by-5-foot void. Also try saving and reopening before completing it.
  - Expected: The drawing hint names the chosen parent. Finishing reduces its net to 75 square feet without adding the void to a living-area category. One undo removes the new area, its dimensions and the parent deduction together. Save/reopen retains the parent choice; canceling leaves the parent unchanged.
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
  - Steps: With qualified Appraisal areas, open **Tools > Area display**. Save with 0, 1 and 6 decimal places in turn. Compare the selected area, property total, **Tools > Schedules**, and an Appraisal area summary on a PDF sheet. Cancel an edit, try saving the unchanged value, then Undo/Redo, save and reopen.
  - Also try: Two separate rooms each measuring 10 ft by 10.251 ft. At zero decimal places each rounds to 103 ft², but their combined total must be 205 ft² (205.02 rounded once), not 206 ft².
  - Expected: Appraisal inspector, schedule and PDF summary totals use the chosen precision and workspace units. Dimensions and geometry remain unchanged. Cancel and unchanged Save create no edit; Undo/Redo and reopening retain the saved setting. Changing decimal places does not change a garage or basement into living area or qualify missing appraisal declarations.
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
  - Expected: The left-panel Library tab opens without a modal. Search for base cabinet, wall cabinet, and fridge; each has recognizable artwork and a useful physical size. Browse all categories and verify distinct useful families. Old procedural compatibility definitions do not clutter the placement list; aliases and resized duplicates do not count as new artwork.
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
  - Steps: Select a symbol. Drag its right/left side handle, then its top/bottom side handle. Double-click it and enter exact width and depth.
  - Expected: Each side handle changes only its corresponding dimension; the opposite edge stays fixed. Dimensions appear on the canvas, including on rotated symbols. Exact entries update the footprint and undo restores each edit.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U116 — Resize a symbol proportionally**
  - Expected: Its proportions remain unchanged.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U117 — Rotate a symbol**
  - Steps: Note the symbol's starting angle. Drag the rotation handle to 45, 90 and 180 degrees, releasing between drags. Hold Shift to choose an angle between snapping points. Return to the starting angle (zero for an initially unrotated symbol).
  - Expected: The selection box and rotation handle retain the object's angle after release. Rotation snaps every 45 degrees, Shift permits fine adjustment, and a live angle matches the saved orientation. Returning to the starting angle restores the original orientation; undo/redo and save/reopen preserve it.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U118 — Move a symbol after placing it**
  - Expected: It moves without changing size or rotation.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U119 — Flip a symbol**
  - Steps: Double-click an asymmetric symbol and try horizontal and vertical flip separately, then together.
  - Expected: The artwork mirrors about its local axes without changing its dimensions or rotation. Undo/redo, save/reopen and exported drawings preserve the result.
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
  - Steps: In the Library tab, click + Text. Click New, name the entry "Finished basement", choose a category, enter two lines of text, and choose a font, text height, color and emphasis. Save it. Search for its name and use Insert; click the canvas to place it. Insert a second copy. Double-click the first placed label and change its text. Reopen + Text, edit the saved entry and Save, then insert a third copy. Delete the library entry, save the project, close it, and reopen it.
  - Expected: All three placed labels retain their own text, style and positions; editing the library affects future insertions only. Searching and category filtering find the saved entry. A built-in entry offers Save copy and cannot be overwritten. While Insert is active, dragging pans without placing; Escape cancels without changing the drawing. Reopening the project retains labels even after their library entry is deleted.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U131 — Add a dimension to an edge**
  - Expected: It shows the correct measured value and units.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U132 — Move a dimension away from a wall**
  - Steps: Draw a wall and double-click it, or right-click it and choose Wall measurement. Use Place measurement, then click a clear position away from the wall. Drag while placement is armed to pan; verify that no position is committed until a stationary click. Use Automatic to restore placement. Repeat in a rotated horizontal plan, then save and reopen.
  - Expected: Its association with the measured edge remains clear.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U133 — Edit a dimension's appearance**
  - Steps: Select the wall's quick properties and set the Wall measurement's text height, color, Bold, Italic and rotation. Apply. Check the same measurement in PDF output, undo/redo and after save/reopen. Change the wall's length and confirm the displayed value updates while its chosen appearance remains.
  - Expected: Text, line style and visibility update as expected.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U134 — Hide and show dimensions**
  - Steps: In the selected wall's Wall measurement controls, clear Visible and Apply. Confirm the wall and hosted openings remain. Select the wall again, restore Visible and Apply. Export both states, then test undo/redo and save/reopen.
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
  - Steps: Place two fingers on the plan, spread/pinch them to zoom, then move both together to pan. Start dragging a selected object with one finger and add a second finger: the object edit should cancel and navigation should take over. Lift one finger and move the other, then lift both and tap again to draw or select normally.
  - Expected: Navigation does not unintentionally draw or select objects.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U155 — Use the app at 150% or 200% Windows display scaling**
  - Expected: Controls and text remain legible and usable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Walls, doors and windows in 2D and 3D

- [ ] **U156 — Draw a straight architectural wall**
  - Steps: Start a new project. Confirm Wall is the drawing mode without choosing a separate Draw tool. Enter thickness and height in the Library tab, then click the start and end. Click two more endpoints to extend the chain, then press Escape. Drag empty canvas to pan. Select a wall, drag inside its selection to move it, then click outside once to deselect before starting another wall. Choose Area and draw a closed appraisal boundary.
  - Expected: Both wall faces appear at the entered thickness during preview and after placement, with a live length and retained wall measurements. Escape keeps completed walls. Preview and committed endpoints agree. Area creates a measured boundary rather than another wall. 3D shows the same physical walls. Changing thickness later updates the footprint; wall length labels agree in print/export and after save/reopen.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U157 — Change wall thickness and height**
  - Steps: Draw a wall and place a door and a window on it. Double-click the wall and change its thickness and height. Check the plan and 3D, Undo, Redo, and save/reopen. Try reducing the height below the top of its window, then apply a valid height.
  - Expected: Valid edits update plan and 3D together and retain the hosted objects. An invalid height gives an explanation and changes nothing; the subsequent valid edit still works. Undo restores the previous wall and openings in one step.
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
  - Library check: Place a door from the component list on an existing wall. Confirm it becomes a hosted opening, rather than a floating furniture symbol. Try placement away from a wall; the app should explain the missing host without adding an unrelated annotation.
  - Expected: Choose Door in the Library tab, set its width and height, then move onto an existing wall. A placement preview shows the opening and swing before clicking. The door cuts that wall and remains hosted there after save/reopen.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U162 — Change a door's width and height**
  - Canvas check: Select a door in the plan and drag either jamb handle. The opposite jamb stays fixed, the wall cut and circular swing update together, and the width/height readout follows the edit. Undo and Redo restore it. Use quick properties to change height.
  - Expected: The opening and door update together.
  - Curved wall check: Repeat on a curved host with sufficient frame depth. The opposite jamb remains fixed; Arc W reports width along the wall, while the swing remains circular. An impossible leaf/frame fit rejects the edit without changing the project.
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
  - Library check: Repeat using a window from the component list. The library and Window action must use the same hosted placement and wall cut. Undo, redo, save and reopen the result.
  - Expected: Choose Window in the Library tab, set its width, height and sill, then click the placement preview on an existing wall. The window and opening appear in that wall in 2D, elevation and 3D.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U166 — Change window size and sill height**
  - Canvas check: Select a window in the plan and drag either jamb handle. Its width changes along the wall while height and sill remain. Dragging into another opening or beyond the host wall is rejected without changing the saved geometry.
  - Expected: Plan, elevation and 3D agree.
  - Curved wall check: Resize both jambs separately. Rails follow the wall arc and Arc W measures along it; the native frame, sash and glass remain concentric with the host.
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
  - Steps: Create a shed, gable or hip roof. In its properties, add an opening and enter its local X/Y position, width and depth. Save, reopen the roof properties, change the opening size, and apply. Repeat for the other two roof forms. Inspect the cut in 3D, then remove the opening and undo.
  - Expected: An opening can be added while creating the roof and edited in the same properties dialog afterward. Its size and location survive saving and reopening. Removing it closes the cut; undo restores it. Moving an opening outside the roof or overlapping another opening shows an error and leaves the roof unchanged.
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
  - Saved 2D room check: Open a project containing a 2D room, select it in the plan, move it, rotate it, and resize it with a side handle. Open **Room dimensions**. Enter its measured height and base elevation, review the 3D preview, and apply. Undo should restore the 2D room; Redo should restore its measured volume. Cancel before applying should retain the original room, and other walls and roofs should remain visible in 3D throughout.
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
  - Steps: Open **Named elevations and sections**, select a section, and add a dimension annotation. Choose a wall under **Measure object**, choose **Width** or **Height**, and enter the line offset. Save, then change that wall's length or height.
  - Expected: The dimension line, witnesses, and value follow the wall's new size. Changing the offset moves the dimension without changing its value. Undo/redo and save/reopen retain the binding; PDF uses the same value. Choose **Detached** to retain an independent endpoint measurement, then delete its former wall: the detached dimension remains.
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
  - Steps: Choose the output page in **Sheet settings**, set the plan viewport's scale in **Sheet layout manager…**, then open **Print selected sheet (draft)**.
  - Expected: The preview matches the selected sheet, orientation and scale.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U221 — Export PDF**
  - Steps: Use **Export selected sheet PDF** for the selected page at its declared viewport scale. Also use **Export sketch PDF** and compare its tight crop of the visible drawing; that sketch is for report insertion and does not establish physical sheet scale.
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
  - Steps: Set the viewport scale in **Sheet layout manager…**, use **Print selected sheet (draft)** and print at actual size without fitting to the printer page. Measure a known line on the paper.
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
  - Steps: Choose Import DXF with a drawing containing several CAD layers and text. Review its layers, assign them to existing project layers on two floors, and import. Check the geometry and text on each layer. Undo, Redo, save and reopen. Repeat and cancel the layer review.
  - Expected: Supported lines, arcs, text and blocks arrive at the correct scale and on the chosen layers. Floor assignment preserves source coordinates and elevations. Text follows its assigned layer. One Undo restores the pre-import document; save/reopen preserves the mapping and source. Cancel changes nothing. Unsupported content is reported and its original bytes retained.
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
  - Steps: Select a wall, door or window. Open Import DISTO reading from Tools, choose Keyboard, enter the device name, choose its output units and decimal separator, then choose the dimension to change. Send a reading from a keyboard-capable device, or type a reading to try the controls without a device. Apply it, then Undo and Redo. For a second reading on the same dimension, choose Replace existing measurement explicitly.
  - Expected: The chosen dimension changes in the correct units. Undo restores the dimension and its previous reading together; Redo restores both. A conflicting wall length, an opening too large for its wall, an incompatible unit or an invalid reading shows an error and leaves the drawing unchanged. A previous reading is retained unless replacement is chosen. Record device reception separately from manually typed input.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U246 — Send project information to a supported appraisal application**
  - Expected: Chosen fields and sketch output arrive correctly and failures are understandable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________


## Survey and georeferencing — when used

- [ ] **U282 — Correct an existing survey boundary's calls**
  - Steps: Select a survey boundary, open Survey traverse, change a bearing or distance, Calculate, then Update boundary. Undo, Redo, save and reopen it.
  - Expected: The same boundary updates at its current starting point; its layer and area settings remain. Reopening restores the revised calls and closure choice. Conflicting dimensions or constraints show an error without partially changing the outline.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

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
  - Also try: Place and rotate a building in Site Plan, trace a reference assigned to its floor and layer, and compare the suggestion with the accepted outline. Before accepting another suggestion, change the active layer; request fresh suggestions if the application reports that the drawing context changed.
  - Expected: The outer contour and its interior void become editable boundaries in one history step. The outer boundary lists the void as a deduction, its net area excludes the void, one Undo removes both, and one Redo restores both. The accepted outline remains aligned with its reference in Site Plan and belongs to the reference's original floor and layer. Other unaccepted suggestions remain previews; changing the drawing context cannot silently place one on a different layer.
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
  - Display check: Start with a manually classified dwelling area, then declare Garage in the facts editor. The selected category must read Garage and be read-only; its default outline/fill becomes the garage style. Open Area appearance and check its defaults agree with the canvas. Change the use back to Dwelling and check both update. An undeclared legacy area's manual classification remains editable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U270 — Review appraisal totals by floor, building and property**
  - Expected: Create a 100 ft² area in Building 1 and a 200 ft² area in Building 2. Selecting the first shows 100 ft² for its floor and building while the property remains 300 ft²; selecting the second shows 200 ft² for its floor and building while the property remains 300 ft².
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U271 — Apply a typed deduction without double counting**
  - Steps: Draw and qualify a 10 ft by 10 ft dwelling area using **Edit appraisal facts**. Draw an internal 1 m by 1 m boundary and declare it **Open to below**. Select the dwelling, open **Edit deductions...**, choose the internal boundary and apply. Do this without assigning manual appraisal categories. Undo and Redo the deduction.
  - Expected: Declare an internal boundary as Open to below, Stair footprint or Other void and link it as a deduction to its enclosing area. It reduces the enclosing physical area once and has no standalone contribution. An unlinked exclusion keeps the appraisal result visibly Unqualified instead of returning a plausible total.
  - Expected for these dimensions: Linking the void shows Qualified and 89.24 ft² above-grade finished area. Undo removes the link and shows Unqualified because the exclusion is unlinked; Redo restores 89.24 ft². A deduction extending outside its enclosing area is rejected without changing the drawing or saved links.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U272 — Change appraisal facts and undo them**
  - Steps: Double-click a declared area. Use **Edit appraisal facts** directly below Classification, change its Area use, and Save. Repeat using the area's right-click action, but Cancel. Expand and collapse appraisal **Details** to inspect the remaining category totals.
  - Expected: Change an area's declared use or its floor's grade. Vertex immediately derives the new category and recalculates totals. Undo and Redo restore the prior declarations, derived category, qualification state and totals together.
  - Appearance check: Give the area a custom color and label position before changing its use. Those custom settings must stay. Reset to defaults must use the latest declared category, and Undo must restore the custom appearance. Save/reopen and check the category and appearance again. Clear a required fact: the selected category must show Unqualified instead of an older manual category.
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

- [ ] **U278 — Print the automatic appraisal area summary**
  - Expected: Create and qualify a 10 ft × 10 ft above-grade finished dwelling boundary. Open **Sheet layout manager…**, add **Appraisal area summary**, and use **Export drawing set PDF**. The sheet shows Qualified under the declared Vertex policy, Above-grade finished (GLA) at 100.00 ft², and matching property/building/floor totals. Remove a required appraisal fact and export again; the sheet says **Unqualified - automatic totals withheld** and contains no appraisal area values.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U283 — Show calculated square feet directly on the plan**
  - Steps: Qualify a 10 ft by 10 ft dwelling boundary in Appraisal. Double-click the boundary and enter First floor in **Area attributes > Name**. Draw an internal 5 ft by 5 ft garage, name it Garage, declare Garage use and subtract it from the dwelling. Hide the garage's layer, restore it, change workspace units, edit the garage boundary, Undo/Redo, save/reopen and export a plan PDF.
  - Expected: The dwelling label shows its name with 75.00 ft² and the garage shows 25.00 ft². Hiding the garage leaves the dwelling at 75.00 ft². Metric display shows 6.97 m² and 2.32 m². Editing the garage updates its value and the dwelling's net value. The plan PDF contains both names and values on readable separate lines. Void/site outlines receive no standalone building-area value. Missing declarations withhold numbers while preserving meaningful names.
  - Also try: Change **Tools > Area display** to 0, 1 and 6 decimals. Labels follow the selected precision. Drag a garage corner: its area and the dwelling's net area should update together before release. Press Escape to restore the original values, then repeat and release; the committed values should match the preview. Inspect labels in small or densely furnished areas; record any omitted or overlapping label.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

## Architectural joins and named views

- [ ] **U279 — Join and unjoin connected walls**
  - Expected: In Architectural workspace, draw three connected walls, Ctrl-click all three and choose **Join selected walls**. The 3D view shows one fused result while the source walls remain editable. Undo and redo each change the join in one step; save/reopen preserves it. Unjoin from either a source wall or the fused join and confirm no source wall or hosted opening is deleted. Repeating the command with two separate connected wall pairs is rejected without changing the project.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U280 — Join and unjoin touching roofs**
  - Expected: Create touching roof panels, select them and choose **Join selected roofs**. The 3D view shows one fused roof while the original roof parameters and openings remain editable. Undo, redo and save/reopen preserve the result. Unjoin keeps every source roof. Attempting to join two disconnected roof groups is rejected without changing the project.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U281 — Create and edit named elevations and sections**
  - Expected: Open **Named elevations and sections...**, create at least two elevations and one vertical section with distinct names, origins, directions, depth limits and model crop extents. Confirm model geometry crossing the left/right/bottom/top crop is clipped while geometry outside it is absent; doors/windows inside or crossing the crop retain appropriate view detail. Plan-only area labels must not appear in elevation or section sheet viewports. Place the views on sheets, edit one frame/crop, and confirm its other sheet references stay linked. Undo/redo and save/reopen preserve every named view and the same cropped geometry on canvas, print and export.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

## Measure a physical wall layout

- [ ] **U284 — Calculate exterior area from walls**
  - Steps: Draw a closed rectangular wall layout, finish it with Escape, then select a wall. Right-click and choose **Measure exterior from walls…**. Check the blue exterior outline and area in the preview, cancel once, then reopen and create it. If there are interior partitions, check that the review excludes them from the exterior perimeter.
  - Expected: Cancellation changes nothing. Creation adds one measurement area and keeps the physical walls. The area includes half the stored wall thickness outside each baseline. Repeating the command with the same walls selects the existing measurement instead of creating a duplicate.
  - Appraisal check: Select the area, use the Appraisal workflow, and enter the observed property/floor/area facts with **Edit appraisal facts…**. Check its category and net square feet on the plan and in the totals. Add a contained garage deduction and check the living total decreases while the garage remains separately classified.

- [ ] **U285 — Refresh area after changing wall thickness**
  - Steps: Name the exterior measurement, change its appearance, and add an area dimension. Change a source wall's thickness. Select the area, right-click and choose **Refresh exterior measurement…**; review and apply. Undo, redo, save, and reopen.
  - Expected: Before refresh, the old outline is marked stale and qualified appraisal totals are withheld. Refresh changes the exterior outline and area while keeping its name, appearance, facts, deduction links and dimension bindings. Undo restores the previous outline; redo restores the refreshed area. An open perimeter, missing source wall or read-only project is refused without a partial change.

## Wall drawing and connected edits

- [ ] **U286 — Draw and close a wall outline with ordinary clicks**
  - Steps: Start a new project with **Draw > Wall**. Click four corners, then click near the first corner. Move the mouse without clicking. Select a wall. Start another wall chain and press Esc after two segments.
  - Expected: Corners snap to existing endpoints and horizontal/vertical alignment when Snap is enabled. Every committed wall shows its length. Returning to the starting corner finishes the chain without another draft following the pointer. The next wall click selects it. Esc finishes an open chain while keeping its committed walls.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U287 — Change a connected wall's length without separating its corner**
  - Steps: Draw two connected walls. Double-click the first wall, edit its length, keep its starting endpoint anchored and enable connected movement. Review and apply. Undo, redo, save and reopen.
  - Expected: The shared endpoint moves together on both walls. The preview shows the adjacent wall's changed geometry. Undo restores the entire edit in one step. Redo and reopening preserve the connection. Conflicting locked measurements block the edit with an explanation rather than separating the walls.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U288 — Place a library window into a physical wall**
  - Steps: Draw a long wall. In Library, search for a window and double-click it. Choose Style and adjust width, height and sill; hover over the wall, then click away from its corners. Try the Window quick-insertion button on another part of the wall and change its Style before placement. Repeat with Door. Try placing a window outside any wall and too close to a corner.
  - Expected: The catalog and quick buttons use the same style and dimension controls, with an opening preview before the click. Both create wall-hosted openings with a real wall cut and editable dimensions. Switching Style loads its dimensions. An invalid location explains how to place the opening; it creates neither a detached decorative window nor a partial wall edit. Escape cancels placement; subsequent empty clicks draw walls again.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

## Moving walls

- [ ] **U289 — Drag a wall without separating its connected corner**
  - Steps: Draw two connected walls and place a window in the first. Select the first wall, then drag inside its blue selection frame. Watch the adjoining wall and window before releasing. Undo, redo, save and reopen.
  - Expected: Both endpoints of the selected wall move together. The adjoining shared corner follows; its other endpoint stays in place unless another declared relationship requires movement. Its length updates in the preview. The window stays in its host at the same offset and width. One undo restores the whole edit; reopening preserves the result.
  - Also try: Move to one location, then release at a slightly different one. The released position determines the completed move. Press Escape during a drag; no preview geometry is saved.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U290 — Move several walls as one selection**
  - Steps: Ctrl-click two or more walls, then drag inside their selection frame. Try connected and separate walls. Undo and redo.
  - Expected: Every selected wall moves by the same distance. Their lengths remain unchanged, shared corners remain connected, and hosted openings move with their walls. The selection stays active after the move. Undo restores all moved geometry in one step.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U291 — Keep a locked wall from moving**
  - Steps: Lock a wall endpoint with a fixed-anchor relationship in its constraint editor. Select the wall and drag away from that point.
  - Expected: The move is refused with an explanation. Neither the selected wall, its neighbors nor its openings change. Canceling the failed drag leaves the drawing usable.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

## Wall corners

- [ ] **U292 — Draw a clean wall corner and keep its window cut**
  - Steps: Draw two walls meeting at a right angle. Set different wall thicknesses. Place a window away from the corner. Select and move a wall, undo, then export a plan PDF.
  - Expected: The inside and outside wall faces meet cleanly at the corner. There is no diagonal line or overlapping end cap through the joint. The window remains a real opening at its stored position and width. Wall lengths and exterior appraisal calculations use the stored geometry. The PDF shows the same joined corner.
  - Result: Not tested
  - Notes / steps to reproduce: ____________________

- [ ] **U293 — Join a partition to the middle of a wall**
  - Steps: Draw a long wall. Draw a second wall from its midpoint at a right angle, then press Escape to finish. Select each wall, change their thicknesses, and export a plan PDF. Also try drawing a partition across the long wall.
  - Expected: The partition meets the host face cleanly. No wall end cap or host-face line crosses the solid junction. The exterior face stays continuous, each wall remains selectable, and the PDF has the same outline. Wall lengths remain the lengths you entered.
  - Also try: Place a window across the meeting point. A partition ending inside that opening has an exposed end; the app must not draw wall material through the opening. Walls on separate floors must keep their own outlines.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U294 — Calculate exterior square footage with interior partitions**
  - Steps: Draw a closed rectangular wall layout. Add a partition from the middle of one side into the room, and another wall across the room. Select the interior partition, right-click, and choose **Measure exterior from walls…**. Cancel once, then reopen the review and create the area. Repeat after selecting all the walls with Ctrl-click.
  - Expected: The preview traces the outside perimeter and reports how many candidate walls it excluded. The partitions do not increase or decrease the exterior area. Selecting the same layout again finds the existing area instead of counting it twice. The walls remain editable.
  - Also try: Change an exterior wall's thickness, refresh the area, and verify its square footage changes. Move or resize an interior partition and verify it does not change the exterior measurement. Save and reopen; the same measurement remains. Select walls from two separate buildings together; the app must request a single exterior rather than silently choosing one.
  - Result: Not tested
  - Notes: ______________________________

## Door mechanisms and readable wall dimensions

- [ ] **U295 — Place and edit a double door**
  - Steps: In Library, choose Door, then the Double style. Click a straight wall to place it. Double-click the door, open **Door operation**, change the angle to 70 degrees, and apply. Resize its width, undo, redo, save and reopen. Inspect the same door in 3D.
  - Expected: Two separate leaves and two swings appear. The leaves, wall cut and frame follow the edited dimensions and angle. Reopening retains the double mechanism. An angle that intersects its frame, wall or other leaf is refused without changing the project.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U296 — Slide a glass door open**
  - Steps: Choose the Sliding glass Door style and place it in a straight wall. Open **Door operation**, set Open to 50%, then 100%. Change the movable jamb and track side. Resize the width, undo, redo, save and reopen; compare the plan and 3D.
  - Expected: One half-panel moves along a separate track; the other stays fixed. At 100% the movable panel stacks behind the fixed panel. There is no swing arc. The glass, opening, travel and mechanism survive reopening. A curved host explains that this sliding construction requires a straight wall and leaves the drawing unchanged.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U297 — Read wall lengths beside exterior dimensions**
  - Steps: Draw a closed rectangular wall layout with a partition. Create its exterior measurement area. Zoom in and out, select a wall by its length label, then export and print preview the plan. Also try a diagonal wall and rotate the drawing.
  - Expected: Each wall length remains readable beside the exterior dimensions. Automatic wall labels avoid other measurement text and follow their wall's orientation. Existing manually placed dimensions stay where you placed them. Selection and the PDF agree with the visible labels.
  - Result: Not tested
  - Notes: ______________________________

## Window layouts and opening controls

- [ ] **U298 — Place double and triple windows**
  - Steps: Choose Window in Library, then the Double or Triple style, and click a straight wall. Resize the opening width. Open **Opening assembly**, switch between single, double and triple pane layouts, undo, redo, save and reopen. Inspect the same window in 3D and export the plan.
  - Expected: Double and triple layouts have two or three distinct framed glass panes separated by mullions. The wall cut follows the opening width. The panes, frame and layout survive edits and reopening; plan output and 3D agree.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U299 — Open and close a casement window**
  - Steps: Choose the Casement Window style. Set Angle to 70 degrees and place it on a straight wall. Double-click it and open **Opening assembly**. Change the hinge jamb, opening side and angle; set angle to zero to close it. Undo, redo, reflect its host wall, save and reopen.
  - Expected: The glazed sash rotates about its actual jamb attachment while the frame stays in place. Angle zero closes it. The plan and 3D show the same sash. A pose that intersects its host or frame is refused without changing the project. A curved host explains the straight-wall requirement.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U300 — Slide a window open**
  - Steps: Choose the Sliding Window style and set Open to 50%. Place it on a straight wall. In **Opening assembly**, try 0%, 50% and 100%, change the moving half and track side, then resize its width. Undo, redo, save and reopen. Compare plan, 3D, DXF and IFC output.
  - Expected: One framed sash moves on a separate track; the other remains fixed. At 100% the moving sash stacks behind the fixed one, leaving half the aperture open. There is no hinged swing. Reopening and exported native geometry retain the window layout and travel.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U301 — Place and reshape a bay window**
  - Steps: Choose Window in Library, then Bay. Set Projection to 0.65 m and choose a side of a straight wall. Place it. Double-click it and open **Opening assembly**. Change Projection to 0.85 m, Front width to 40%, and the side. Resize its wall-opening width, undo, redo, reflect its host wall, save and reopen. Compare the plan and 3D; export DXF and IFC.
  - Expected: The bay projects beyond the selected wall face with a front pane and two angled side panes. Changing depth, front width or side reshapes the same assembly. The wall cut, frame and glass remain attached and follow width edits. Reopening and exported geometry retain the shape. Invalid dimensions are refused without changing the project. The window alone does not add floor or appraisal area. A curved host explains the straight-wall requirement.
  - Result: Not tested
  - Notes: ______________________________

## Plan label placement

- [ ] **U302 — Place a calculated area label where it is readable**
  - Steps: Create and qualify a small appraisal area, then furnish a larger area until its interior is crowded. Double-click an area, choose **Place label** in Area attributes and click a clear position on the canvas. Choose **Automatic** to restore automatic placement. Repeat placement but press Escape before clicking. Undo, redo, save and reopen, then export the plan PDF.
  - Expected: Names and calculated values remain visible; a label that cannot fit inside has a leader to its area. Placement changes only the label position. The click does not draw a wall or start a shape. Escape preserves the previous position. Reopening and PDF retain the manual placement; editing the boundary still updates the calculated value. Automatic restores normal placement. A read-only project refuses placement edits.
  - Result: Not tested
  - Notes: ______________________________

## Appraisal reports and plan sheets

- [ ] **U303 — Inspect an area's calculation and its deductions**
  - Steps: Qualify a 10 ft by 10 ft dwelling area. Create an internal 5 ft by 5 ft garage, declare Garage use and subtract it from the dwelling. Open **Tools > Appraisal area report…**. In **Areas and deductions**, select the dwelling and expand its deduction row. Select the garage, then click **Show on canvas**.
  - Expected: The dwelling has 100 sq ft gross, 25 sq ft deducted, 75 sq ft net and factor 1/1. The garage contributes 25 sq ft to its own category. The detail identifies source areas, requested/applied deductions, perimeter, floor grade, property policy and rounding. Show on canvas selects the garage without editing it. Hiding a layer does not change the report's quantities.
  - Also try: Change a source while the report is open. Export or Show on canvas must ask you to Refresh. A non-unity factor exposes physical and adjusted diagnostics while property totals remain withheld.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U304 — Export the complete appraisal calculation report**
  - Steps: Create enough qualified non-overlapping areas to require several pages. Give the final area a long descriptive name. Open the report and choose **Export PDF…**. Reopen the PDF and inspect the first, middle and final pages. Repeat in Metric units and with a missing required appraisal fact.
  - Expected: Every area and its calculation details appear, including the final area. Names wrap, quantities use the chosen units and precision, and pages identify the project revision. Missing facts show specific issues and withhold qualified totals. Export leaves the project unchanged and refuses to overwrite the open project or its recovery file.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U305 — Add a plan sheet with an appraisal summary**
  - Steps: Draw a qualified appraisal plan. Open **Sheet layout manager…**, select a horizontal plan and click **Create appraisal plan sheet**. Cancel once, then repeat and accept. Adjust scale if needed. Undo, redo, save, reopen and use **Export drawing set PDF**.
  - Expected: Cancel adds nothing. Accept adds one A3 page with a large plan and separate area summary while preserving the previous sheets. One Undo removes the entire addition; Redo restores it. Reopening and PDF retain both placements and the qualified quantities. A summary too long for its box identifies omitted rows and points to the complete report.
  - Result: Not tested
  - Notes: ______________________________

## Boundary editing and diagnostic repairs

- [ ] **U306 — Restore a deleted edge measurement**
  - Steps: Draw a room, delete one edge-length dimension, then choose **Tools > Add length, angle or area dimension…**. Choose the room and missing edge, leave exterior placement checked and click **Add length**. Change the edge length, undo, redo, save and reopen.
  - Expected: The restored label measures the selected edge and appears outside the room. It updates with geometry, preserves its edge association after reopening, and each committed edit can be undone.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U307 — Change a boundary curve without moving its endpoints**
  - Steps: Draw a measured room, select it and choose **Edit curve…**. Choose an edge and enter a signed sweep angle, signed height or arc length. Review the preview, then Apply. Try the opposite side, undo, redo, save, reopen and export the plan. Try a curve that crosses another edge.
  - Angle check: Enter `1` without a unit; Apply must be disabled and explain how to enter the angle. Try `90 deg`, `pi/2`, and `-pi/2`; the first two produce the same quarter-circle sweep and the last chooses the opposite side.
  - Expected: Both endpoints stay fixed. The selected edge becomes the requested analytical curve; associated lengths, area and perimeter update. The preview shows the proposed result before commitment. An invalid crossing disables Apply and leaves the drawing unchanged. Saved and exported geometry agrees.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U308 — Locate and repair an invalid appraisal area**
  - Steps: Open the appraisal report with a missing policy or invalid required area fact. Select the affected area in **Areas and deductions**, inspect its issue and click **Show on canvas**. Repair the declaration, reopen or Refresh the report, then export PDF.
  - Expected: Missing policy retains valid diagnostic quantities but withholds qualified totals. An invalid source remains selectable and shows unavailable quantities with a specific issue. Repair restores current measurements and qualification only when all requirements are met. The PDF retains the same status and reasons.
  - Result: Not tested
  - Notes: ______________________________

## Curved exterior walls and appraisal measurements

- [ ] **U309 — Measure a building with a curved exterior wall**
  - Steps: Draw a closed physical wall layout containing a curved wall and an internal partition. Select a wall and choose **Measure exterior from walls…**. Inspect the exterior outline, area, perimeter and excluded partition count; cancel once, then create it. Declare the appraisal facts. Change a perimeter wall's thickness, select the measured area and choose **Refresh exterior measurement…**. Undo, redo, save, reopen and export the plan PDF.
  - Expected: The measurement follows the outside of the actual curved and straight walls, with each wall's thickness included. The curved part stays curved; partitions do not add area. The review leaves the project unchanged until accepted. Area, perimeter and edge measurements update together. Changing a source wall marks the old measurement stale and withholds qualified appraisal totals until refreshed. Undo, reopening and PDF retain the same curved outline and measurements. An ambiguous or crossing layout gives a specific error without changing the project.
  - Result: Not tested
  - Notes: ______________________________

## Read measurements while editing

- [ ] **U310 — Select an object without covering its measurements**
  - Steps: Create an exterior measurement from a straight or curved wall layout. Select the measured area, then zoom in and out. Move a wall's measurement label manually near the selection frame and rotate its text. Select the area again, drag its visible rotation pin, undo, save/reopen and export PDF.
  - Expected: Complete baseline and exterior measurements stay readable. The selection frame and rotation connector leave space around the text, and the rotation pin and size badge occupy clear positions. The displaced pin still rotates the selected object. Selection and navigation leave saved measurement values, manual label positions and styles unchanged. PDF retains the measurements and omits selection controls.
  - Result: Not tested
  - Notes: ______________________________

## Custom area details

- [ ] **U311 — Add and edit custom area details without JSON**
  - Steps: Double-click a closed area and open **Area attributes > Details…**. Add Name/Value rows for Finish = Oak and Use = Conditioned. Change a value, remove a row and Save. Try a duplicate name and an empty name; fix them, then Cancel. Reopen and Save without changes. Undo, Redo, save the project and reopen it.
  - Expected: Invalid names explain the problem and prevent Save. Cancel and saving unchanged rows leave the drawing and history unchanged. Accepted details survive Undo/Redo and reopening. Custom details do not change the area's geometry, appraisal facts, colors or square-footage totals.
  - Result: Not tested
  - Notes: ______________________________

## Exact keyboard drawing

- [ ] **U312 — Draw walls by typing exact lengths**
  - Steps: Leave Draw set to Wall, set thickness to 7 in and height to 9 ft, and click a start point. Type 12 ft 6 in and press Right, type 8 ft and press Up, then repeat the lengths with Left and Down to close. Undo, Redo, save and reopen. Repeat in Metric using centimetres and metres.
  - Expected: The lengths bypass grid rounding. Each wall keeps the chosen depth and height and meets the preceding endpoint exactly. Closing returns to ordinary canvas use. Undo removes one wall; Redo and reopening restore its geometry.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U313 — Enter a measured outline without opening a dialog for every edge**
  - Steps: Choose Area, click a start point and type lengths followed by Right, Up and Left. Undo and Redo an unfinished edge, then press Enter on the canvas to close. Repeat through Define First, placing each requested dimension before entering the next edge.
  - Expected: Typed measurements retain their units. Draft Undo/Redo leaves the saved drawing unchanged until closure; the completed area is one undoable operation. Define First waits for each dimension placement. The existing D dialog remains available for angles and curves.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U314 — Use the drawing keypad and cancel an invalid measurement**
  - Steps: Start a wall, expand 123, enter a length with the keypad and choose a direction. Try zero, a negative length and invalid text. Press Escape in the field, then enter a valid length. Change Imperial/Metric while another entry is pending. Finish the wall chain with Escape on the canvas.
  - Expected: Keypad and keyboard use the same measurement control. Invalid input explains the problem and changes no geometry. Escape clears pending text while retaining committed walls. Changing units clears the pending entry rather than reinterpreting it. The keypad can collapse again to recover drawing space.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U315 — Start and draw an angled wall with the keyboard**
  - Steps: Leave Draw set to Wall, set the wall depth and height, and press D before clicking a start point. Enter exact start coordinates. Press D again, choose Length / heading, enter 2 m and 45 deg, and add the wall. Repeat with Rise / run and Line to world coordinate. Try an angle without a unit and cancel once.
  - Expected: The start point adds no wall. Each accepted edge adds one physical wall using the entered geometry and chosen depth and height, without grid rounding. Invalid input explains the problem. Cancel retains the current chain. Undo, Redo and reopening retain the wall.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U316 — Draw a physical curved wall from its measurements**
  - Steps: Start a Wall chain and press D. Draw an arc with chord endpoints and a 90 deg sweep. Repeat using signed chord height, chord/arc length and start tangent/arc length/sweep. Try Clockwise for chord/length. Finish the chain, select a curve, inspect its dimensions, save and reopen. Repeat in Imperial and Metric.
  - Expected: Each result is a curved wall with the chosen thickness and height, not a straight chord or a decorative symbol. Arc-length input measures along the curve. The selected construction and measurements survive reopening, and one Undo removes the wall and its new endpoint connections together.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U317 — Turn from the preceding wall and recover from cancelled input**
  - Steps: Draw a wall, press D and choose Relative turn. Enter a length and 90 deg. Repeat after a curved wall. Cancel a proposed next edge, then enter it again. Change Imperial/Metric while the form is open and try adding the pending edge. Finish the chain, undo and redo.
  - Expected: A relative turn uses the preceding wall's ending direction, including the ending tangent of a curve. Cancel retains the accepted walls and current endpoint. Changing units invalidates the pending form instead of reinterpreting it. Accepted walls remain independently undoable.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U318 — Edit a curved wall's classification**
  - Steps: In Imperial, create a curved wall with **Draw curved wall** and add a hosted opening, a name and another wall property. Select the wall and open **Edit curve…** in Properties. Change only Classification and Apply; compare the exact endpoints, sweep and arc length before and after. Reopen **Edit curve…**, change both the curve measure and Classification, and Apply once. Undo once, then Redo. Repeat the workflow in Metric.
  - Expected: Changing only Classification in **Edit curve…** leaves the exact curve geometry unchanged. Changing the curve and Classification together is one undoable operation: one Undo restores both previous values, and Redo restores both new values. Hosted openings remain attached, and the wall name and other properties are retained in both unit systems.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U319 — Undo and redo while continuing a wall chain**
  - Steps: In Imperial, start a wall chain and add two edges. Press Ctrl+Z, then Ctrl+Y; press Ctrl+Z twice and confirm the original start point remains active. Add an edge, undo it, add a different one and press Ctrl+Y. Finish the chain. Start another chain and place only its start point, then click toolbar **Undo** before drawing a wall. Repeat in Metric.
  - Expected: Ctrl+Z removes only the last accepted chain edge and resumes at the retained endpoint where that edge began; Ctrl+Y restores the edge and resumes at its restored end. Undoing all edges returns to the original anchor while keeping the chain active. Adding a different edge after Undo abandons the old redo branch. Toolbar Undo with only an unfinished start point cancels that draft without changing project geometry or history.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U320 — Change a curved boundary length with a related wall**
  - Steps: In Imperial, create a closed area with a curved edge and a wall joined to that edge through a saved relationship. Select the area and edit the curved edge's physical length. Leave **Move related objects** checked and Apply; inspect the joined wall and undo. Repeat with the option unchecked. Repeat both choices in Metric.
  - Expected: The option is on by default. With it checked, the boundary and connected wall update together as one undoable edit. With it unchecked, an edit that would leave invalid connected geometry is refused without changing either object. The area remains valid after Undo and Redo.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U321 — Replace the recorded walls of a stale exterior measurement**
  - Steps: Create and name an exterior measurement from a closed wall shell. Set its style, declare appraisal facts, and link a deduction. Delete a perimeter wall and redraw its equivalent. Try **Refresh exterior measurement…** and confirm it cannot refresh from the removed source. Open **Replace source walls…** from Tools or the measured area's right-click menu, select the replacement shell wall, and review the proposed sources and counts. Cancel and confirm nothing changed; reopen, Apply, then inspect dimensions and appraisal totals. Undo, Redo, save and reopen.
  - Expected: Ordinary Refresh reports that its recorded sources are unavailable and leaves the measurement unchanged. Replacement shows the recorded shell in gray and proposed shell in blue, with source details for review. Cancel is non-mutating. Apply preserves the measurement's owner, facts, name, style, deduction relationships and dimensions while restoring current qualified appraisal totals. Undo/Redo and reopening retain the replacement. When replacement geometry differs, including with the same edge count, the reference planner asks how supported manual dimensions and constraints should be mapped or removed; ambiguous or unsupported references cannot be guessed.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U322 — Map a manual dimension when replacing changed exterior geometry**
  - Steps: Create a four-sided exterior measurement from walls, declare its appraisal facts, and add a manual dimension to one boundary edge. Delete and redraw the shell with one corner moved, keeping four sides. Open **Replace source walls…**, choose the new shell, and review the changed outline and source counts. In the reference planner, cancel once and confirm the measurement is unchanged. Reopen repair, map the manual dimension to its intended new edge, then Apply. Inspect the new area and perimeter, Undo and Redo, and save and reopen.
  - Expected: Changed geometry with an equal number of edges still asks you to choose the manual dimension's intended new edge. Cancel leaves the original measurement and dimension intact. Apply replaces the measured outline, retains the dimension at its chosen location, and updates area/perimeter and qualified totals. Undo/Redo and reopening preserve the chosen replacement and mapping.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U323 — Copy an appraisal area with its deduction**
  - Steps: Declare a dwelling area and a contained garage deduction with appraisal facts. Select the parent and choose **Clone**; place the copy without overlapping the original. Check both appraisal totals, then change the copied garage boundary and check the totals again. Undo and Redo, save and reopen. Next, select only the parent, use Copy and Paste, and drag the pasted group clear of the original; verify its deduction and totals.
  - Expected: Each copy includes the required garage boundary and deduction relationship, preserving the parent's net area, category and appearance. Editing the copied garage changes only the copied parent's net. The original parent, garage, appearance and totals remain unchanged. Undo/Redo and reopening retain the copies and their deduction links. Copy and Paste of the parent also carries the required deduction so the pasted group can be placed without overlap and qualified independently.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U324 — Rotate or mirror an appraisal area and its deduction**
  - Steps: Create a dwelling area with a garage deduction. Select only the dwelling and use its rotation handle; confirm the garage follows in the preview, then press Escape. Rotate again and release. Open **Transform selection**, cancel once, then try both horizontal and vertical flips. Check net area and category totals, Undo and Redo, save and reopen. Repeat with an exterior area measured from walls containing a hosted window or door, and with a light-commercial occupiable area and service deduction.
  - Expected: Preview includes the parent, deduction and, where used, supporting walls and hosted openings. Cancel leaves them unchanged. Rotation or flipping keeps the same objects, facts, appearance and deduction links; it preserves unrounded areas and category totals. Undo/Redo moves the complete group in one step. Reopening preserves its geometry and current exterior sources. A transformation that would invalidate another area's shared walls, deduction or external locked relationship is refused without changing the drawing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U325 — Change a wall and keep its exterior appraisal measurement current**
  - Steps: Draw a closed exterior wall shell and use **Measure exterior from walls**. Name the resulting area, set its color, declare its appraisal facts and add a garage deduction. Select a source wall and change its thickness in Properties. Then edit a wall length with connected objects enabled. Inspect the measured outline, dimensions and appraisal totals after each edit without choosing Refresh. Undo once, Redo, save and reopen. Repeat in Imperial and Metric and with a curved exterior wall.
  - Expected: Each valid wall edit updates its existing linked outline and dimensions in the same operation. Net appraisal totals recalculate automatically; the area keeps its name, color, declarations and deduction. One Undo restores the complete previous wall and measured outline; Redo and reopening retain the update. An edit that conflicts with a locked measurement or places a deduction outside the area is refused without changing the drawing. A measurement whose sources were already missing or stale remains visibly in need of repair.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U326 — Move walls and furniture together without losing the measured outline**
  - Steps: Draw a closed exterior wall shell, add a door or window, and measure its exterior. Name the measured area and declare its appraisal facts. Place a furniture symbol and a text label inside it. Ctrl-click the shell walls, furniture and label to select them together, leaving the measured area unselected. Drag the group a short distance; inspect the preview, then cancel with Escape. Repeat and release. Inspect the walls, opening, symbol, label, exterior dimensions and appraisal totals. Undo once, Redo, save and reopen. Repeat in Imperial and Metric, including a curved shell wall.
  - Expected: The complete preview shows the linked exterior outline and dimensions following the walls alongside the furniture, label and hosted opening. Cancel changes nothing. Release commits the complete group in one step and keeps the exterior measurement current with its existing name and appraisal facts. The area's unrounded size and qualified totals remain unchanged by translation. Undo/Redo and reopening preserve the complete operation. Invalid or locked relationships refuse the move without leaving part of the selection behind.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U327 — Draw useful lengths without decimal-inch mouse placement**
  - Steps: In Imperial with Snap on, start a wall away from the world origin. Move along both a horizontal direction and a diagonal, inspecting the live length before clicking. Zoom out and repeat; zoom very close and repeat. Draw a measured boundary in the same way. Switch to Metric and repeat. Then enter an exact typed dimension that is not a multiple of the current mouse increment and place it.
  - Expected: Mouse lengths settle on common feet/inch increments when wider out and fractional inches close in; Metric uses common meter, centimeter and millimeter increments. The visible length agrees with the committed geometry. Diagonals do not produce arbitrary decimal-inch values from cursor pixels. Typed input remains exact. Snapping to a real existing endpoint or closing a boundary preserves that exact point, even if its distance is not a common increment.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U328 — Pan using the right mouse button**
  - Steps: Right-click and hold, then drag across empty canvas. Repeat over a selected object, while a wall is pending, and while a new symbol is waiting for placement. Release the button. Compare the drawing and selection before and after.
  - Expected: Right-drag pans the view with no object movement, new node, cancellation or context menu. The selection and pending action remain available. A stationary right-click remains distinct from a drag.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U329 — Cancel a pending wall or new symbol with right-click**
  - Steps: Start a wall with a left-click, move its pending endpoint and right-click without dragging. Next place one wall in a chain and right-click while its next wall is pending. Start a measured area and right-click before closing it. Choose a new library symbol for placement and right-click before placing it. Repeat after placing a symbol, with another new placement pending.
  - Expected: Right-click cancels the pending drawing or placement and removes its preview. Previously committed walls and symbols remain. No context menu or new object is produced by the cancellation. With no pending action, a right-click opens the relevant context menu.
  - Result: Not tested
  - Notes: ______________________________

## Project appraisal details

- [ ] **U330 — Read GLA and floor contributions without selecting an object**
  - Steps: Open the left panel's **Details** tab with nothing selected. Use **Setup** to enable appraisal and declare the property kind and measurement basis; cancel once, then save. Select an area row, inspect its dimensions and deductions, and use **Edit facts** to supply missing observations. Check the GLA and separate garage/below-grade totals. Use **Show on canvas** and **Full report**. Switch Imperial/Metric, hide a layer, edit a qualifying area's dimensions, Undo, save and reopen.
  - Expected: Details remains accessible without a canvas selection. Required facts are explained and totals are withheld until qualified. GLA and separate totals agree with the current authoritative report; hiding a layer does not change them. Area rows expose current dimensions, perimeter and the gross/deduction/net breakdown. Edits and units refresh the display. Setup Cancel changes nothing; Save is undoable. The actual calculation policy and unverified ANSI status are clear; the application does not imply ANSI approval.
  - Result: Not tested
  - Notes: ______________________________

## ANSI-oriented measurements

- [ ] **U331 — Enable ANSI-oriented rules and read the actual GLA**
  - Steps: Open **Details > Setup**, choose **ANSI Z765-2021**, a single-family property and Exterior measurement. Declare inspection, direct measurement and the acquisition increment; cancel once, then save. Draw a 10 ft by 10 ft measurement area. In **Edit facts**, declare above grade, finished, direct interior access, primary dwelling, year-round suitability, comparable finish and a flat minimum ceiling height of 7 ft. Check Details, then switch the workspace to Metric and open **Full report**.
  - Expected: Cancel changes nothing and saved setup/facts are undoable. Details shows 100 sq ft of primary GLA and 10.0 ft analytical edges in either workspace. Metric equivalents are supplementary. The actual profile, passed Vertex rule checks and pending final-standard validation are visible; no ANSI approval is asserted.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U332 — Keep below-grade, low ceilings and ADUs out of primary GLA**
  - Steps: With ANSI-oriented appraisal enabled, declare a finished area with a flat 6 ft 11 in ceiling. Inspect its category and reason. Change it to 7 ft and inspect again. Choose tenth-foot acquisition and try observed heights of 6.96 ft, 6.85 ft and 6.951 ft; inspect the Facts preview, Details and exported Full report PDF. Switch acquisition to inches while retaining 6.951 ft. Save/reopen and undo the changes. Mark its floor partly below grade. Add another finished measured area and identify it as an ADU, then check the separate totals.
  - Expected: Facts shows the rounded height used for classification; Details and PDF also retain the recorded observation. Tenth-foot acquisition reports 6.96 ft as 7.0 ft and 6.85 ft as 6.9 ft. The first passes the flat-height rule; the second remains nonstandard finished. At 6.951 ft, tenth-foot acquisition reaches 7.0 ft while inch acquisition reaches 6 ft 11 in and remains nonstandard. Changing precision does not replace the original observation. Undo and reopening preserve the facts and resulting totals. Any partly below floor is reported wholly below grade; ADU area stays separate from primary GLA. Totals update without a separate Calculate action.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U333 — Record a sloped room and actual low-height exclusions**
  - Steps: Draw a complete measured room and contained areas representing portions below 5 ft; link these as deductions. In Edit facts choose Sloped, record the measured area at least 7 ft high and select the real low-height deductions. Save and inspect the room's gross, excluded and net areas and rule notes. Change the room outline, including a change that keeps its area equal, then inspect Details. Undo the geometry edit.
  - Expected: Low-height portions use actual drawn geometry and subtract only their union. Evidence binds the complete room and its exclusions. Geometry changes withhold stale ceiling qualification until observations are updated; Undo restores the original evidence and measurements. The provisional sloped-room denominator and unresolved final-standard interpretation are visible rather than presented as approval.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U334 — Inspect and print the measurement evidence**
  - Steps: Select an area row in Details and inspect edge lengths, perimeter, gross, deductions, net, classification reasons and ceiling evidence. Open Full report and export its PDF. Save and reopen the project and compare the figures. Hide the area's canvas layer and check the totals again.
  - Expected: Details and PDF show the same current analytical dimensions and canonical square-foot results, inspection/method declarations, limitations and separate ADU/nonstandard areas. Reopening preserves the evidence. Hiding a layer does not change totals. Output identifies itself as a measurement summary and does not claim a complete UAD appraisal report.
  - Result: Not tested
  - Notes: ______________________________

## Exact corner corrections and appraisal sheets

- [ ] **U335 — Correct a completed corner with exact coordinates**
  - Steps: Close a measurement area. Open its geometry editor, choose Vertex position, select a corner and enter X and Y. Preview the change, then cancel once. Repeat and apply it. Check connected geometry, Undo/Redo, save and reopen. Repeat in Metric and Imperial; try an explicit unit such as `2 in` while Metric is selected.
  - Expected: The editor shows the current corner coordinates and the actual proposed outline. Cancel preserves the drawing. Apply updates that corner and its validated relationships in one Undo step. Invalid outlines or locked/conflicting geometry are refused with an explanation. The entered coordinates survive reopening.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U336 — Print ANSI-oriented totals on a saved plan sheet**
  - Steps: With ANSI-oriented appraisal configured and a qualifying 10 ft by 10 ft area, create an appraisal plan sheet including its area summary. Save and reopen, then export the plan PDF in Imperial and Metric. Compare the sheet's dimensions, GLA and profile status with Details and Full report.
  - Expected: Both PDFs show the canonical 100 square feet and 10.0 ft dimensions. Metric values may appear as supplementary measurements. The sheet identifies the actual profile and pending final-standard validation; its totals do not switch to ordinary workspace rounding. Saved sheets remain consistent with the current area calculations.
  - Result: Not tested
  - Notes: ______________________________

## Exterior corner corrections

- [ ] **U337 — Correct an exterior measurement and its physical walls together**
  - Steps: Draw a closed exterior wall loop with different wall thicknesses. Create its measured exterior. In the measured area's geometry editor, choose Vertex position, select a corner and enter X/Y. Inspect the proposed walls, area and dimensions; cancel once, then apply. Check the physical walls, other corners and GLA in Details. Undo, Redo, save and reopen. Repeat in Imperial and Metric and with a curved exterior wall.
  - Expected: The physical walls reshape to produce the requested exterior corner. Other exterior corners and wall thicknesses remain unchanged. Measurements, dimensions and GLA use the resulting geometry. Cancel changes nothing; one Undo restores all affected geometry. Saved source correspondence and original construction history remain intact. Invalid geometry or conflicting locks give an explanation and cannot apply.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U338 — Drag an exterior corner with attached walls and openings**
  - Steps: Add a partition attached to an exterior wall and a hosted door or window. Derive the exterior measurement. Select it and drag a corner handle, checking the live walls and measurements. Release, Undo and Redo. Try a change that makes an opening too large for its host and a change conflicting with a fixed measurement.
  - Expected: The live preview and committed geometry agree. Partitions stay attached and hosted openings follow their physical wall. A refused edit preserves the entire drawing and history. The accepted change updates the measured exterior and its calculation in one Undo step.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U339 — Keep CAD source layers when importing**
  - Steps: Import a DXF with several named layers. Accept the proposed destinations. Check that missing source-named layers appear on the active floor. Import it again and check that matching layers are reused. Undo each import and reopen the saved project.
  - Expected: Source layers remain distinct. Matching layers are reused instead of duplicated. Newly created layers and their contents undo together. Source bytes and the chosen mappings remain in the saved project.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U340 — Assign imported doors and windows with their walls**
  - Steps: Import a Vertex DXF containing a wall and hosted door or window on separate CAD layers. Assign those layers to different floors and try Import. Repeat with two destination layers on the same floor.
  - Expected: The different-floor assignment is refused with an explanation and changes nothing. The same-floor assignment imports editable walls and openings on the chosen layers, preserving their source coordinates, elevation, size and host relationship. Hiding the opening's layer hides it independently; hiding their shared floor hides both.
  - Result: Not tested
  - Notes: ______________________________

## Building-object appearance

- [ ] **U341 — Change a wall's plan appearance**
  - Steps: Draw a wall, double-click it to open its quick properties, then open its appearance editor. Change Outline color, Fill color, Fill pattern and Line width. Apply and compare the canvas and an exported PDF. Undo, Redo, save and reopen.
  - Expected: The physical wall uses the chosen colors, pattern and paper line weight. Its length, thickness, hosted openings and appraisal measurements stay unchanged. The accepted appearance survives history and reopening.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U342 — Style a door or window independently of its wall**
  - Steps: Add a door and window to a wall. Open one opening's appearance editor and change its outline color. Apply, export a PDF, save and reopen. Compare the other opening and the host wall.
  - Expected: The chosen opening's plan geometry uses its appearance. The other opening and wall retain their own appearance; opening width, position, operation and host relationship remain unchanged.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U343 — Hide and restore a building object**
  - Steps: Uncheck visibility in a wall or opening's appearance editor and Apply. Select that same object from the Layers tree and restore visibility through its properties. Check Undo/Redo and save/reopen.
  - Expected: Hiding changes the presentation and output without deleting the object or changing measured totals. The hidden object remains available in Layers and can be restored. History and reopening preserve the chosen state.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U344 — Reset a building object's appearance**
  - Steps: Customize a wall and an opening differently. Reopen one object's appearance editor and Apply without changing anything. Cancel a separate attempted edit. Finally choose Reset to defaults and Apply; Undo once.
  - Expected: Unchanged Apply and Cancel add no edit. Reset restores only the chosen object's default appearance, keeping the other object's appearance and all geometry. Undo restores the custom appearance.
  - Result: Not tested
  - Notes: ______________________________

## Saved-view appearance

- [ ] **U345 — Style two saved views differently**
  - Steps: Draw a wall with a door. Create two named plan views of it. In Architectural view settings, choose Drawing appearance and give each whole view a different outline color and line weight. Switch between them, put both on a sheet, export PDF, save and reopen.
  - Expected: Each view retains its own appearance on the canvas and sheet/PDF. Wall and door dimensions and appraisal totals remain unchanged.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U346 — Override one object's appearance in one view**
  - Steps: Give a saved view a custom appearance, then choose its wall in Drawing appearance and set a different style. Check the hosted door and a second saved view. Reset only the wall override. Undo and Redo.
  - Expected: The wall override affects only that wall in that view. Other objects and views retain their settings. Reset returns to inherited styling; Undo restores the override.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U347 — Hide and restore a complete saved view**
  - Steps: Add an annotation or reference image and a saved view. Turn off that view's visibility in Drawing appearance. Check its canvas and sheet/PDF, then restore it through the saved-view selector and settings.
  - Expected: The hidden view's geometry, annotations and reference content do not draw or print. Its definition remains available so it can be restored without deleting or recreating anything.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U348 — Keep view styling through source filtering and deletion**
  - Steps: Style a wall in one view. Exclude that wall using the view source filter, then include it again. Delete the wall and Undo. Save and reopen after each accepted edit.
  - Expected: Filtering preserves the wall's authored style for re-inclusion. Deletion removes its dependent view settings together; Undo restores the wall and those settings. No invalid references or geometry changes occur from styling.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U349 — Change one component's colors**
  - Steps: Place two copies of the same sofa. Double-click one, choose Colors, clear Use library colors and change its Outline and Surface. Apply, then inspect both on the canvas and in PDF. Undo, Redo, copy/paste, save and reopen.
  - Expected: Only that placed component changes. Its shading, glass/recess details, size and rotation remain intact. Accepted colors survive output and reopening; the copied component retains them.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U350 — Restore a component's library colors**
  - Steps: Open Colors on a customized component, change a color and Cancel. Reopen, check Use library colors and Apply. Undo and Redo.
  - Expected: Cancel leaves the drawing unchanged. Reset restores the original appearance of only that component. Undo restores its custom colors; Redo resets them again.
  - Result: Not tested
  - Notes: ______________________________

## Editing within styled saved views

- [ ] **U351 — Move a wall into a saved view's crop**
  - Steps: Draw two connected walls and add a window. Make a saved plan view with a crop excluding one wall. Hide that wall globally, then restore it in this view through Drawing appearance and give it a different outline color. Select the connected wall and drag it so the restored wall enters the crop. Inspect the live proposal; cancel once, then repeat and commit. Undo and Redo.
  - Expected: The restored wall appears as it enters the crop, using its view-specific color. Its hosted window follows the proposed wall and respects host visibility. The committed result agrees with the preview. Cancel and Undo restore the original drawing. Other saved views keep their own visibility and style.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U352 — Preview and apply an exact length in a saved view**
  - Steps: In a styled saved plan view, select a wall and open its length/constraint editor. Enter a new length and choose Preview. Cancel once, then repeat and Apply. Check the resulting wall, connected geometry and view-specific appearance. Undo the edit.
  - Expected: The comparison diagram shows the proposed geometric change. Cancel preserves the drawing. Apply updates the geometry in one edit while the saved view retains its styling and visibility. Undo restores the previous dimensions and appearance.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U353 — Rotate and reflect a measured curved wall**
  - Steps: Create a curved wall from an arc length. Open Transform selection, flip it horizontally, then vertically, then flip both axes together. Rotate it by 37 degrees and translate it. Repeat with Copy enabled. Undo and Redo each edit, change the copied wall's length, then save and reopen.
  - Expected: The actual curve follows the chosen rigid transform. A single reflection reverses its bend; two reflections preserve its bend. Rotation and translation preserve arc length. Copy leaves the original in place. Original construction information and properties survive the edits, Undo/Redo and reopening. Conflicting locked connections refuse the complete change with an explanation.
  - Result: Not tested
  - Notes: ______________________________

## Appraisal measurements while drawing walls

- [ ] **U354 — Close walls and inspect their measured exterior**
  - Steps: Open Details > Setup, enable appraisal calculations and choose Exterior as the measurement basis. Draw a rectangular wall outline and click its first corner to close it. Open Details and select its measured area. Check the dimensions, perimeter and gross area. Undo once, then Redo and save/reopen.
  - Expected: Closing creates an exterior measurement automatically, including the walls' thickness. It has edge dimensions and appears in Details. GLA waits for your actual floor and area observations. One Undo removes the closing wall and its measurement together; Redo and reopening restore them.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U355 — Draw an interior loop without duplicating appraisal area**
  - Steps: In an Appraisal/Exterior project with an existing measured outline, draw and close a smaller wall loop inside it. Repeat with a loop crossing the original measured outline. Check the message, original measurement and physical walls; Undo and Redo.
  - Expected: The newly drawn walls remain. Vertex explains why it did not add an automatic measurement. It does not count the same floor space twice or guess that the new loop is a deduction. The original measured area remains intact.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U356 — Close a wall outline with a precise curved wall**
  - Steps: In an Appraisal/Exterior project, draw three sides of an outline. Press D for precise input. Choose a curved wall defined by its chord endpoint and angle, enter the original starting point as the endpoint and use a 90-degree sweep to close the outline. Inspect the exterior measurement, arc dimension and Details. Undo, Redo and save/reopen.
  - Expected: The curve closes the outline and produces a current exterior measurement in the same edit. Its dimension measures the actual arc, rather than the straight chord. The physical curve and measured exterior survive Undo/Redo and reopening.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U357 — Review an ANSI measurement edit before applying it**
  - Steps: In an ANSI-oriented Appraisal project, add edge dimensions to a measured area. Open its boundary geometry editor and change an edge length. Compare the original and proposed dimensions in the preview and review table with the canvas. Repeat in Metric, then Cancel. Repeat in a project using ordinary measurement rules.
  - Expected: ANSI dimensions use tenths of a foot; Metric adds a supplemental metre value. Analytical areas use whole square feet. Editable coordinates and inputs retain the workspace units. Cancel preserves the geometry and dimensions. Ordinary measurement projects retain their usual unit formatting.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U358 — Return from another control without a stuck canvas gesture**
  - Steps: Hold Space over the canvas, move focus to Search or another input, release Space there, then click the canvas. Start a wall outline, use the keypad or left-panel controls and return. If an object move is still showing its pending preview, move focus to another control before it completes, then try a new move.
  - Expected: Normal clicks select or place a node without a stuck pan mode. The unfinished outline remains available. A canceled pending move cannot apply later, and a fresh move works normally.
  - Result: Not tested
  - Notes: ______________________________

## Comparing saved revisions

- [ ] **U359 — Compare two named drawing revisions**
  - Steps: Name an existing plan revision. Move a wall, change a dimension style, add a component and delete another. Name the revised plan. Open Named revisions, choose the two names, compare them and select the floor you want to inspect.
  - Expected: Before and After show their own geometry, dimensions, components and appearance. Pan or zoom either drawing; the other stays aligned. The text report still lists the changes.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U360 — Inspect a change overlay**
  - Steps: Compare two revisions and switch to Overlay. Check added and removed geometry and an edited wall or dimension against the legend. Switch back to side-by-side. Select a floor that exists in only one revision.
  - Expected: Changes are distinguishable using the legend. A missing floor produces an empty pane with an explanation; it does not substitute a different floor. Returning to side-by-side restores each revision's original appearance.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U361 — Keep current work while viewing history**
  - Steps: After naming a revision, make additional edits and begin an unfinished outline. Open Named revisions and compare the named revision with Current head. Close the dialog, finish or cancel the outline, then make an ordinary edit and Undo it.
  - Expected: Current head includes the latest committed edits. Comparing preserves current work, selection and the unfinished outline. Normal editing and Undo continue after closing history.
  - Result: Not tested
  - Notes: ______________________________

## Importing round CAD fixtures and preserving layers

- [ ] **U362 — Import an exact circle from a CAD plan**
  - Steps: Use Import DXF with `tests/fixtures/dxf/circle-mm.dxf`. Choose the destination floor/layer in the review. Fit the canvas and select the round pad. Undo and Redo, then save and reopen.
  - Expected: One complete circle appears with a 4 m diameter, rather than a missing item or a polygon. Its area is about 12.566 m² and perimeter about 12.566 m. The chosen layer and original DXF are retained. Undo/Redo and reopening preserve it. An imported shape does not automatically become verified GLA.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U363 — Edit an imported curved boundary**
  - Steps: Select the imported circle, run Upgrade boundary editing from Commands, then open its boundary geometry editor. Change a curved edge length, Apply, Undo and Redo. Save/reopen, then export DXF.
  - Expected: The selected curve's physical length can be edited precisely. Undo restores the circle. Editing and reopening preserve true curves; export retains curved geometry.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U364 — Keep annotation layers when exporting CAD**
  - Steps: Put two text labels and two components on different named layers. Export DXF and import the result into another project. Inspect source layers in the import review. Repeat with labels imported earlier into a chosen destination layer.
  - Expected: Labels and component linework use the layers you assigned. Reviewed imported labels retain the destination layer on later export. Export notes disclose unsupported symbol artwork rather than promising full native component fidelity.
  - Result: Not tested
  - Notes: ______________________________

## Keyboard alignment while drawing

- [ ] **U365 — Align a side with the outline's starting point**
  - Steps: Start an outline in Wall mode and draw two sides ending diagonally from the starting point. Check the X and Y guide choices. Press X; inspect the proposed horizontal side, then press Enter to accept it. Repeat with Y for a vertical side. Continue and close the outline. Repeat in Area mode and in Define First, placing each dimension before proposing the next side. Undo, Redo, save and reopen.
  - Expected: X ends the next horizontal side at the starting point's X coordinate. Y ends the next vertical side at its Y coordinate. The guide shows its length. X/Y preview a side without creating it; Enter commits the exact proposed endpoint. The starting point and previous sides stay in place. The committed side uses normal dimensions and drawing history.
  - Also try: Move the mouse after proposing a side, cancel with Escape, use X/Y while typing in a name field, and hold Ctrl while pressing them. Change the active layer or undo an earlier side before accepting a proposal.
  - Expected: An abandoned or stale proposal cannot commit. Typing and modified keys keep their ordinary behavior. A zero-length alignment is unavailable.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U366 — Align a drawing side with other walls using the arrow keys**
  - Steps: Draw a wall above the area where you will draw another outline. Start that outline below it. Press Ctrl+Right repeatedly to preview alignment with the existing wall's endpoint X coordinates, then Enter to accept the desired side. Try the other arrow directions. Press Ctrl+Shift+Arrow toward a wall crossing the cursor's horizontal or vertical path. Repeat in Area and Define First, placing each pending dimension before the next side. Try the same operations from Tools → Directional alignment. Undo, Redo, save and reopen the finished drawing.
  - Expected: Ctrl+Arrow advances through visible structural endpoint coordinates without changing the other coordinate. Ctrl+Shift+Arrow stops at an actual line or curve intersection in that direction. The guide displays the proposed length, and only Enter creates the side. A direction with no target leaves the cursor and proposal unchanged. Accepted geometry and dimensions use normal history and persist after reopening.
  - Also try: Move the pointer after proposing a side, change the displayed scene, type Ctrl+Arrow inside a text field, and use the keys during canvas dragging. Hide the target wall's layer. Place a symbol or reference image near the proposed path.
  - Expected: Abandoned or stale proposals cannot commit. Text navigation and canvas dragging remain available. Hidden walls, symbol artwork, reference images and grid lines do not become alignment targets.
  - Result: Not tested
  - Notes: ______________________________

## Reviewing measured-area sources

- [ ] **U367 — Reassign an appraisal area after its measured lines split it**
  - Steps: In Area draw mode, create a closed outline with an internal divider and define its areas. Set one area's facts to dwelling and another to garage. Add another divider that splits the dwelling. Select its retained area, open Details and click Review measured sources (also available by right-clicking the area). Choose a face; inspect the blue proposed outline against the gray retained outline. Apply it, resolving any referenced dimensions explicitly. Review the other affected areas as needed. Undo, Redo, save and reopen.
  - Expected: The selected area keeps its name, classification and facts while adopting only the chosen face. GLA updates after all affected areas and declarations are current. Newly unassigned faces are not silently classified or included. A face already assigned to another current area is unavailable. Cancel changes nothing.
  - Also try: Select a different face, cancel reference review, or change the project/units/layer while the dialog is open. Hide the source layer normally and review through Details.
  - Expected: An abandoned or stale review cannot apply. Normal layer hiding does not alter appraisal totals or invalidate source geometry.
  - Result: Not tested
  - Notes: ______________________________

## Measured-line drawing history

- [ ] **U368 — Undo a measured line and keep drawing from the restored endpoint**
  - Steps: Choose Measured lines in Draw. Start a stroke and place three sides. Press Ctrl+Z once; check that the last side disappears and the drawing preview starts at the previous endpoint. Press Ctrl+Y and place another side. Undo back to the starting point, Redo the sides, then Undo once and draw a different side. Repeat using typed distances and angles. Finish with Enter, Escape or a stationary right-click; save and reopen.
  - Expected: Each Undo removes one committed side and leaves the pen at its exact previous position. Redo restores the same side and moves the pen to its endpoint. Continuing after Undo keeps the existing stroke and replaces its abandoned Redo branch. Earlier typed measurements stay unchanged. Undo at an uncommitted starting point cancels only that point. Finishing or reopening does not restart drawing unexpectedly.
  - Also try: Change the active layer or workspace before using history, and make an unrelated edit before Undo.
  - Expected: History still navigates the document, but an obsolete drawing session cannot resume on the wrong layer, workspace or geometry.
  - Result: Not tested
  - Notes: ______________________________

## Precise measured-line input

- [ ] **U369 — Enter an exact measured-stroke starting point and side**
  - Steps: Choose Measured lines in Draw. Press D before clicking a starting point. Enter X = 2 ft and Y = 3 ft, then place the start. Press D again, choose Length / heading and enter 12 ft 6 in at 30 deg. Repeat in Metric with explicit cm or mm entries and with bare numbers.
  - Expected: The starting point and side use the entered coordinates, units and heading. Bare lengths use feet in Imperial and metres in Metric. A placed side remains after finishing, saving and reopening. Input display rounding does not change its stored measurement.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U370 — Draw a measured diagonal and turn relative to it**
  - Steps: Start Measured lines at X = 0, Y = 0. Press D, choose Rise / run and enter rise = 3 ft, run = 4 ft. Press D again, choose Relative turn and enter length = 2 ft and turn = 90 deg. Undo and Redo, then enter a different relative turn after Undo. Also try Relative turn before placing any side.
  - Expected: The first side is 5 ft long and ends at X = 4 ft, Y = 3 ft. The next side turns 90 degrees from that side. Undo/Redo restores the correct pen and previous direction. Relative turn without a previous side explains what is missing and creates nothing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U371 — Draw a curve from its chord and angle**
  - Steps: Start Measured lines at X = 0, Y = 0. Press D and choose Arc chord / angle. Choose Endpoint X/Y in Chord definition, then enter endpoint X = 10 ft, Y = 0 ft and sweep = 90 deg. Add the curve. Repeat with sweep = -90 deg. Undo, Redo, save and reopen.
  - Expected: Both curves end at the entered endpoint and have a quarter-circle sweep in opposite directions. Their measured lengths exceed the 10 ft chord. The curve remains analytical after history navigation and reopening.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U372 — Draw a curve from its chord and height**
  - Steps: Start Measured lines at X = 0, Y = 0. Press D and choose Arc chord / height. Choose Endpoint X/Y in Chord definition, then enter endpoint X = 10 ft, Y = 0 ft and signed chord height = 2 ft. Repeat with height = -2 ft. Inspect the bulge, finish the stroke, save and reopen.
  - Expected: The curves share their chord and bow to opposite sides. The midpoint's distance from the chord is 2 ft. The saved curves retain their shape and measurements.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U373 — Draw a curve from its chord and arc length**
  - Steps: Start Measured lines at X = 0, Y = 0. Press D and choose Arc chord / length. Choose Endpoint X/Y in Chord definition, then enter endpoint X = 10 ft, Y = 0 ft and arc length = 12 ft. Add it, then repeat with Clockwise enabled. Try an arc length of 9 ft before correcting it to 12 ft.
  - Expected: Each valid curve ends at the entered point and measures 12 ft along the curve. Clockwise changes its direction. The impossible 9 ft arc is rejected inline without placing anything; correcting it allows placement.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U374 — Draw a tangent-defined curve and continue from its tangent**
  - Steps: Start Measured lines at X = 0, Y = 0. Press D and choose Arc start tangent / length / sweep. Enter start tangent = 0 deg, arc length = 12 ft and sweep = 90 deg. Add it. Press D, choose Relative turn and enter length = 3 ft, turn = 0 deg. Undo, Redo, save and reopen.
  - Expected: The curve starts horizontally, measures 12 ft and turns through 90 degrees. The next straight side follows the curve's ending tangent, rather than its chord direction. History and reopening preserve both constructions.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U375 — Correct or cancel precise measured-line input**
  - Steps: Open D while a measured stroke is active. Enter an invalid quantity or impossible curve; correct it without closing the dialog. Try Cancel after entering valid values. Switch Imperial/Metric after closing the dialog, reopen D, choose Length / heading and enter a bare length. Finish the stroke and select another layer before starting another one.
  - Expected: Invalid values explain the problem inside the dialog and place nothing. Correct values can be added. Cancel keeps existing geometry and the pen unchanged. Reopened input uses the selected units, and a newly started stroke belongs to the active layer. Previous geometry retains its actual size.
  - Result: Not tested
  - Notes: ______________________________

## Entering a measured chord directly

- [ ] **U376 — Draw a curve using a measured chord and heading**
  - Steps: Choose Measured lines, press D and start at X = 0, Y = 0. Press D, choose Arc chord / angle and keep Length / heading. Enter chord length = 10 ft, heading = 90 deg and sweep = 90 deg. Add it, finish, save and reopen. Repeat with a negative sweep and in Metric using 3 m.
  - Expected: The chord points upward and the curve ends 10 ft (or 3 m) above its starting point. Positive and negative sweeps bow in opposite directions. Reopening preserves the exact size. You do not need to calculate endpoint coordinates.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U377 — Draw a curve using chord height or arc length**
  - Steps: From X = 0, Y = 0, choose Arc chord / height. Enter chord length = 10 ft, heading = 0 deg and height = 2 ft. Repeat with -2 ft. For Arc chord / length enter the same chord and heading with arc length = 12 ft; try Clockwise and an impossible 9 ft arc length.
  - Expected: Height gives a 2 ft bulge on the chosen side of the chord. Arc length gives a true 12 ft curve. Clockwise reverses the bend. An arc shorter than its chord explains the error and places nothing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U378 — Switch between direct chord input and endpoint coordinates**
  - Steps: In a chord curve dialog, choose Endpoint X/Y and enter a known endpoint. Add the curve, then reopen D. Switch to Length / heading, enter a chord in mm or feet/inches and add it. Reopen D again. Change the choice, then Cancel and reopen.
  - Expected: Both definitions work. The last successfully added choice and values are remembered during drawing. Cancel does not change them. Explicit units are respected even when the project uses another unit system.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U379 — Edit and reopen a curve made from a measured chord**
  - Steps: Draw a chord-defined curve and finish it. Move and rotate it. Open its geometry editor and change its physical arc length with the start fixed. Undo and Redo, save/reopen, then export a PDF. Also close a semicircle with a straight side and use Detect closed areas.
  - Expected: Movement and rotation preserve the physical curve length. The length edit uses the requested measurement and history restores each state. Saved and printed curves match the canvas. Detect closed areas measures the curved region rather than the triangle or rectangle around it.
  - Result: Not tested
  - Notes: ______________________________

## Defining nested measured areas

- [ ] **U380 — Keep an inner measured outline as a reference**
  - Steps: Choose Measured lines and draw a closed 10 m by 10 m outline. Draw a closed 4 m by 4 m outline entirely inside it. Select either stroke and use Detect closed areas. Define the outer outline as living; leave the inner outline as Reference only. Apply.
  - Expected: The review shows both outlines and their parent relationship. Only the outer outline becomes an area: gross and net are 100 m². The inner measured lines remain available, with no guessed deduction or room classification.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U381 — Subtract an explicitly classified inner area**
  - Steps: In a measurement project, repeat the 10 m square and inner 4 m square on a new layer. In Detect closed areas, define the outer as living, choose Deduct from parent for the inner, and classify the inner as garage. Inspect the preview, then Apply. To check appraisal totals, switch the calculation workflow to appraisal and record the actual floor and area facts in Details.
  - Expected: The outer shows gross 100 m², deducted 16 m² and net 84 m². The garage remains a separately editable identified area. Qualified appraisal totals count 84 m² of living area and 16 m² of garage once; missing facts leave GLA unavailable rather than guessed.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U382 — Cancel, repeat and undo a nested-area definition**
  - Steps: Open the nested-area review, change choices, then Cancel. Reopen and apply an explicit deduction. Undo and Redo once. Select the source stroke and repeat Detect closed areas; inspect the existing definitions without changing them. Save/reopen and export a PDF.
  - Expected: Cancel changes nothing. One Undo removes the new area definitions and deduction link together while preserving the measured strokes; Redo restores them. Repeat detection creates no duplicates and preserves existing names, styles and facts. Reopened and exported geometry matches the accepted definition.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U383 — Correct an invalid nested-area choice**
  - Steps: In an ordinary Measurement project, open the nested review and try subtracting an inner outline while its parent is Reference only. Try giving parent and child the same classification. Correct the choices. After definition, move the inner source stroke and open Details and Review measured sources. Test ANSI partition authoring separately in U407.
  - Expected: Invalid choices explain the issue and cannot be applied. Correct choices restore a valid preview. Moving the inner source marks its derived area stale and withholds dependent GLA until the source review is accepted; unchanged outer gross geometry is retained.
  - Result: Not tested
  - Notes: ______________________________

## Define First keyboard start

- [ ] **U384 — Start an area at the cursor with Enter**
  - Steps: Press Ctrl+Shift+D and choose a classification. Move the cursor to an empty starting point and press Enter. Type `12 ft` and Right; move the pointer outside the side and press Enter to place its dimension. Repeat with `8 ft` and Up, then `12 ft` and Left. Press Enter to close, position the last dimension and press Enter again. Undo, Redo and save/reopen. Repeat the starting step with Snap off.
  - Expected: The first Enter fixes the starting point without creating a side. Snap on uses the snapped cursor; Snap off uses the exact cursor. Enter still places each side's dimension separately. The completed rectangle measures 96 square feet and commits as one undoable operation. Holding Enter or panning cannot accidentally accept a starting point. Saved geometry and dimensions reopen unchanged.
  - Result: Not tested
  - Notes: ______________________________

## Define First dimension labels

- [ ] **U385 — Orient a dimension horizontally or vertically**
  - Steps: Start Define First with Ctrl+Shift+D, choose a classification, then anchor with Enter. Type `12 ft` and Right. While its label follows the pointer, press V, then H, then V again. Click or press Enter to place it. Continue and close the area; save/reopen and export a PDF.
  - Expected: V turns the text vertically and H makes it horizontal immediately. Placement keeps the chosen orientation. The side remains 12 feet and reopened/printed text has the same orientation.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U386 — Omit a dimension label without losing its side**
  - Steps: In Define First, enter a side and tap Space while its dimension is pending. Undo and Redo. Finish a 12 ft by 8 ft rectangle, omitting one label. Save/reopen and export it.
  - Expected: Space hides only that side's dimension text and lets you continue drawing. Undo makes the label pending and visible again; Redo hides it. The completed rectangle remains 96 square feet. Reopened and exported drawings preserve the omitted label.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U387 — Pan while a dimension is waiting for placement**
  - Steps: With a Define First dimension pending, hold Space, left-drag the canvas, release the mouse, then release Space. Repeat with right-drag and middle-drag. Move the pointer and press Enter to place the label.
  - Expected: The canvas moves and the label remains pending. Releasing Space after a pointer gesture cannot omit it or place a new node. Enter places it at the chosen position after navigation.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U388 — Reopen an undone vertical dimension placement**
  - Steps: Place a pending dimension with V, then Undo once. Save the unfinished drawing and reopen it. Redo the placement, Undo again, tap Space to omit it, then Undo and save/reopen again.
  - Expected: The reopened pending label is visible and vertical. Redo restores the recorded placement. Undo of omission returns a visible vertical pending label; reopening retains that orientation. No side or measurement changes.
  - Result: Not tested
  - Notes: ______________________________

## Drawing feedback and image output

- [ ] **U389 — Read practical drawing dimensions**
  - Steps: With Snap off, draw a diagonal wall or measured line. Switch Imperial/Metric, inspect its length, and select and resize a component to inspect its width and depth. Also draw a precisely entered 12-foot wall.
  - Expected: Ordinary labels use millimetres or feet and fractional inches. A rounded display has an approximation mark (`≈`); a common exact measurement stays uncluttered. Entered measurements and the saved geometry are preserved.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U390 — Navigate an unfinished drawing with the overview**
  - Steps: On an empty layer, enable Map. Start a wall, measured line or measurement boundary and move the cursor to preview the next side. Click and drag inside the overview, then continue drawing on the main canvas. Cancel the unfinished drawing and repeat on a layer with existing objects.
  - Expected: The overview shows unfinished sides and their live preview. Navigating it pans the view without placing a side or changing the pending drawing. After cancellation, its unfinished lines disappear from the overview.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U391 — Export a 3D image into a long or accented folder path**
  - Steps: Open a residential or light-commercial model, select an object, and export a native 3D PNG into nested folders with a long name and an accented character. Reopen the PNG. Repeat export into your normal folder and try a destination you cannot write to.
  - Expected: Valid destinations produce the same model view with the normal output footer. Selection controls stay available in Vertex and do not appear in the export. A failed export reports the problem and preserves any existing destination file.
  - Result: Not tested
  - Notes: ______________________________

## Project folders

- [ ] **U392 — Save and reopen a project in deeply nested folders**
  - Steps: Choose an existing nested folder on your local Windows drive, including accented characters in a folder or project name. Save a drawing there, close and reopen it, edit a wall, and Save again. Reopen once more. Repeat with an unfinished drawing saved in the project.
  - Expected: Vertex saves and opens the chosen file without requiring you to shorten its folder path. The wall edit and unfinished drawing persist. Saving an existing file keeps the normal replacement/conflict safeguards and previous-file backup behavior.
  - Result: Not tested
  - Notes: ______________________________

## Sloped-room appraisal calculations

- [ ] **U393 — Check a sloped room's counted area**
  - Steps: In Details → Setup, select the ANSI-oriented V2 rule. Draw the complete room and a separate boundary around the portion below five feet high. In Edit facts, choose Sloped ceiling, link the low-height boundary, enter the observed area at least seven feet high, and confirm that the boundary and observation describe the complete room outside its voids. Select the room in Details and open Full report.
  - Expected: The low region is excluded once. Details and the report show the gross footprint, excluded area, denominator and seven-foot share. The half-height check uses the countable finished room rather than its gross footprint. Missing observations withhold GLA. Final ANSI validation is identified as pending.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U394 — Change an existing sloped-room calculation rule**
  - Steps: Open a project using ANSI-oriented V1. In Setup choose V2 and Cancel; reopen Setup and save V2. Reconfirm the room in Edit facts. Undo and Redo the changes, then save and reopen the project.
  - Expected: Opening or cancelling Setup preserves the recorded V1 rule. Saving V2 preserves observations and withholds unconfirmed sloped totals until you reconfirm them. Undo/Redo restores the corresponding rule and facts. Save/reopen retains the selected rule and calculation basis.
  - Result: Not tested
  - Notes: ______________________________

## Appraisal detail readability and unfinished reports

- [ ] **U395 — Read and copy an area's calculation sources**
  - Steps: Select an appraisal area in Details, scroll to Area dimensions and trace, and read its dimensions, perimeter, deductions and calculation values. Expand Source IDs and fingerprint, select and copy an ID, then collapse the section. Repeat with a sloped room and a narrow left panel.
  - Expected: Values stay within the panel and dimensions appear before declarations. Full source IDs and the fingerprint remain readable and copy without added characters. Collapsing the section frees the technical detail space without changing calculations.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U396 — Try exporting a report during a measured-line drawing**
  - Steps: Export an appraisal PDF. Start Measured lines, place only the first point and try exporting over that PDF. Cancel and export again. Repeat with one accepted side, try export, continue with another side, then Finish and export.
  - Expected: Active drawings refuse export with a Finish-or-cancel message. The existing PDF and active drawing stay intact. Drawing continues from the same point after refusal; Cancel or Finish restores export.
  - Result: Not tested
  - Notes: ______________________________

## Combine measured regions

- [ ] **U397 — Combine adjoining regions into one area**
  - Steps: Choose Measured lines. Draw a closed rectangle and two crossing dividers so it contains four regions. Open Detect closed areas, select the four rows, click Combine selected and choose Living. Check the preview, cancel once, then repeat and Apply.
  - Expected: Cancel preserves the lines and creates no areas. Apply creates one Living area with the rectangle's exterior; the internal dividers remain drawing lines but are excluded from the area's perimeter. The preview shows one combined net result rather than four copies of the total.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U398 — Reopen and review a combined area**
  - Steps: After combining regions, Undo and Redo, save and reopen, then run Detect closed areas again. Open Details, select the combined area and inspect its dimensions and deductions. If using appraisal, record the required property, floor and area facts.
  - Expected: Undo removes and Redo restores the whole definition. Save/reopen preserves it. Re-detection keeps the same combined area and does not create duplicate totals. Appraisal GLA includes its eligible contribution once and withholds totals when required facts are missing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U399 — Check invalid combinations and source changes**
  - Steps: Try combining separated regions and regions touching at only one corner. Try combining two already-defined individual areas. For a valid combined area, edit an original divider and inspect its area source status; then copy, paste and move the combined area. Edit one of the pasted copy's source lines and Undo the edit.
  - Expected: Invalid combinations explain the issue without changing the project. Existing definitions are not consumed silently. Changed sources either refresh the complete group together or require explicit repair; stale totals are withheld. The pasted measured copy appears beside the existing drawing and remains selected. Copying and movement preserve all sources, including internal dividers. Editing the copy's source refreshes the copy without changing the original, and Undo restores the whole edit.
  - Result: Not tested
  - Notes: ______________________________

## Measured bay returns and phased adjustments

- [ ] **U400 — Complete a bay return with Measured lines**
  - Steps: Choose Measured lines. Draw a diagonal bay side followed by its straight front. Press B, then continue drawing. Undo and Redo the return. Finish, save and reopen.
  - Expected: B adds the matching diagonal return at the current pen. Earlier measurements remain unchanged. Undo removes only the return; Redo restores it. Reopening retains the lines. A curved front or disconnected stroke explains why B cannot complete the return without changing the drawing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U401 — Adjust areas within the current design phase**
  - Steps: Create an area and a smaller area of a different type inside it. Put a potential parent in another design alternative, then open Auto-Subtract. Switch to the alternative containing that parent and try again. Hide its layer using the eye control and inspect the choices again. Cancel once, then add the adjustment and Undo/Redo it.
  - Expected: Add choices include only areas in the current design phase; demolished and inactive alternative areas cannot become new deduction links. Hiding a layer does not change calculation eligibility. Cancel changes nothing, and Undo/Redo restores the complete adjustment.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U402 — Remove an adjustment whose design phase changed**
  - Steps: Add a valid area adjustment. Change the design phase so its parent or subtracting area is no longer active. Select the subtracting area, open Auto-Subtract and remove its existing link. Undo and Redo the removal.
  - Expected: The existing link remains available for removal, even when it cannot be added under the current phase. Removal repairs the relationship without moving geometry. Undo/Redo preserves the original link and its removal.
  - Result: Not tested
  - Notes: ______________________________

## Curved survey parcels

- [ ] **U403 — Draw a parcel with a curved boundary**
  - Steps: Open Tools → Survey traverse. Enter `NE,0,10 m`, `CURVE,NE,90,20 m,-180`, `SE,0,10 m`, and `SW,90,20 m` on separate lines. Calculate and add the boundary.
  - Expected: The preview and placed parcel have a curved top. Area is about 357.0796 m² and acreage about 0.088236. Undo removes the parcel and Redo restores it. Save/reopen retains the actual curve.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U404 — Enter a curve by height or arc length**
  - Steps: Repeat U403 using `ARC_HEIGHT,NE,90,20 m,-10 m` for the curved call. Repeat with `ARC_LENGTH,NE,90,20 m,31.415926536 m,CW`. Export and reopen each report.
  - Expected: Both constructions produce the same semicircular top within the entered precision. The report restores your exact inputs. Invalid directions, zero height or an arc length shorter than its chord explain the problem and create nothing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U405 — Correct an existing curved survey**
  - Steps: Select the U403 parcel and reopen Survey traverse. Change the curved chord and final westward distance from 20 m to 40 m. Calculate and Update boundary. Undo/Redo, save/reopen and open its calls again.
  - Expected: The same parcel becomes wider with a larger curve. Layer, classification and original source remain retained. Undo restores the earlier curve and Redo restores the correction. Reopening restores the corrected calls.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U406 — Review survey closure without changing measurements silently**
  - Steps: Enter `SE,0,10 m`, `SW,90,20 m`, `NE,0,10 m`, and `CURVE,NE,90,20.0005 m,-180`. Set closure tolerance to `0.01 m`, Calculate and inspect the proposed closure before adding. Try an open traverse and an inward curve that crosses another edge.
  - Expected: The residual is disclosed. A curved final call cannot have its endpoint adjusted; adding retains that curve and uses a separate closing line. Open or intersecting calls cannot become area boundaries. A residual too small for a valid closing segment produces an explanation instead of silently moving the endpoint.
  - Result: Not tested
  - Notes: ______________________________

## Nested appraisal rooms and exclusions

- [ ] **U407 — Define a room and its internal void from measured outlines**
  - Steps: Use a test project. In Details > Setup choose ANSI Z765-2021 for a single-family exterior measurement. With Measured lines, draw three contained closed outlines: outer 10 m × 10 m, room 6 m × 6 m, and void 2 m × 2 m inside the room. Select a measured stroke and run Detect closed areas. Define the outer as above-grade finished, deduct the room from its parent as above-grade finished, and deduct the void from its parent as other void. Review, cancel once, then apply. In Edit facts supply the test's above-grade, finished, primary-dwelling, direct-access and flat-ceiling observations for the outer and room, plus the required property measurement observations. Inspect Details, Undo/Redo, save/reopen and export the appraisal PDF. Use the room's deduction editor to remove and restore its void link.
  - Expected: Review shows outer gross100/deduct36/net64 m² and room gross36/deduct4/net32 m². One Apply creates both links together. Labels alone do not establish GLA. After the explicit observations qualify, GLA is 96 m², displayed as 1033 whole sq ft under this profile. The real 4 m² void is excluded once. Cancel changes nothing. Fact edits retain both links and source geometry. Undo/Redo, reopening and the PDF agree with Details. Removing/restoring a link is reversible. Ordinary measurement Auto-Subtract retains its existing TYPE and nesting rules.
  - Result: Not tested
  - Notes: ______________________________

## Copy appraisal rooms and exclusions

- [ ] **U408 — Copy a floor with its room and internal void**
  - Steps: Complete U407 in a test project. Select the outer measured area and Clone it with a 20 m horizontal offset. Inspect Details, then Undo and Redo. Undo again, Copy the original and Paste it. Move the pasted copy, edit one of its source lines, Undo, save/reopen and export the appraisal PDF. Try Paste into a separate project using an ANSI appraisal profile. For a sloped room, inspect the copied room's facts, then confirm its actual observations through Details > Edit facts.
  - Expected: Clone and Paste include the room, void, measurement sources, dimensions and appearance. The original remains unchanged. The test's flat-ceiling copy adds another 96 m² of GLA: together 192 m², displayed as 2067 sq ft. Each copy's deductions refer to its own areas. One Undo removes the entire copy and Redo restores it. Editing the copy leaves original source lines unchanged. A copied sloped room withholds GLA until its observations are explicitly confirmed; raw measurements are retained. Nested Paste into an ordinary measurement project explains the unsupported destination without changing it. Reopening and PDF agree with Details.
  - Result: Not tested
  - Notes: ______________________________

## Copy and remove measured lines

- [ ] **U409 — Copy and cut a measured line into another project**
  - Steps: Choose Measured lines, draw two sides with exact length inputs, then finish with Enter. Select the stroke, Copy it, create a new project and Paste. Move or resize the copy and save/reopen. Return to the original, Cut the stroke, then Paste it back. Try the keyboard shortcuts and canvas right-click menu. Undo and Redo each edit.
  - Expected: Copy leaves the original unchanged. An empty project receives the stroke at its measured position; a project with geometry receives a separate copy beside it. Exact measurements and saved style survive. Editing the copy does not change the original. Cut removes the selected stroke, Paste restores an independent editable copy, and each operation is one reversible step. Right-click on empty canvas offers Paste.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U410 — Delete a measured line shared by two appraisal areas**
  - Steps: Draw a rectangular measured outline and a separator that divides it into two rooms. Detect and define both rooms, then record appraisal facts until Details shows qualified totals. Select and Delete the separator. Inspect both rooms and Details, save/reopen, then Undo the deletion.
  - Expected: Delete removes only the selected measured stroke. Both room boundaries and their facts remain; their missing measurement source is identified and qualified GLA is withheld. Unrelated lines stay unchanged. Reopening preserves the missing-source state. Undo restores the exact source and previous qualification.
  - Result: Not tested
  - Notes: ______________________________

## Constraints on measured lines

- [ ] **U411 — Lock the length of an open measured line**
  - Steps: Draw an open measured stroke with two edges. Select it and open Dimensions and constraints. Add a fixed length to its last edge using an expression such as 10 ft. Preview, Cancel once, then Apply. Move its free end compatibly, then try an endpoint move or direct resize incompatible with the retained lock. Separately edit the relation's target length. Undo, Redo, save and reopen.
  - Expected: The last endpoint is available. Preview shows proposed changes without modifying the drawing. Apply saves the relation. Compatible edits respect the lock; conflicting edits explain the problem and leave the drawing unchanged. Explicitly changing the relation's target previews and applies the new length. Undo and reopening preserve the entered measurement and relationship.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U412 — Connect measured lines and align them with walls**
  - Steps: Draw two open measured strokes and a physical wall. In Dimensions and constraints, connect one stroke's final endpoint to the other stroke's start using Coincident. Add Horizontal or Vertical and try Parallel and Perpendicular against the wall. Preview each relationship before applying. Move a connected endpoint, Undo, then remove a relationship.
  - Expected: Segment names and both endpoints are selectable. Connected geometry moves together when permitted. Merely overlapping endpoints does not establish a relationship. Conflicting locks cannot apply; removing a relationship is reversible.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U413 — Keep a curved measured line's physical length while reflecting it**
  - Steps: Draw a curved measured stroke. Open Dimensions and constraints and compare Fixed length with Fixed physical arc length. Apply the physical arc-length lock. Reflect the stroke, rotate it, move it, then Undo, Redo and save/reopen. Copy and Paste the edited stroke and edit its copy.
  - Expected: Chord distance and analytical physical curve length are distinguished. The reflected curve keeps its physical length and reverses its curvature correctly. Rotation and movement preserve the saved relationship. The copy has independent internal references and remains editable without changing the original.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U414 — Update appraisal areas when their connected measured lines change**
  - Steps: Draw and define two adjoining measured rooms, then record the required appraisal facts until Details shows qualified GLA. Add a relationship to the dividing line and move a connected endpoint. Inspect the geometry, room dimensions, deductions and GLA before accepting. Undo, Redo and save/reopen. Repeat with a combined measured area and an active remodeling alternative.
  - Expected: Previously current, unambiguous, unauthored areas and measurements update in the same reversible edit, including combined-area member lineage. Recorded classifications and observations are retained. An ambiguous, authored or already-stale area requires source review and withholds qualified totals. The active design phase governs sources; hiding a layer alone does not change GLA.
  - Result: Not tested
  - Notes: ______________________________

## Saved dimensions on measured lines

- [ ] **U415 — Add saved dimensions to an open or curved measured line**
  - Steps: Choose Measured lines and draw an open stroke, then a curved stroke. Select each and open Tools > dimension creator, or the canvas right-click dimension action. Add a length to the final edge. Choose Angle, two connected edges and their shared point, then add the angle. Try a closed measured stroke as well.
  - Expected: The selected stroke is the default source. Its final edge can receive a saved length. A curve shows physical arc length; the angle uses its endpoint tangent. The angle point list contains genuine shared points of the chosen edges. Identical edges cannot produce an angle. Area is unavailable for measured strokes, including closed ones; define a measured area first.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U416 — Style a saved line dimension and edit its source**
  - Steps: Select a saved length or angle dimension. Change its color, paper text size and position. Move a source vertex, resize an edge and edit a connected relationship. Undo, Redo, save and reopen. Add an automatically positioned length in each workspace at different zoom levels.
  - Expected: The displayed value follows the actual source geometry. Ordinary geometry edits retain the saved position, style and target edge or point. Automatic placement starts beside the chosen line at a readable distance using the current workspace zoom. Undo and reopening retain the complete edit.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U417 — Move and duplicate a line with saved dimensions**
  - Steps: Select a dimensioned stroke and open Transform selection. Preview a rotation, reflection and offset; Cancel once, then Apply. Drag it together with an ordinary area and with a connected wall. Create a transformed copy, then Copy/Paste into another project. Edit the copy.
  - Expected: Preview changes no saved objects. Apply and dragging move the source and its dimension positions together once. Valid connected moves retain their relationships. Copies contain independent dimension and source references; editing one leaves the original unchanged. Each committed change is one Undo/Redo step.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U418 — Reopen, remove and export dimensioned measured lines**
  - Steps: Save/reopen a stroke with styled length and angle dimensions. Delete its owner, Undo and Redo, then save/reopen again. Restore it and inspect sheet preview, SVG and PDF at two output scales.
  - Expected: Delete removes the stroke and attached dimensions together. Undo restores their exact saved values, positions and styles, including after reopening. Output contains the saved dimensions and styles at the chosen sheet scale.
  - Result: Not tested
  - Notes: ______________________________

## Insert points in walls

- [ ] **U419 — Split a wall with Insert point**
  - Steps: Draw and select a wall. Choose Tools > Insert point. Change the position from 0.5 to 0.4, inspect the two wall lengths and seam, and Cancel. Reopen and Apply. Undo and Redo.
  - Expected: Preview and Cancel leave the drawing unchanged. Apply creates two connected walls with the original thickness and layer, in one reversible edit. Invalid or endpoint positions cannot apply.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U420 — Split a wall containing doors or windows**
  - Steps: Place doors or windows before and after the proposed split. Insert the point between them, then save/reopen and Undo. Try inserting through a door or window.
  - Expected: Valid insertion leaves openings in the same physical positions and retains their hosts. An insertion crossing an opening identifies the conflict and changes nothing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U421 — Keep appraisal measurements when splitting an exterior wall**
  - Steps: Draw a closed exterior wall loop, define its measured area and record its appraisal facts. Note the area, perimeter and Details totals. Insert a point in one exterior wall. Inspect the outline, dimensions and Details, then save/reopen and Undo.
  - Expected: The new wall seam does not change the measured outline, area or facts. Source-current measurements remain current. A manually placed full-wall dimension still measures the full original span; automatic piece dimensions reflect each child. Undo restores the original wall and outline together.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U422 — Split a curved wall with a physical-length lock**
  - Steps: Draw a curved wall and add Fixed physical arc length through Dimensions and constraints. Insert an interior point, then inspect the curve and retained relationship. Split again to make three pieces. Reopen the existing relationship, change its total length, Preview and Cancel once, then Preview and Apply. Try a compatible connected edit, Undo and save/reopen.
  - Expected: Splitting reconstructs the original curve. The saved relationship retains one total physical arc length across all pieces; it does not replace it with separate fixed lengths. Editing that total identifies the complete chain and retains every member. Cancel changes nothing; Apply and Undo change the total and connected geometry together. Conflicts explain the problem and cannot apply.
  - Result: Not tested
  - Notes: ______________________________

## Remove boundary points

- [ ] **U423 — Remove a corner or an inserted point**
  - Steps: Draw and select a measured rectangle. Right-click > Remove point, choose a corner, inspect the replacement and Cancel. Reopen and Apply. Undo, Redo, save and reopen, then Undo again. Repeat with a point inserted on a curved edge. Add manual dimensions before removing a point and review their Keep/Remove choices.
  - Expected: Preview changes nothing. A rectangle becomes the previewed triangle with updated area and perimeter. Removing a split point on the same circle restores one analytical arc. Dimensions on retained edges stay attached; references to retired edges or corners need your explicit decision. Apply is one reversible edit. Invalid or stale edits explain the conflict and change nothing. Physical-wall-derived outlines direct you to edit their source walls.
  - Result: Not tested
  - Notes: ______________________________

## Related geometry and visible GLA

- [ ] **U424 — Move a related room and its driving area or wall together**
  - Steps: Declare that a room follows a measured area or wall in Room relationships. Open Preview propagation, choose that driver and enter an offset or rotation. Cancel once, then reopen and Apply. Inspect both objects and their dimensions, Undo/Redo, save and reopen. Repeat with an Independent relation.
  - Expected: Preview shows the exact driver and dependent positions. Apply moves both in one reversible edit, retaining wall-hosted doors/windows and attached measurements. Independent geometry stays unchanged; its chosen driver can still move. Contradictory or stale edits explain the problem and cannot apply.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U425 — Find GLA and its calculation basis**
  - Steps: Open the left Details tab, use Setup to choose the appraisal profile, and record the required property/floor/area facts. Inspect the GLA total and profile, choose an area to see edge lengths, gross area, deductions and net area, then open Full report.
  - Expected: GLA is visible without selecting a canvas object. The ANSI profile, when selected, appears beneath qualified totals with its pending standards-validation status. Missing facts withhold the total and explain what to correct. Floor/area contributions and Full report agree; the app does not label unverified results ANSI-approved.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U426 — Keep a related room attached to the whole wall after inserting points**
  - Steps: Draw a wall, place a door or window away from the planned split points, and declare that a room follows that wall in Room relationships. Select the wall and use Insert point. Insert another point in its second piece. Use Sync references, then Preview propagation to move the original whole-wall reference. Cancel once, reopen and Apply. Undo/Redo, save and reopen. Repeat with a curved wall.
  - Expected: The reference still represents the entire wall, with no separate child entries in the relationship pickers. Every piece moves together with the room in one reversible edit. Hosted components remain attached at their local positions. Cancel changes nothing; save/reopen retains the whole-wall relationship and Undo history.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U427 — Resize a measured exterior edge and keep its walls and GLA synchronized**
  - Steps: Draw a closed exterior wall loop with one curved wall. Define its measured area and record appraisal facts until Details shows GLA. Select the measured area and open Edit boundary geometry. Choose the curved edge, enter an explicit length, choose which endpoint stays fixed, and preview. Try the connected boundary-chain option on and off. Cancel once, reopen and Apply. Inspect the physical walls, edge dimensions and Details; Undo/Redo, save and reopen. Repeat with a straight edge and with a physical partition attached to a moved source wall; try disabling movement of related objects.
  - Expected: Preview shows the wall and measured-area changes together. Apply produces the entered measured-edge length while retaining the chosen endpoint and curve direction. Current dependent measured areas and GLA recalculate in one reversible edit; Cancel changes nothing. Source walls must follow their measured exterior even when movement of other related objects is disabled. An impossible locked measurement or frozen attached partition explains the conflict and cannot apply. Reopening retains the entered measurement and exact Undo history.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U428 — Change an exterior curve without losing its walls or GLA**
  - Steps: Draw a closed wall outline and define its measured exterior. Record the appraisal facts in Details. Select the measured area, open Edit boundary geometry and choose a straight edge. Try Signed sweep angle, Signed arc height and Arc length; preview each, Cancel once and then Apply. Repeat on an existing curved edge and on the opposite side of the chord. Inspect the physical walls, dimensions and GLA. Undo/Redo, save and reopen. Try a partition attached to the edited wall with Move related objects enabled and disabled.
  - Expected: Both measured chord endpoints stay fixed. Preview shows the proposed curve and physical-wall changes together. The area remains linked to its source walls, and dimensions and GLA update in the same undoable edit. The entered construction survives reopening. Frozen or locked geometry that cannot meet the proposal explains the conflict and cannot apply. Cancel changes nothing.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U429 — Drag a saved measurement label**
  - Steps: Add a length, angle or area dimension. Select its label and drag it to a clearer position. Try dragging and pressing Escape, then Undo/Redo the completed move. Save and reopen.
  - Expected: The selected label and its guide update during dragging. Only the annotation moves; the measured shape, wall length, angle and area value stay unchanged. Escape discards the move. Undo/Redo and reopening preserve the placement and styling.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U430 — Move a measured shape with its saved label**
  - Steps: Select a measured shape and Ctrl-select its saved dimension. Drag the selection. Repeat by selecting every physical wall of a closed exterior and its saved dimension, without selecting the measured area itself.
  - Expected: The shape, supporting walls and attached label move together. The label shifts once, retains its automatic/manual placement setting and keeps the same measurement. Undo restores the entire move together.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U431 — Drag one wall with its saved measurement label**
  - Steps: Draw a closed wall outline and define its measured exterior with saved dimensions. Select one wall, then Ctrl-click one of the measured exterior's dimension labels. Start a drag from inside the selected bounds. Inspect the wall, connected corners, label, dimensions and area. Press Escape during a second drag. Undo and Redo the completed drag, then save and reopen. Also try in a saved plan view whose orientation is rotated from the default Top view; drag right and up on screen.
  - Expected: The wall and following corners move together, and the selected label shifts once with the wall. Dimensions and area update to match the moved geometry. In the rotated saved view, the wall and label follow the pointer as shown in that view and retain their intended relationship. Escape cancels the second drag. Undo/Redo and reopening preserve the completed move.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U432 — Drag a wall, ordinary text label and measurement label together**
  - Steps: Draw a wall without an associated measured exterior, create an independent measured shape with a saved dimension, and add an ordinary text label. Select the wall, then Ctrl-click both labels. Drag from inside the selected bounds. Inspect the wall, both labels and the independent measured shape. Undo and Redo, then save and reopen.
  - Expected: The wall and both selected labels move together. The dimension label shifts once while its independent measured shape and measured value stay unchanged. Undo/Redo and reopening preserve the group move.
  - Result: Not tested
  - Notes: ______________________________

## Rooms measured from physical walls

- [ ] **U433 — Create and classify a room inside drawn walls**
  - Steps: Draw four connected walls to make a closed room. Select one wall and use **Create room boundary from selected geometry** in Commands. Open **Library > Area classes**, choose a classification and apply it to the detected space or its row. Double-click the room to open floating **Properties** and inspect **Area calculation**. The left **Details** tab reports property GLA; creating or classifying this physical room does not qualify or add it to GLA.
  - Expected: A named room follows the clear inside faces of the walls. Its area excludes wall thickness. Applying a classification changes the room's classification without moving walls. Repeating room creation does not create a duplicate current room.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U434 — Exclude an interior wall from clear room area**
  - Steps: In a metric test project, draw a 4 m by 3 m rectangle using wall baselines and 0.2 m wall thickness. Add an isolated 2 m long, 0.2 m thick wall entirely inside. Detect and create the room, then double-click it to open floating **Properties** and inspect **Area calculation**. Read property GLA separately in the left **Details** tab; this clear room area is not an additional GLA contribution.
  - Expected: Outer clear area is 10.64 m², the interior wall deducts 0.40 m², and net clear room area is 10.24 m². The room fill excludes the interior wall footprint.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U435 — Keep room classification separate from appraisal GLA**
  - Steps: In a test project with an exterior measurement and its required appraisal facts already entered, record the GLA total. Create and classify a physical room inside the walls. Change the room's classification and inspect GLA again.
  - Expected: Room creation and classification do not add the room area a second time to exterior GLA or invent eligibility facts. Details distinguishes room clear area from the appraisal total.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U436 — Select an outlined room without blocking its furniture**
  - Steps: Create an outlined physical room with an interior wall obstacle. Place a sofa inside. Click an empty part of the clear room interior, then click the sofa. Deselect, then click inside the obstacle's footprint away from an actual wall line.
  - Expected: The room can be selected from its clear interior without precisely clicking an outline. Clicking the sofa selects the sofa. The excluded hole does not count as room material.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U437 — See when a physical room's source has changed**
  - Steps: Create a physical room, record its area, then change a source wall's thickness. Select the retained room in Layers, open Details, and inspect its canvas label and schedule. Undo the wall change.
  - Expected: The changed room is marked stale; its previous numeric area is withheld from the canvas, Details and schedule. Undo restores the current room and its original area. Use U439 to test explicit source repair.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U438 — Save and reopen a classified room with a hole**
  - Steps: Create and name a physical room containing an isolated wall obstacle. Choose its classification, save, close the project, then reopen it. Select the room and inspect its name, classification, fill and Details.
  - Expected: The same room, hole, classification and clear area return. The hole remains excluded from room fill and quantities.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U439 — Repair a room after changing wall thickness**
  - Steps: Start with the metric room and interior obstacle in U434. Change the bottom exterior wall thickness from 0.2 m to 0.4 m. Select the stale room in Layers and use **Tools > Repair room from walls**. Try clicking inside the obstacle, then inside clear room space. Cancel once; reopen the review, choose the clear space and use **Review and apply**. Undo/Redo, save and reopen.
  - Expected: The obstacle cannot be chosen. Cancel changes nothing. Apply retains the room's name and classification, rederives its hole and reports 9.86 m² clear area. Undo restores the prior stale room; Redo restores the repaired one. Reopening retains the repaired room and history. Independent exterior appraisal GLA stays unchanged.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U440 — Choose which split room keeps its name**
  - Steps: Create and name one physical room. Add a partition across it. Select the stale room in Layers, open **Repair room from walls**, and click the intended side. Cancel once, then apply. Inspect both sides and Undo.
  - Expected: Only the explicitly chosen space keeps the old room's name and classification. The other space is available to classify separately. The application does not guess which side owns the old facts. Cancel changes nothing; Undo restores the retained stale room.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U441 — Apply each named drawing type**
  - Steps: Create a new measurement project and draw a closed area. Open **Library > Area classes**. Search and apply First, Second, Third and Fourth Floor; Gross Building Area; both Basement types; Garage; Detached Garage; ADU; Outbuilding; Carport; Porch; Patio; Wood Deck; Balcony; Storage; Low Ceiling; Open to Below; Non-Calculated Area; Subject Site; and Clear. Try Undo after each.
  - Expected: Every named choice is available. Classification changes without moving the outline or changing its actual layer/floor assignment. Clear removes the classification. Undo restores the previous choice. Generic measurement totals follow the configured rules; these names do not establish appraisal eligibility.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U442 — Add the drawing types to an older project**
  - Steps: Open an older measurement project whose Area classes list lacks some named types. Record a custom classification rule in **Calculation profile**. In **Library > Area classes**, click **Add types**, inspect the list and your existing rule, then Undo.
  - Expected: Missing types appear. Existing rules are preserved. The button disappears when no types are missing. One Undo restores the previous profile and list.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U443 — Keep an appraisal drawing type separate from GLA facts**
  - Steps: Open an appraisal project and record its current GLA or missing-facts explanation in Details. Apply a named type such as Finished Basement from **Library > Area classes > Drawing types** to an area. Inspect its row and Details, then apply Undefined / Clear.
  - Expected: The descriptive type appears alongside its qualification. Actual appraisal facts, floor assignment and GLA stay unchanged. A named type alone cannot qualify an area. Clear removes the descriptive type while retaining its appraisal facts.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U444 - Separate an area's name and calculated value**
  - Steps: Draw and name a closed area. Double-click it to open its properties. Choose **Separate name and value**, then switch the callout selector between Name and Calculation.
  - Expected: Both callouts appear separately. They still belong to the same area; separation does not change its outline, classification or total.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U445 - Move the name without moving the calculated value**
  - Steps: Separate an area's callouts. Choose Name and use **Place label** to move it. Choose Calculation and place it elsewhere. Undo and Redo each move.
  - Expected: Each move affects only the chosen callout. Undo and Redo restore that callout's own position without moving the area or its other callout.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U446 - Give the name and value different appearances**
  - Steps: Choose Name, change its text size, color, alignment and rotation, and apply. Choose Calculation and give it different settings. Save and reopen.
  - Expected: Each callout retains its own settings. Left and right alignment anchor the appropriate text edge; clicking the painted text still selects its area.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U447 - Hide only the calculated value**
  - Steps: Choose Calculation, turn off its visibility and apply. Inspect the area and Details. Turn it back on, then repeat with Name.
  - Expected: Only the chosen callout disappears. The area, its other callout and its calculated total remain available.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U448 - Keep a separated value current after editing**
  - Steps: Record a separated area's value. Change one wall measurement, then Undo and Redo. Save and reopen. For a physical room, also change a source wall's thickness without repairing the retained room.
  - Expected: A current area's value follows its actual geometry and units. A stale physical room does not display its previous value as current. Callout styles and placement survive the edits and reopening.
  - Result: Not tested
  - Notes: ______________________________

- [ ] **U449 - Align reusable text at its insertion point**
  - Steps: Create a text-library entry with Left alignment, place it, then change it to Right alignment. Repeat with multiline text, rotate it, save and reopen.
  - Expected: The insertion point stays fixed while the aligned text extends to the chosen side. Picking follows the painted text, and the saved entry and placed text retain their alignment.
  - Result: Not tested
  - Notes: ______________________________

## Issue report template

- Task ID(s):
- What I did:
- What I expected:
- What actually happened:
- Screenshot or project file, if useful:
- Severity: Cannot continue / Major difficulty / Minor issue
