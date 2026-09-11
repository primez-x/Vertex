# Geometry operation primitives

`geometry_operations.hpp` supplies immutable, validated boundary operations.
It is a lower-layer contribution to APX-KEY-003, APX-KEY-004, APX-EDIT-002,
and APX-EDIT-003. None of those production acceptance gates is complete.

## Identity and geometry contracts

All identified-boundary inputs and outputs pass the existing boundary entity
codec validation, including exact endpoint joins, supported identity syntax,
unique topology identities, finite analytical geometry, and intersection checks.
Failures throw `std::invalid_argument`; the source remains unchanged.

Rotation uses an explicit world-space pivot and radians. Horizontal reflection
reflects y about the supplied pivot, vertical reflection reflects x. Reflections
negate arc sweeps and signed area; rotations retain sweeps. IDs remain stable.

Insertion splits a selected segment at an explicit fraction of its line or arc
length. It preserves the original ID on the first segment and requires caller
IDs for the new vertex and second segment. Curves remain analytical circular
arcs. Tiny, unrepresentable or invalid resulting geometry is rejected. The new
vertex is a geometric subdivision; this helper adds no solver constraint and
does not calculate constraint degrees of freedom.

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

## Logical shortcuts

`apex_operation_preset` maps case-sensitive logical operation names such as
`Auto Close` and `Insert Vertex` to deterministic application command IDs.
`apex_command_id` returns an empty view for unknown names. These names are
an application vocabulary, not a verified mapping of Apex physical keys.
`shortcut_conflicts` reports sorted duplicate exact shortcut strings, even when
the repeated command is identical, and rejects empty shortcuts/commands.
The caller must normalize platform key chords before conflict detection.

## Integration still required

No UI dispatch, physical key preset verification, editable shortcut persistence,
workspace command history, undo/redo, file reopen, or semantic dependency update
is implemented here. In particular, callers must not replace a document entity
with the geometry result while silently dropping its owned semantics. Existing
document commands and integrity checks remain authoritative.

APX-EDIT-004 reopen/redefine/delete/cancel/restore lifecycle support is outside
these helpers and remains open. Other areas, architectural objects, annotations,
and references are also outside the supported transform types. A future command
adapter must retain exact original snapshots for undo; inverse floating-point
transforms are not an exact restoration mechanism.

`geometry_operations_tests.cpp` checks pivot rotation, handedness and arc sweep,
line/arc subdivision area and perimeter, clone identity/translation, exact
closure, bay validation, command mapping/conflicts, and invalid inputs. These
are core tests, not evidence of end-user workflow or persistence parity.
