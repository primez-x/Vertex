# PincSketch 4.3 feature inventory

Reviewed 2026-10-04. This is a user-facing feature inventory of the supplied
package, not a claim that every interaction has passed runtime testing.

Archive SHA-256: `0feaed1b1cd7f472b0ae706c52214792fce383d5f486775e7cc9b6b7f4565395`.
The standalone HTML is embedded byte-for-byte in `PincSketch.exe` at offset
3435040. Its title and saved project version say 4.2; README/QA/package names
say 4.3. Inventory the supplied bytes rather than assuming a version label.
The archive contains the app, installer, standalone HTML, README, hashes and QA
report. No installer or executable was launched during this source review.
The browser tool rejected the local file URL; no indirect browser workaround
was used. Source findings are distinct from actual mouse/render/print evidence.

The supplied QA report itself says 4.3 did not receive a full mouse-level
Chromium rendering pass. Treat its PASS statements as supplied claims.

## User-visible functions

These IDs identify practical operations. Source anchors refer to
`PincSketch_4.3_Standalone.html` from the supplied archive.

### Drawing and precision

Source: exterior/interior tools; snapPoint; commitTentative; jump; keydown (354-451,728-798,878-893).

- **PIN-001**: Draw connected exterior/calculation walls by clicking successive points.
- **PIN-002**: Draw interior walls independently of calculation boundaries.
- **PIN-003**: Preview the next wall before committing it.
- **PIN-004**: Snap to existing corners, straight wall segments and grid positions.
- **PIN-005**: Align the current endpoint with existing drawing geometry.
- **PIN-006**: Hold Shift to constrain a wall to 45-degree directions.
- **PIN-007**: Attach to a straight wall and split its node/edge at the join.
- **PIN-008**: Reuse an already covered exterior-wall path instead of duplicating it.
- **PIN-009**: Detect closed spaces formed by the calculation-wall graph.
- **PIN-010**: Recognize separate spaces sharing one wall.
- **PIN-011**: Retain a space classification after its detected-face key changes.
- **PIN-012**: Enter a distance in decimal feet or feet and inches.
- **PIN-013**: Enter a one-operation addition, subtraction, multiplication or division length expression.
- **PIN-014**: Type distance and use an arrow key to preview a direction.
- **PIN-015**: Compose rise/run arrow legs into one angled segment.
- **PIN-016**: Press Enter to accept the keyboard segment preview.
- **PIN-017**: Use normal arrow increments or finer Shift-arrow increments.
- **PIN-018**: Enter exact wall length, angle and quadrant in the Exact Angle dialog.
- **PIN-019**: Use X/Y to align a tentative endpoint with the point of beginning.
- **PIN-020**: Use Ctrl-arrow to jump along an axis to a nearby calculation corner.
- **PIN-021**: Use J to jump to a hovered existing corner.
- **PIN-022**: Use J plus distance/direction travel when no nearby corner is selected.
- **PIN-023**: Park a jump landing and confirm the new drawing anchor explicitly.
- **PIN-024**: Start an interior wall at an existing wall/corner.
- **PIN-025**: Enter an interior-wall starting offset along an incident straight wall.
- **PIN-026**: Walk connected wall corners with arrows while the pen is up.
- **PIN-027**: Bend a tentative wall using the mouse wheel.
- **PIN-028**: Use Shift-wheel for a smaller curve adjustment.
- **PIN-029**: Drag a curved-wall handle to change its bow.
- **PIN-030**: Auto-close an exterior chain with A or the toolbar command.
- **PIN-031**: Close a chain by returning to its starting point.
- **PIN-032**: Show horizontal/vertical distances back to the point of beginning.
- **PIN-033**: Cancel uncommitted drawing, armed placement or jump state with Escape.
- **PIN-034**: Raise the interior-wall pen without deleting already drawn walls.

### Selection, transformations and navigation

Source: selectPointerDown; continueDrag; fence; layout; wheel; dblclick (609-677,679-680,780-829,869-893).

