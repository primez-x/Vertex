# Linked floor tracing

Outcome: implement PINC-004 as a live, aligned floor reference for 2D drawing.
Store the optional link, visibility, opacity and XY offset on its destination
floor. Resolve source geometry from the current document; never copy geometry
into a new measurement boundary or infer appraisal facts from a reference.

When a floor has a tracing link and is active, the ordinary measurement canvas
focuses on that floor. Other floors remain in the project and can be activated
through Layers. The focus is independent of ghost visibility, so hiding the
ghost does not change exported content. Existing sheet scopes and 3D geometry
remain independent. Invalid/deleted sources show a repairable diagnostic and
never reuse a stale ghost. Clearing the link restores the ordinary multi-floor
view and leaves each user's visibility masks intact.

Layers provides a compact source selector and show/hide control, opacity,
alignment and clear actions. The heading names the destination floor and
communicates the focused view. The source selector lists other floors in the
same building. The user chooses the preceding floor explicitly: project names
and UUIDs do not define a reliable floor sequence. Offsets use application units
and store metres.

Ownership: core worker owns the new typed codec/resolver and core tests;
renderer worker owns PlanCanvas and screen/output isolation checks; root owns
UI, projection wiring, public APIs, CMake, documentation, all native jobs and Git.
All native writers must return frozen before any native job starts.

Verification: real source floor edits, curved and physical walls, symbols,
labels/dimensions, visibility/opacity/alignment, source deletion and repair,
read-only/stale-edit refusals, Undo/Redo and save/reopen. Screen ghost pixels must
change while vector/sketch/sheet outputs, selection, snapping and measurements
exclude the ghost. Inspect the actual captured canvas. The architectural plan
lane requires correct view-frame projection; do not claim it from an XY-only test.
