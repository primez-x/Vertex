# Geometry operation primitives

`geometry_operations.hpp` supplies immutable, validated boundary operations.
It is a lower-layer contribution to APX-KEY-003, APX-KEY-004, APX-EDIT-002,
APX-EDIT-003, and APX-AREA-001. None of those production acceptance gates is
complete.

## Identity and geometry contracts

All identified-boundary inputs and outputs pass the existing boundary entity
codec validation, including exact endpoint joins, supported identity syntax,
unique topology identities, finite analytical geometry, and intersection checks.
Failures throw `std::invalid_argument`; the source remains unchanged.

`segment_bounds` and `boundary_bounds` include endpoints and the cardinal
extrema that lie on each directed circular arc. They accept open segment sets
without certifying enclosure or topology. Empty boundaries and invalid numeric
geometry reject instead of inventing extents. Shallow-arc extrema use stable
half-angle differences to avoid losing the curve height through subtraction of
nearly equal center and radius values. The desktop uses these shared bounds for
boundary transform pivots and fitting retained canvas geometry.

`PlanarTransform`, `transform_point`, and `transform_segment` share the desktop
operation order: rotation about a pivot, X/Y reflection about that pivot, then
offset. Finite inputs and output are required. Arc sweep reverses for one
reflection and is retained for two. Identity transforms preserve signed zero.
Receipt schema 3 uses these primitives to transform replayed geometry while
retaining the original local measurement inputs.
Replay checks transformed endpoints against an origin-relative reference and
each analytical edge length against its original length after every frame.
Non-finite residuals or cumulative shape drift beyond the geometry tolerance
(normally 1e-7 metres) reject the operation. Pure translation rounding is also
accumulated and checked, so an offset cannot silently disappear below coordinate
resolution. These checks do not certify arbitrary-scale affine placement;
coordinates must remain representable at the required precision.

For nearly parallel straight segments, overlapping axis-aligned bounds alone
do not imply an uncertain intersection. The validator can prove separation
when both endpoints lie strictly on the same side of the other segment's line,
beyond the geometry tolerance and a floating-point error margin. Potential
crossings, near-contact within tolerance, and non-finite intermediate values
retain the conservative fallback. This admits ordinary rotated rectangles
without weakening the crossing and uncertainty diagnostics.

Arc/arc contacts retain both positive-height circle roots even when radial
penetration is below the metre tolerance. Contact deduplication uses the actual
distance between roots. Uncertain discriminants return indeterminate. An exact
axis-aligned semicircle tangent can be admitted only when compensated endpoint,
midpoint, center and radius arithmetic establishes it, including every prior
origin subtraction. Rounded translation cannot establish that exception.
For exact shared endpoints, arbitrary sweeps and rotations use endpoint-anchored
circle equations. A single shared contact is admitted only when the complete
second-root distance and its numerical error allowance lie within the metre
tolerance. Origin/chord subtraction and power-of-two scaling must retain their
inputs exactly. Other contacts retain the existing two-root or indeterminate
path.

An indeterminate arc/arc result now has a bounded rational retry on the original
binary64 endpoints, before any floating-point origin subtraction. Circle
coefficients use exact binary64 decomposition and rational Taylor enclosures
for the stored sweep's half-angle. Canonical positive/negative half turns use
exact sine and cosine values, matching the existing semicircle convention.
The radical-axis quadratic can certify a negative discriminant, retain genuine
positive roots, or establish exact zero for a polynomial half-turn case.
This includes rotated interior semicircle tangency without a shared endpoint.
Generic trigonometric intervals containing zero remain indeterminate unless an
independent selected-arc separation proof establishes a strict gap; an intended
tangent construction is not proof of exact stored-input tangency.

Selected-arc projections also permit separation without a unique radical axis.
For each of the x/y axes, two diagonals and both original chord normals, the range encloses both original
endpoints and the supporting circle's two stationary projection points. Only a
proved-outside stationary point is discarded. A strict squared projection gap
greater than four times the squared metre tolerance times the direction's squared
length proves separation even after allowing tolerance at both arcs' endpoints.
The contact kernel may admit a supporting-circle root near each endpoint even
outside both selected sides; a gap above twice tolerance excludes that case as
well as actual contact and tolerance-close geometry. This can admit opposed
subarcs of nearly concentric circles whose axis-aligned boxes touch. Chord normals
also cover long separated pieces whose projections overlap in every fixed-axis
direction, tying the proof to their actual orientation. Failure in these
directions remains inconclusive; it does not manufacture a root or prove
clearance. The projection arithmetic shares each attempt's existing fixed budget.

