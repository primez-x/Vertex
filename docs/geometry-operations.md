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

Rotation uses an explicit world-space pivot and radians. Horizontal reflection
reflects y about the supplied pivot, vertical reflection reflects x. Reflections
negate arc sweeps and signed area; rotations retain sweeps. IDs remain stable.

Insertion splits a selected segment at an explicit fraction of its line or arc
length. It preserves the original ID on the first segment and requires caller
IDs for the new vertex and second segment. Curves remain analytical circular
arcs. Tiny, unrepresentable or invalid resulting geometry is rejected. The new
vertex is a geometric subdivision; this helper adds no solver constraint and
does not calculate constraint degrees of freedom.

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
chain choice. Stale revisions, invalid topology, degenerate edges, and
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

The Windows desktop now dispatches point jumping from the More menu and command
palette, and routes automatic closure and bay-window completion through the
identified measurement-boundary command path. Those two creation operations
retain their named history action and undo/redo as one document revision; point
jumping updates the precision pointer without dirtying the project. The direct
`MainWindow` APIs are also used by the deterministic desktop smoke fixture.
Physical key preset verification, editable shortcut persistence, and broader
semantic dependency migration remain open. Callers must not replace a document
entity with a geometry result while silently dropping its owned semantics;
existing document commands and integrity checks remain authoritative.

Coordinate-only edits preserve dimension target IDs, including the secondary
segment and shared vertex used by angle dimensions. Topology-changing insertion
still requires explicit reference migration for receipt-backed geometry and
angle targets; that broader insertion path remains open.

APX-EDIT-004 reopen/redefine/delete/cancel/restore lifecycle support is outside
these helpers and remains open. Other areas, architectural objects, annotations,
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