- **PIN-035**: Select a wall, area, symbol, text, area label or dimension label.
- **PIN-036**: Drag an interior wall, symbol or text label.
- **PIN-037**: Drag an area name independently of its area calculation label.
- **PIN-038**: Drag a wall dimension label without editing its measurement.
- **PIN-039**: Double-click a calculation wall to enter a new length.
- **PIN-040**: Propagate a moved calculation node to other walls sharing that node.
- **PIN-041**: Double-click text to edit its content.
- **PIN-042**: Double-click a symbol to rotate it 90 degrees.
- **PIN-043**: Resize and rotate a symbol with selection handles.
- **PIN-044**: Resize the width of a door/opening.
- **PIN-045**: Rotate a symbol with Ctrl-wheel.
- **PIN-046**: Rotate selected objects in 90-degree steps.
- **PIN-047**: Mirror selected symbols or drawing groups horizontally and vertically.
- **PIN-048**: Nudge selected objects with normal/faster arrow increments.
- **PIN-049**: Fence-select objects intersecting a dragged rectangle.
- **PIN-050**: Select all page content while using Fence.
- **PIN-051**: Rotate or mirror a fence group including linked labels and symbols.
- **PIN-052**: Apply shared dimension presentation to a fence group.
- **PIN-053**: Apply shared wall presentation to a fence group.
- **PIN-054**: Apply shared text presentation to a fence group.
- **PIN-055**: Select and drag a connected floor/outbuilding in Layout mode.
- **PIN-056**: Copy/paste a complete layout group on the same page or another page.
- **PIN-057**: Remap pasted object IDs and attached-wall references.
- **PIN-058**: Pan using middle mouse or the Pan tool.
- **PIN-059**: Zoom about the pointer with the wheel when no tentative curve is active.
- **PIN-060**: Fit/center the drawing content.
- **PIN-061**: Use keyboard shortcuts while leaving form fields free to accept normal typing.

### Area classification and appearance

Source: AREA_CODES; classifyFace; style helpers; properties; area palette (291-315,461,558-572,692-702,831-834,897-900).

- **PIN-062**: Search the area-classification palette by text.
- **PIN-063**: Filter area classes by group.
- **PIN-064**: Drag an area class onto a detected space on the canvas.
- **PIN-065**: Drag an area class onto a detected-space list row.
- **PIN-066**: Arm an area class and click a space to assign it.
- **PIN-067**: Reclassify the selected area from its properties.
- **PIN-068**: Clear an area assignment with Undefined/Clear.
- **PIN-069**: Edit the area name.
- **PIN-070**: Set area fill color and opacity.
- **PIN-071**: Choose no hatch, diagonal, cross, horizontal or dot hatch.
- **PIN-072**: Set area label color.
- **PIN-073**: Set area-name and calculation-label sizes independently.
- **PIN-074**: Show/hide the area name and calculation independently.
- **PIN-075**: Set exterior outline color, width and line type per area.
- **PIN-076**: Link exterior outline color to the area color.
- **PIN-077**: Automatically apply area/classification style to true exterior walls.
- **PIN-078**: Keep walls shared by adjacent detected faces neutral and thinner.
- **PIN-079**: Set wall color, width and solid/dashed/dotted/dash-dot appearance.
- **PIN-080**: Apply wall appearance to all exterior or all interior walls.
- **PIN-081**: Set dimension visibility, size, font and color per wall.
- **PIN-082**: Apply dimension presentation to the whole page.

### Symbols and text

Source: SYMBOL_CATALOG; symbolDefaultSize; renderSymbolBody; text library; properties (317-354,477-580,699-701,833-835,909).

- **PIN-083**: Filter symbols by category and search within the active category.
- **PIN-084**: Find a symbol in Favorites or the deduplicated All Residential category.
- **PIN-085**: Click a symbol tile and then click to place it.
- **PIN-086**: Drag a symbol tile directly onto the canvas.
- **PIN-087**: Place symbols at their named default real-world dimensions.
- **PIN-088**: Edit symbol width and depth numerically.
- **PIN-089**: Edit symbol rotation or use 15/90-degree rotation controls.
- **PIN-090**: Mirror a symbol on either axis.
- **PIN-091**: Snap a door/opening to a wall or detach it.
- **PIN-092**: Adjust a swing-door hinge and opening side.
- **PIN-093**: Move a hosted door/opening and reattach it to a wall.
- **PIN-094**: Search the reusable room/note text library.
- **PIN-095**: Click-arm or drag a library label onto the canvas.
- **PIN-096**: Place custom text.
- **PIN-097**: Save a reusable custom label in the local text library.
- **PIN-098**: Edit text content, size, color and font.
- **PIN-099**: Set text alignment, rotation, bold and italic.

