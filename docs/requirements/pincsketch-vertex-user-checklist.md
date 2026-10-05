# PincSketch comparison: Vertex user checklist

Use a copy of a project. Each checkbox is one action to try in Vertex. Record
Pass/Fail and your observations alongside it; unchecked means Not tested.
Some additions are still missing: the [comparison](pincsketch-4.3-vertex-comparison.md)
identifies those gaps. Do not count a missing control as a passing test.

These 134 IDs match the [source inventory](pincsketch-4.3-feature-inventory.md).
Equivalent Vertex controls are acceptable; its unified canvas gestures and
double-click quick properties remain the requested behavior. Browser-only file
fallbacks are satisfied by dependable native file dialogs. Export quality must
be at least equivalent; a vector PDF is acceptable in place of a raster PDF.

## Before you start

- Work in a saved test project with two outlines, an interior wall, a door, a symbol and text.
- For appraisal checks, enter the actual required area and floor facts in Details.
- After each edit, try Undo/Redo; save and reopen any result that looks wrong.
- Compare symbol names and area types against the inventory; a larger count alone does not pass.

## Drawing and precision

- [ ] **PIN-001** — Draw connected exterior/calculation walls by clicking successive points.
- [ ] **PIN-002** — Draw interior walls independently of calculation boundaries.
- [ ] **PIN-003** — Preview the next wall before committing it.
- [ ] **PIN-004** — Snap to existing corners, straight wall segments and grid positions.
- [ ] **PIN-005** — Align the current endpoint with existing drawing geometry.
- [ ] **PIN-006** — Hold Shift to constrain a wall to 45-degree directions.
- [ ] **PIN-007** — Attach to a straight wall and split its node/edge at the join.
- [ ] **PIN-008** — Reuse an already covered exterior-wall path instead of duplicating it.
- [ ] **PIN-009** — Detect closed spaces formed by the calculation-wall graph.
- [ ] **PIN-010** — Recognize separate spaces sharing one wall.
- [ ] **PIN-011** — Assign a room type, then edit its outline; verify it keeps that type.
- [ ] **PIN-012** — Enter a distance in decimal feet or feet and inches.
- [ ] **PIN-013** — Enter a one-operation addition, subtraction, multiplication or division length expression.
- [ ] **PIN-014** — Type distance and use an arrow key to preview a direction.
- [ ] **PIN-015** — Compose rise/run arrow legs into one angled segment.
- [ ] **PIN-016** — Press Enter to accept the keyboard segment preview.
- [ ] **PIN-017** — Use normal arrow increments or finer Shift-arrow increments.
- [ ] **PIN-018** — Enter exact wall length, angle and quadrant in the Exact Angle dialog.
- [ ] **PIN-019** — Use X/Y to align a tentative endpoint with the point of beginning.
- [ ] **PIN-020** — Use Ctrl-arrow to jump along an axis to a nearby calculation corner.
- [ ] **PIN-021** — Use J to jump to a hovered existing corner.
- [ ] **PIN-022** — Use J plus distance/direction travel when no nearby corner is selected.
- [ ] **PIN-023** — Jump to another corner and confirm the next wall starts there without an unwanted connecting wall.
- [ ] **PIN-024** — Start an interior wall at an existing wall/corner.
- [ ] **PIN-025** — Enter an interior-wall starting offset along an incident straight wall.
- [ ] **PIN-026** — Walk connected wall corners with arrows while the pen is up.
- [ ] **PIN-027** — Bend a tentative wall using the mouse wheel.
- [ ] **PIN-028** — Use Shift-wheel for a smaller curve adjustment.
- [ ] **PIN-029** — Drag a curved-wall handle to change its bow.
- [ ] **PIN-030** — Auto-close an exterior chain with A or the toolbar command.
- [ ] **PIN-031** — Close a chain by returning to its starting point.
- [ ] **PIN-032** — Show horizontal/vertical distances back to the point of beginning.
- [ ] **PIN-033** — Cancel uncommitted drawing, armed placement or jump state with Escape.
- [ ] **PIN-034** — Raise the interior-wall pen without deleting already drawn walls.

## Selection, transformations and navigation

