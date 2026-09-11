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

`make_schedule_edit` returns a deterministic command description containing the
source target, expected document revision, prior value and replacement. It
rejects edits to calculated cells with an explanation naming the sources, type
or dimensional-unit changes, nonfinite values and invalid marks. Command
creation does not mutate the snapshot or semantic model.

The document integration must validate the revision and prior value, enforce
semantic constraints such as positive widths, translate the description into a
normal document command, and regenerate calculated values after acceptance.
This module does not yet create document history, persist schedules, group
material quantities, calculate geometry, or provide UI. Its tests establish the
projection/edit contract, not the complete production acceptance criteria.