### Pages, references and calculations

Source: page state; layout; composition; compactCalcLines; renderCalcs; page handlers (354,633-645,693,836-839,850,854-855,910-911).

- **PIN-100**: Add a drawing page.
- **PIN-101**: Duplicate the current page including its drawing.
- **PIN-102**: Rename a page from its action or by double-clicking its list row.
- **PIN-103**: Delete a page while retaining at least one page.
- **PIN-104**: Navigate pages with the list or previous/next controls.
- **PIN-105**: Ghost the previous page behind the current one.
- **PIN-106**: Show/hide a portrait report composition guide.
- **PIN-107**: Load a raster image behind the drawing for tracing.
- **PIN-108**: Set reference-image width and opacity.
- **PIN-109**: Clear a reference image.
- **PIN-110**: Keep a tracing reference out of appraisal print/PDF output.
- **PIN-111**: See a live GLA total while drawing.
- **PIN-112**: See a list of detected spaces with name, code, area and perimeter.
- **PIN-113**: See whether each classified space contributes to GLA.
- **PIN-114**: See a rectangle multiplication or short rectangle-component sum.
- **PIN-115**: See a strip-method component count and subtotal for complex orthogonal shapes.
- **PIN-116**: See chorded footprint and curve-area adjustment for curved spaces.
- **PIN-117**: See a straight/angled-footprint explanation for other shapes.
- **PIN-118**: Sum unrounded contributing face areas before rounding GLA.
- **PIN-119**: Exclude basement, garage and other non-GLA area types from the GLA sum.

### Files, history and output

Source: history; file pickers; projectJson; legacy convert; print builders (385-391,840-863,904-906).

- **PIN-120**: Start a new sketch.
- **PIN-121**: Save a JSON .pinc project to a selected file.
- **PIN-122**: Save again to the previously chosen file.
- **PIN-123**: Save As under a different file name.
- **PIN-124**: Use browser-download fallback when a file picker is unavailable.
- **PIN-125**: Open an existing .pinc/.json project.
- **PIN-126**: Convert a version-2 PincSketch project into current page data.
- **PIN-127**: Undo and redo drawing/project edits.
- **PIN-128**: Copy, paste and delete selected drawing content.
- **PIN-129**: Print a portrait report using the browser Print/PDF route.
- **PIN-130**: Include drawing, class/code, area, perimeter and GLA in the printed report.
- **PIN-131**: Include compact per-area calculation explanations in the report.
- **PIN-132**: Show page GLA and aggregate project GLA on multipage reports.
- **PIN-133**: Export a tightly cropped, lossless high-resolution raster PDF.
- **PIN-134**: Keep symbol export stroke widths independent of workstation zoom.

## Area types

22 entries total: four GLA floor presets, 17 non-GLA presets and one Clear entry.
A selected preset is not independently verified ANSI eligibility.

| Code | Name | Pinc GLA flag |
| --- | --- | --- |
| GLA1 | First Floor | Yes |
| GLA2 | Second Floor | Yes |
| GLA3 | Third Floor | Yes |
| GLA4 | Fourth Floor | Yes |
| GBA | Gross Building Area | No |
| BSMT-F | Finished Basement | No |
| BSMT-U | Unfinished Basement | No |
| GAR | Garage | No |
| DGAR | Detached Garage | No |
| ADU | Accessory Dwelling / ADU | No |
| OUT | Shed / Outbuilding | No |
| CAR | Carport | No |
| PORCH | Porch | No |
| PATIO | Patio | No |
| DECK | Wood Deck | No |
| BALC | Balcony | No |
| STG | Storage | No |
| LOW | Low Ceiling / Non-GLA | No |
| OPEN | Open to Below | No |
| NCA | Non-Calculated Area | No |
| SITE | Subject Site | No |
| UND | Undefined / Clear | No |

## Symbol coverage

80 distinct named kinds, 110 listed category occurrences including 26 Favorites;
84 occurrences excluding Favorites. All Residential deduplicates the non-Favorites
categories. These counts are names, not 80 independent drawing implementations:
the renderer shares keyword-based bodies among several variants.

### Favorites