- [ ] **PIN-035** — Select a wall, area, symbol, text, area label or dimension label.
- [ ] **PIN-036** — Drag an interior wall, symbol or text label.
- [ ] **PIN-037** — Drag an area name independently of its area calculation label.
- [ ] **PIN-038** — Drag a wall dimension label without editing its measurement.
- [ ] **PIN-039** — Double-click a calculation wall to enter a new length.
- [ ] **PIN-040** — Change a corner shared by two walls; verify both walls still meet at that corner.
- [ ] **PIN-041** — Double-click text to edit its content.
- [ ] **PIN-042** — Double-click a symbol to open quick properties; rotate it by 90 degrees using its controls.
- [ ] **PIN-043** — Resize and rotate a symbol with selection handles.
- [ ] **PIN-044** — Resize the width of a door/opening.
- [ ] **PIN-045** — Rotate a symbol with Ctrl-wheel.
- [ ] **PIN-046** — Rotate selected objects in 90-degree steps.
- [ ] **PIN-047** — Mirror selected symbols or drawing groups horizontally and vertically.
- [ ] **PIN-048** — Nudge selected objects with normal/faster arrow increments.
- [ ] **PIN-049** — Fence-select objects intersecting a dragged rectangle.
- [ ] **PIN-050** — Select all page content while using Fence.
- [ ] **PIN-051** — Rotate or mirror a fence group including linked labels and symbols.
- [ ] **PIN-052** — Apply shared dimension presentation to a fence group.
- [ ] **PIN-053** — Apply shared wall presentation to a fence group.
- [ ] **PIN-054** — Apply shared text presentation to a fence group.
- [ ] **PIN-055** — Select and drag a connected floor/outbuilding in Layout mode.
- [ ] **PIN-056** — Copy/paste a complete layout group on the same page or another page.
- [ ] **PIN-057** — Paste a drawing group twice; move each copy and its hosted doors independently of the original.
- [ ] **PIN-058** — Pan using middle mouse or the Pan tool.
- [ ] **PIN-059** — Zoom about the pointer with the wheel when no tentative curve is active.
- [ ] **PIN-060** — Fit/center the drawing content.
- [ ] **PIN-061** — Type in a length, name or search field; verify drawing shortcuts do not consume the text.

## Area classification and appearance

- [ ] **PIN-062** — Search the area-classification palette by text.
- [ ] **PIN-063** — Filter area classes by group.
- [ ] **PIN-064** — Drag an area class onto a detected space on the canvas.
- [ ] **PIN-065** — Drag an area class onto a detected-space list row.
- [ ] **PIN-066** — Arm an area class and click a space to assign it.
- [ ] **PIN-067** — Reclassify the selected area from its properties.
- [ ] **PIN-068** — Clear an area assignment with Undefined/Clear.
- [ ] **PIN-069** — Edit the area name.
- [ ] **PIN-070** — Set area fill color and opacity.
- [ ] **PIN-071** — Choose no hatch, diagonal, cross, horizontal or dot hatch.
- [ ] **PIN-072** — Set area label color.
- [ ] **PIN-073** — Set area-name and calculation-label sizes independently.
- [ ] **PIN-074** — Show/hide the area name and calculation independently.
- [ ] **PIN-075** — Set exterior outline color, width and line type per area.
- [ ] **PIN-076** — Link exterior outline color to the area color.
- [ ] **PIN-077** — Automatically apply area/classification style to true exterior walls.
- [ ] **PIN-078** — Keep walls shared by adjacent detected faces neutral and thinner.
- [ ] **PIN-079** — Set wall color, width and solid/dashed/dotted/dash-dot appearance.
- [ ] **PIN-080** — Apply wall appearance to all exterior or all interior walls.
- [ ] **PIN-081** — Set dimension visibility, size, font and color per wall.
- [ ] **PIN-082** — Apply dimension presentation to the whole page.

## Symbols and text