Output points have bounded rational square-root and binary64 rounding
enclosures. Each published point's near/far classification at all four original
endpoints must agree with its entire root enclosure; an ambiguous threshold
remains indeterminate rather than becoming an expected adjacent join.
Deduplication requires the second root's entire enclosure to fit within
tolerance of the retained output and preserve its endpoint classifications.
Refinement is finite (12, 24 and 48 Taylor terms, 192 square-root bisections,
a 20,000-operation budget per attempt
and a 32,768-bit intermediate arithmetic limit); exhausted or uncertain cases
remain indeterminate. The ordinary fast contact paths are retained. Numerical
error allowances and general tangency/runtime qualification remain open.

Strict hole topology also checks analytical arc/arc clearance after a contact
miss. A supporting-circle gap or selected-arc projection gap can prove complete
separation directly. Otherwise,
certified selected-arc contact absence precedes the complete nonconcentric
minimum-distance candidate set: endpoints, endpoint radial projections and both
circles' center-axis combinations. Only proved-outside candidates are discarded;
all others require a squared-distance lower bound strictly above tolerance.
Clearance and the contact prerequisite each use their own bounded arithmetic
context at each fixed refinement. Concentric or uncertain cases without a strict
separation proof reject conservatively. Proving no intersection does not permit
tolerance-close holes. Projection behavior and performance remain uncompiled
and runtime-unqualified in the current source-only work.

Rotation uses an explicit world-space pivot and radians. Horizontal reflection
reflects y about the supplied pivot, vertical reflection reflects x. Reflections
negate arc sweeps and signed area; rotations retain sweeps. IDs remain stable.

Insertion splits a selected segment at an explicit fraction of its line or arc
length. It preserves the original ID on the first segment and requires caller
IDs for the new vertex and second segment. Curves remain analytical circular
arcs. Tiny, unrepresentable or invalid resulting geometry is rejected. The new
vertex is a geometric subdivision; this helper adds no solver constraint and
does not calculate constraint degrees of freedom.

The desktop's typed insertion command also updates dependent dimensions and
endpoint constraints without changing their original endpoint positions.
It preserves the exact construction receipt as derivation evidence for a
normally drawn boundary. A second-piece automatic dimension is included in
the same command when the original edge has one. Replay validates the complete
split, references, and new identities; a raw entity edit cannot authorize the
otherwise forbidden change to the retained edge's end-vertex identity.

The insertion dialog previews the canonical split, labels original edges by
number and marks the new vertex in green. Apply uses the exact captured command
and identities; invalid input or a changed document, selection, workspace, layer
or units discards the candidate. Cancel preserves the source.

Persistent coordinate freedom is diagnosed by PlaneGCS without solving or
moving geometry. Straight identified boundaries and walls contribute their
independent X/Y endpoint variables; explicit saved relationships expand the
connected component. Temporary editing anchors, wall thickness, height, fused
wall joins and curve parameters do not participate. Before/after comparisons
retain the union of both components, so removing a bridge does not silently
drop neighboring objects. A copy reports its source and copied components
separately. Curved owners, unsupported relationships and ambiguous bindings
report unavailable rather than zero. This local rank report is not a count of
all architectural parameters or a production qualification.

Direct vertex movement updates both incident endpoint occurrences of the stable
vertex ID and retains every boundary, segment, and vertex identity. Direct
segment resizing accepts an analytical target length and an explicit fixed
endpoint. For an arc, the retained sweep makes analytical length proportional
to chord length, so resizing changes the chord and radius without converting
the arc to a line. With connected movement disabled, only the opposite vertex
moves and its two incident edges reshape. With connected movement enabled, the
complementary boundary chain translates as a unit while the two edges meeting
the fixed endpoint reshape. These primitives do not infer connections to other
objects from coincident coordinates.

