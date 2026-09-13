# Schedule model foundation

`schedule_model.hpp` provides a storage-independent projection for door, window,
room, material, assembly, and building-object records (ARCH-SCH-001/002). Adapters supply stable object IDs,
marks and typed properties. Values support text, Boolean flags, integer counts,
scalar numbers, and SI length/area/volume quantities. Marks are unique within a
row kind; object IDs are globally unique within a snapshot. Rows sort by object
ID, so changing input order does not change the projection.

Editable properties and the reserved `mark` cell retain their exact source.
Calculated cells carry a value, explanation and references to primitive source
properties. References must resolve inside the input record set. References to
other calculated cells are deliberately rejected, avoiding implicit dependency
cycles. Examples include gross area, deductions and net area; semantic adapters
are responsible for computing correct values and regenerating the snapshot
after accepted model changes. The projection does not infer geometry or units.

`document_schedule_adapter.hpp` provides a deterministic projection from the
shared `DocumentSnapshot`. Hosted door/window entities become rows with stable
marks, dimensions, host identity, and a calculated area whose source refs are
the width and height cells. Room entities can supply an explicit area or a
validated closed boundary, and material rows can be supplied through the
`material_name`/`volume_m3` semantic fields. The projection is stamped with the
source document revision and reports malformed or incomplete rows as sorted
diagnostics; invalid rows never become partial schedule output. The desktop
`MainWindow::scheduleSnapshot()` API exposes this same projection without a
second document model. Its organization and active design-phase visibility is
applied to source entities before projection, so visible material quantities
remain present and hidden sources contribute neither rows nor diagnostics.
The unfiltered adapter overload continues to project the whole document.
Rooms with a stored area expose that measurement as read-only source provenance
for their calculated gross area. Architectural \`room\` volumes with explicit
\`height_m\` derive net plan area from the boundary minus any \`holes\`, expose the
stored height as a read-only metre quantity, and add a read-only cubic-metre
\`volume\` calculation with boundary, hole, and height provenance. The schedule
never invents a height for legacy plan-only room records.

Room holes must be individually valid closed boundaries strictly inside the
outer boundary, with disjoint interiors and no boundary contact or nesting.
The dependency-free geometry validator checks lines and circular arcs
analytically at the default local tolerance of `1e-7` metres. Outside, crossing,
touching, overlapping, duplicate, nested, or numerically indeterminate holes
reject the complete room row with a deterministic diagnostic naming the room
and invalid hole topology. No hole is ignored or partially deducted. Valid
disjoint holes retain their analytical area and volume, including curved holes.

The architectural adapter augments assigned-material source rows with net solid
volume and appends deterministic, read-only `material_summary` rows. Assignment
groups use the catalog/material identity; explicit material rows use a normalized
name (case-insensitive with collapsed whitespace). Summary rows expose the
contributing source references, aggregate count, and the sum of net volumes when
every source has a valid quantity. A missing or invalid source volume keeps the
count visible but omits the aggregate quantity and emits a diagnostic. Source
rows remain available for per-object inspection and permitted edits, while sheet
material placements include both source and summary rows.

Architectural building objects also produce deterministic, read-only `building`
rows. Columns, beams, stairs, railings, and supported roof forms expose their
authored type, mark, canonical dimensions, counts, and angular properties where
those fields exist on the source entity. Each row includes a calculated `volume`
from the same native solid used by plan, section, elevation, and 3D projection;
its provenance is the source entity's canonical `geometry` rather than a screen
measurement. Beam length is derived from its full 3D axis, while roof opening
counts and stair riser counts remain source-backed properties. The rows honor the
active visible-entity filter and are intentionally edited through the
architectural object inspector so schedule output cannot diverge from model
geometry. New projects include a `building-objects` sheet placement beside the
default door schedule, so architectural quantities are visible in draft output
without additional sheet setup.

Placed assembly instances also produce one read-only material row for each
resolved catalog slot. The row retains catalog, instance, slot, and host
identities as source references. A declared `volume` or `net_volume` quantity
with cubic-metre units is promoted to the row's net volume; other assembly
quantities remain on the assembly row with their original names and units.
These rows participate in the same deterministic material summaries as object
and layer rows, while host visibility continues to scope placed instances.

`make_schedule_edit` returns a deterministic command description containing the
source target, expected document revision, prior value and replacement. It
rejects edits to calculated cells with an explanation naming the sources, type
or dimensional-unit changes, nonfinite values and invalid marks. Command
creation does not mutate the snapshot or semantic model.

`make_document_schedule_edit` now translates a validated edit into the normal
`ApplyEntityChanges` command. It checks the source document revision and prior
cell value, maps schedule fields back to canonical entity properties (including
hosted opening dimensions and material fields), and rejects calculated cells.
The desktop Schedules dialog exposes an **Edit selected source cell** action;
accepted edits refresh calculated values and participate in the ordinary
undo/redo history. Schedule persistence and complete sheet/print layout remain
open production work. Tests establish the Document-derived projection, command
translation, grouped material aggregation, and desktop history path, not the
complete production acceptance criteria.
