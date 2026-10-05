# Linked floor tracing checkpoint

This increment adds the previous-floor part of PINC-004. It does not certify
all Pinc operations, Apex compatibility or the unified production release gate.

## User access

Activate the destination floor's layer in Layers, then choose a source in Floor
reference. The selector lists other resolved floors in the same building. It
does not assume UUID/name order represents a physical floor sequence. A new
single-floor project does not display an empty reference panel.

Show reference toggles its screen visibility; the percentage adjusts opacity.
Align edits X/Y offsets in the selected measurement units. Reset alignment sets
both offsets to zero. Choosing No floor reference clears the link. These edits
are ordinary undoable document revisions and survive native save/reopen.

The linked destination's 2D measurement canvas focuses on that floor even when
the reference is hidden. Activate another floor's layer to work there. Clearing
the link restores ordinary project visibility without rewriting existing eye
filters. Source geometry, symbols, labels and dimensions are resolved from the
current document rather than copied into the destination. Alignment changes only
the reference's screen position. The source and its analytical quantities stay
unchanged; a reference never supplies appraisal grade, finish or eligibility facts.

## Isolation and failure handling

Reference storage and rendering are separate from committed canvas content.
Ghosts contribute no hit targets, snapping points, navigation bounds, overview
map content, composition-guide extents, quantities or exported ink. Raster
underlays paint below the reference; active drawing ink and opaque furniture
remain foreground. Ordinary translucent area fills retain reference legibility.

The focused measurement projection is separate from architecture, native model
visibility and coordinated sheet caches. Saved viewports use ordinary labels,
references and grids, so an active tracing floor cannot remove another floor's
sheet content. Missing/deleted/malformed sources clear old ghost geometry and
labels and show a repair diagnostic. A user can choose another source or clear
the link. Invalid, nonfinite, self/cross-building and stale edits are refused;
read-only projects disable mutation. Controls retain an immutable document head
as well as revision/context guards. Alignment preserves unedited exact values
despite formatted display rounding.

## Verification

Root runs all native jobs with writers frozen. Evidence is retained locally in
`artifacts/floor-reference-20261004`; final build, capture and installed results
are recorded below after their terminal checks.

Initial Release compilation passed. Seven behavioral checks passed: actual
desktop floor-reference workflow, screen renderer, sketch content output,
desktop sketch PDF, visibility workflow, boundary canvas and coordinated view
output. The first core run rejected a malformed parent fixture at document
admission before the resolver could run. The test now asserts that appropriate
dangling-reference admission refusal; the corrected core run passed. Original
failure logs remain intact.

Desktop cases exercise real source/show/opacity signals, live wall source edits,
exact offsets, analytical geometry/total invariance, Undo/Redo, save/reopen,
read-only and invalid/stale refusals, source deletion/restoration and malformed
geometry/metadata repair. Actual PDFs are reopened with Qt PDF and compared by
page size, rendered pixels and text. Both destination sketch output and a saved
source-floor viewport stay invariant when the reference is shown, hidden or
aligned. Source text and dimensions are absent from the focused sketch while
the saved source viewport retains its source dimension.

Renderer cases cover negative offsets, arcs, holes, derived stroke paths,
rotated SVG and text, source updates, opacity, foreground ink, a reference wall
inside a filled room over an opaque image, and exclusion from all public output
paths, content recordings, fit/map/guide, picking and snapping.

Independent source review found and root fixed shared-cache output leakage and
opaque-underlay occlusion. Corrective source review found no required remaining
correction; it did not claim native verification. Root inspected the light/dark
captures and corrected dark checkbox text contrast.

The final Release rebuild (`theme-build.json`) passed, and the affected desktop
workflow passed again (`final-desktop.json`) after that contrast correction.
Eight focused checks have passing outcomes in the initial, corrected-core and
final-desktop records. Root inspected both final theme captures; the source
floor, annotations and dimensions appear as faded reference ink, while the
destination selection and controls remain clear.

## Remaining limits

This is world-XY floor tracing in the 2D measurement surface. It is not arbitrary
printed-page ghosting or projection through architectural named-view frames.
Those extensions remain visible gaps rather than inferred from these tests.
Fit intentionally excludes reference extents; pan/zoom if the source extends
outside the destination's view. Opaque active symbols intentionally cover source
ink beneath them. Physical printing, clean-machine/network-denied installation,
all hardware workflows and user-observed resolution are not established here.
PINC-007 through PINC-013, physical-wall classification and complete preset/
symbol mapping remain required, along with all original production requirements.