The desktop exposes selected boundary vertices as screen-sized handles. A drag
previews locally and commits one revision on release through the typed
`EditBoundaryGeometry` command. Escape cancels without a revision. The boundary
geometry editor accepts a displayed-unit length, start/end anchor, and connected
chain choice. It previews the canonical `Document::preview_command` result in
an isolated canvas, including both outlines, fixed point, changed vertices,
attached dimensions, analytical area, and perimeter. Apply retains the exact
typed candidate and rechecks it against the unchanged captured source; Cancel
does not publish a command. Invalid input, locked constraints, or changed
document/selection/layer/workspace/units clear the candidate and disable Apply.
Prefilled rounded lengths and curve measurements are presentation values:
leaving them unchanged does not resize or reconstruct an existing edge. Changing
only an arc's side retains its unrounded analytical length. The measured-stroke
editor also keeps unchanged coordinates and lengths directly from its captured
source, avoiding a feet-conversion round trip through the exact quantity parser.
Changed expressions still use that parser and retain their authored quantities.
For a straight selected boundary, an independent related-object option routes
the resize through the shared endpoint solver. The canonical selected resize
pins every selected boundary vertex; explicit relationships may move other
straight walls and boundaries, or reject when those owners are frozen. The
saved transaction retains the selected `resize_segment` proof followed by
related boundary vertex edits and surgical wall edits. Original construction
receipts remain archived; the new target length uses the existing analytical
metre value in the resize proof. Pure vertex groups may replay atomically when
sequential intermediate geometry would be invalid, while nonvertex edit groups
retain their ordered semantic replay.
Straight boundary vertex movement uses the same solver with the canonical selected
shape pinned, retaining a selected `move_vertex` proof and all affected related
owner edits in one transaction. The canvas projects exact proposal geometry from
detached snapshots asynchronously. Serial, source revision, and requesting-canvas
checks discard canceled or obsolete results; active plan crop clipping also applies
to proposed strokes. The final command recomputes admission rather than trusting
screen overrides. Local analytical curve edits keep their existing command path.
Deferred completions also carry dependent dimension labels/strokes and full-model
area/perimeter values. Font footprints are captured on the UI thread; area-name
placement uses the same pure containment and component-avoidance helper as normal
rendering. Annotation overrides and metrics expire with their geometry serial and
never enter retained document labels or output scenes.
Stale revisions, invalid topology, degenerate edges, and
self-intersections reject atomically. Handles are interaction overlays and are
excluded from print/export rendering.

Cloning requires a distinct boundary ID, a complete explicit segment/vertex ID
map, and a translation in metres. No segment or vertex ID can reuse its source
namespace's identities. Project-wide identity allocation remains the caller's
responsibility. Cloning copies boundary geometry/type only, with no document
properties, annotations, dimensions, receipts, relationships, or constraints.

Point jump resolves an existing stable vertex ID to its exact world-space
point; it does not add an edge or change a construction cursor. Automatic
closure adds one straight edge only when needed, requires exact existing joins,
and validates the closed result. It performs no tolerance snapping or repair.
Bay completion returns three straight edges through two explicitly supplied
shoulders. The shoulders must progress strictly along the opening, lie on the
same side, and enclose a valid simple shape with the opening chord. This is an
explicit four-point helper, not inference of missing dimensions or arbitrary
bay forms. Integration into surrounding boundary geometry needs final validation.

`detect_closed_boundaries` walks the endpoint graph as a planar half-edge
embedding and returns each simple counter-clockwise bounded face. It preserves
line and arc geometry, rotates each result to a stable lexical start point, and
orders faces by their source edge and area. Open wall stubs are ignored, while
duplicate geometry, malformed endpoints, non-finite values, and zero-length
segments fail closed. Nested loops remain separate simple faces; callers that
need a hole relationship must model that relationship explicitly in the area
contract.

## Logical shortcuts

`apex_operation_preset` maps case-sensitive logical operation names such as
`Auto Close` and `Insert Vertex` to deterministic application command IDs.
`apex_command_id` returns an empty view for unknown names. These names are
an application vocabulary, not a verified mapping of Apex physical keys.
`shortcut_conflicts` reports sorted duplicate exact shortcut strings, even when
the repeated command is identical, and rejects empty shortcuts/commands.
The caller must normalize platform key chords before conflict detection.

## Integration status and remaining work

The Windows desktop dispatches point jumping and automatic closure from its
menu and command palette. `complete_bay_window_return` takes exactly joined
straight entering/front edges and returns the matching third edge by reflecting
the entering direction across the front. The result must pass the existing
bay-shoulder and topology checks. It leaves the surrounding outline open and
does not snap analytical coordinates.