- [ ] **PIN-083** — Filter symbols by category and search within the active category.
- [ ] **PIN-084** — Find a symbol in Favorites or the deduplicated All Residential category.
- [ ] **PIN-085** — Click a symbol tile and then click to place it.
- [ ] **PIN-086** — Drag a symbol tile directly onto the canvas.
- [ ] **PIN-087** — Place symbols at their named default real-world dimensions.
- [ ] **PIN-088** — Edit symbol width and depth numerically.
- [ ] **PIN-089** — Edit symbol rotation or use 15/90-degree rotation controls.
- [ ] **PIN-090** — Mirror a symbol on either axis.
- [ ] **PIN-091** — Snap a door/opening to a wall or detach it.
- [ ] **PIN-092** — Adjust a swing-door hinge and opening side.
- [ ] **PIN-093** — Move a hosted door/opening and reattach it to a wall.
- [ ] **PIN-094** — Search the reusable room/note text library.
- [ ] **PIN-095** — Click-arm or drag a library label onto the canvas.
- [ ] **PIN-096** — Place custom text.
- [ ] **PIN-097** — Save a reusable custom label in the local text library.
- [ ] **PIN-098** — Edit text content, size, color and font.
- [ ] **PIN-099** — Set text alignment, rotation, bold and italic.

## Pages, references and calculations

- [ ] **PIN-100** — Add a drawing page.
- [ ] **PIN-101** — Duplicate the current page including its drawing.
- [ ] **PIN-102** — Rename a page from its action or by double-clicking its list row.
- [ ] **PIN-103** — Delete a page while retaining at least one page.
- [ ] **PIN-104** — Navigate pages with the list or previous/next controls.
- [ ] **PIN-105** — On the destination floor's active layer, use Layers > Floor reference to choose the previous floor. Show/hide the reference, adjust opacity and alignment, edit the source floor and return, then save/reopen. Confirm that it is visible while tracing and absent from the exported drawing. Vertex links floors explicitly rather than assuming that a printed page is a floor.
- [ ] **PIN-106** — Show/hide the report composition guide. In Vertex use Tools > Sketch composition guide; its dashed frame shows the actual sketch PDF crop, rather than a fixed portrait sheet.
- [ ] **PIN-107** — Load a raster image behind the drawing for tracing.
- [ ] **PIN-108** — Set reference-image width and opacity.
- [ ] **PIN-109** — Clear a reference image.
- [ ] **PIN-110** — Keep a tracing reference out of appraisal print/PDF output.
- [ ] **PIN-111** — See a live GLA total while drawing.
- [ ] **PIN-112** — See a list of detected spaces with name, code, area and perimeter.
- [ ] **PIN-113** — See whether each classified space contributes to GLA.
- [ ] **PIN-114** — See a rectangle multiplication or short rectangle-component sum.
- [ ] **PIN-115** — See a strip-method component count and subtotal for complex orthogonal shapes.
- [ ] **PIN-116** — See chorded footprint and curve-area adjustment for curved spaces.
- [ ] **PIN-117** — See a straight/angled-footprint explanation for other shapes.
- [ ] **PIN-118** — Check that final GLA uses the unrounded areas instead of adding their already rounded displayed values.
- [ ] **PIN-119** — Exclude basement, garage and other non-GLA area types from the GLA sum.

## Files, history and output

- [ ] **PIN-120** — Start a new sketch.
- [ ] **PIN-121** — Save an editable Vertex project to a selected local file.
- [ ] **PIN-122** — Save again to the previously chosen file.
- [ ] **PIN-123** — Save As under a different file name.
- [ ] **PIN-124** — Save a local project through the native file dialog without a browser file picker.
- [ ] **PIN-125** — Import a saved Pinc .pinc or .json project into Vertex.
- [ ] **PIN-126** — Import a version-2 Pinc project and inspect its translated pages and drawing.
- [ ] **PIN-127** — Undo and redo drawing/project edits.
- [ ] **PIN-128** — Copy, paste and delete selected drawing content.
- [ ] **PIN-129** — Print a portrait appraisal report.
- [ ] **PIN-130** — Include drawing, class/code, area, perimeter and GLA in the printed report.
- [ ] **PIN-131** — Include compact per-area calculation explanations in the report.
- [ ] **PIN-132** — Show page GLA and aggregate project GLA on multipage reports.
- [ ] **PIN-133** — Use Tools > Export sketch PDF; open the PDF and inspect its outlines and text at high magnification. Check that tracing images, grid and selection controls are absent.
- [ ] **PIN-134** — Keep symbol export stroke widths independent of workstation zoom.
