# Schedule model foundation

`schedule_model.hpp` provides a storage-independent projection for door, window,
room and material records (ARCH-SCH-001/002). Adapters supply stable object IDs,
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
second document model.

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
undo/redo history. Schedule persistence, grouped material quantities, and
complete sheet/print layout remain open production work. Tests establish the
Document-derived projection, command translation, and desktop history path, not
the complete production acceptance criteria.