The canvas **B** command and **Complete bay-window return (B)** menu/palette
action use that helper during either a measurement draft or physical wall
chain. Measurement authoring retains the ordinary construction receipt,
dimension policy and draft undo/redo; physical walls retain the ordinary guarded
wall creation path. This is distinct from the direct three-point bay-profile
API, which creates an entire closed measurement boundary. Point jumping updates
the precision pointer without dirtying the project.
The separate `measurement_linework` model retains open measured strokes with
individual construction receipts. It deliberately does not relax a closed
boundary's topology contract or pretend that pointer motion starts a new edge.
The desktop now exposes **Measured lines** as the third Draw choice; physical
Wall remains the default. Clicks commit retained exact edges individually. **D**
accepts a distance and a counterclockwise heading from +X. Enter, stationary
right-click or Escape finishes the stroke while keeping committed edges.
**Tools → Lift measured pen** starts an independent stroke. Select a saved
stroke and use **Tools → Jump to measured or boundary vertex** to relocate to
its exact stable vertex without adding a connecting edge. **A** closes a stroke;
Enter accepts an alignment proposal through the ordinary receipt/history path.

With a stroke selected, **Tools → Detect closed areas** collects measured strokes
in the same property/building/floor/layer context, asks for classification, and
creates real `measurement_boundary` entities in one undoable transaction. Source
strokes remain intact. Derived edges retain source owner/segment IDs, parameter
intervals and traversal direction. Repeating detection skips identical derived
geometry; changed geometry with already-used lineage requires explicit review.

`measurement_area_graph.hpp` nodes analytical straight and circular-arc crossings,
T junctions and coincident overlap, deduplicating retracing with all source uses.
Bridges and open stubs do not create faces. Nested cycles requiring holes and
ill-conditioned contacts/arcs reject explicitly. Limits are 2048 sources, 16384
represented stations/derived edges and 65536 processed contacts. Tolerance sets
the minimum usable edge length and never merges nearby geometry.

Retained strokes project into the canvas, dimensions, snapping and shared output.
DXF exports analytical lines/arcs and explicitly diagnoses loss of native typed
expressions and identities. Unsupported receipt models stay opaque rather than
becoming guessed geometry. See the [canvas plan](plans/2026-10-03-measurement-linework-canvas.md)
and [verification checkpoint](verification/measurement-linework-canvas-2026-10-03.md).
Measured-line history restores the same stroke and retained pen after Undo.
Stable-vertex jumps start an independent stroke without a connecting edge.
The shared precise-input dialog accepts combined rise/run, relative turns and
analytical chord/angle, chord/height and chord/arc-length entries through the
ordinary receipt path. Stroke transforms/editing, nested holes and real native
Apex roundtrips remain scoped gaps. This checkpoint does not certify full
production or ANSI compliance.
Physical key preset verification, editable shortcut persistence, and broader
semantic dependency migration remain open. Callers must not replace a document
entity with a geometry result while silently dropping its owned semantics;
existing document commands and integrity checks remain authoritative.

Coordinate-only edits preserve dimension target IDs, including the secondary
segment and shared vertex used by angle dimensions. Topology-changing insertion
still requires explicit reference migration for receipt-backed geometry and
angle targets; that broader insertion path remains open.

APX-EDIT-004 lifecycle support is outside these geometry helpers. The typed document
and workspace adapters support redraw, cancellation, guarded deletion and exact
history restoration. Changed-count redraw includes a native review of explicit
edge/vertex mappings and eligible reference removals, with canonical dimension and
constraint preview before atomic finish. The archived input binds those choices
to the exact replacement geometry and identities. Broader object lifecycle and
production compatibility qualification remain open. Other areas, architectural objects, annotations,
and references are also outside the supported transform types. A future command
adapter must retain exact original snapshots for undo; inverse floating-point
transforms are not an exact restoration mechanism. The desktop's automatic area
command uses `detect_closed_boundaries` to create independent room entities in
one Document transaction; production face, hole, and Apex-output fixtures are
still required.

`geometry_operations_tests.cpp` checks pivot rotation, handedness and arc sweep,
line/arc subdivision area and perimeter, clone identity/translation, exact
closure, bay validation, stable-ID vertex movement, anchored line/arc resizing,
connected-chain behavior, command mapping/conflicts, and invalid inputs. Canvas,
document, storage, and desktop fixtures cover preview/commit separation,
cancellation, receipt derivation, dimensions, undo/redo, and save/reopen. Native
Apex output comparison and production qualification remain open.