- Bath - Bathtub 5'
- Bath - Shower Glass
- Bath - Toilet
- Bath - Vanity Single
- Bath - Vanity Double
- Kitchen - Refrigerator
- Kitchen - Range
- Kitchen - Sink Double
- Kitchen - Dishwasher
- Kitchen - Cabinet Run
- Door - Interior
- Door - Exterior
- Door - Sliding
- Cased Opening
- Fireplace - Standard
- Wood Stove
- Stairs Up
- Stairs U
- Mechanical - FWA
- Mechanical - HWB
- Mechanical - Water Heater
- Pool - Oval
- Pool - Kidney
- Pool - Freeform
- Arrow - North
- Arrow - Plain

### Doors / Openings

- Door - Interior
- Door - Exterior
- Door - French Double
- Door - Sliding
- Door - Pocket
- Door - Bifold
- Door - Barn
- Door - Garage Single
- Door - Garage Double
- Cased Opening

### Windows

- Window - Standard
- Window - Double
- Window - Triple
- Window - Bay
- Window - Bow
- Window - Corner

### Bath / Plumbing

- Bath - Bathtub 5'
- Bath - Freestanding Tub
- Bath - Corner Tub
- Bath - Tub / Shower
- Bath - Shower Rect
- Bath - Shower Corner
- Bath - Shower Glass
- Bath - Toilet
- Bath - Bidet
- Bath - Vanity Single
- Bath - Vanity Double
- Bath - Pedestal Sink
- Bath - Linen Cabinet

### Kitchen

- Kitchen - Refrigerator
- Kitchen - Range
- Kitchen - Cooktop
- Kitchen - Wall Oven
- Kitchen - Microwave
- Kitchen - Dishwasher
- Kitchen - Sink Single
- Kitchen - Sink Double
- Kitchen - Sink Base
- Kitchen - Cabinet Run
- Kitchen - Corner Cabinet
- Kitchen - Island
- Kitchen - Island Seating
- Kitchen - Peninsula
- Kitchen - Pantry

### Laundry / Mechanical

- Laundry - Washer
- Laundry - Dryer
- Laundry - Washer Dryer Stack
- Laundry - Utility Sink
- Mechanical - Water Heater
- Mechanical - FWA
- Mechanical - HWB
- Mechanical - Furnace
- Mechanical - HVAC
- Mechanical - Electrical Panel
- Mechanical - Floor Drain
- Mechanical - Sump

### Fire / Heat

- Fireplace - Standard
- Fireplace - Corner
- Fireplace - Double Sided
- Wood Stove
- Mechanical - FWA
- Mechanical - HWB
- Mechanical - Furnace
- Mechanical - HVAC

### Structure / Detail

- Stairs Up
- Stairs Down
- Stairs L
- Stairs U
- Stairs Spiral
- Railing
- Column
- Open Below
- Closet - Rod Shelf
- Closet - Walk In
- Arrow - North
- Arrow - Plain
- Misc - Structural Issue

### Site / Exterior

- Pool - Rectangle
- Pool - Oval
- Pool - Kidney
- Pool - Freeform
- Hot Tub
- Outdoor - Grill
- Outdoor - Fire Pit

## Built-in text labels

30 built-in labels plus up to 200 locally saved custom labels.

Atrium, Bath, Bed, Breakfast, Carport, Den, Dining, Dining Room, Entrance, Family, Foyer, Garage, Half Bath, Kitchen, Laundry, Living Room, Master, Office, Open to Below, Pantry, Patio, Porch, Primary Bath, Primary Bedroom, Rec Room, Storage, Suite, Utilities, Walk-In Closet, Wood Deck.

## Boundaries of the supplied implementation

- GLA is driven by category flags. It does not independently establish observed
  grade, access, ceiling, dwelling identity or final ANSI conformity.
- Print-Ready PDF is a lossless raster image path. Browser Print/PDF builds SVG
  markup, but its final vector quality was not observed in this review.
- The composition guide explicitly says it is not an architectural print scale.
- No explicit right-click application action or durable autosave journal was
  found in the inspected handlers and persistence paths.
- External calculation-wall drag is limited compared with interior-wall drag;
  no general wall-node editing surface was found.
- Supplied source has no demonstrated Apex native-file, device, architectural
  3D, constraint-proof or crash-recovery parity. Those remain Vertex requirements.

Total practical operations inventoried above: 134. Symbol/type/name inventories are additional coverage.
