# Connected wall movement

Outcome: dragging one or several selected walls moves their shared endpoints
through the persisted constraint system. Adjoining walls and hosted openings
must preview the proposed result and commit as one undoable edit. Locked or
invalid geometry must reject the whole operation without changing the project.

The selected walls receive explicit endpoint targets, independent of selection
order. Preserve their lengths, signed sweeps, entered measurement receipts,
and optional metadata. Solve only owners reached through persisted relations;
do not infer connections from nearby geometry. Neighbor edits must use the
existing typed geometry history, including save/reopen and recovery authority.

Ownership: the core worker owns constraint_authoring.hpp/.cpp and its focused
core test. The canvas test worker owns a new connected-wall canvas regression.
Root owns MainWindow, PlanCanvas, build registration, integration, documentation,
packaging and Git. Preserve unrelated dirty files.

Verification: reproduce current rejection first; then single and multiple wall
drag, connected preview without persistence, hosted-opening placement, entered
length preservation, locked geometry refusal, atomic undo/redo and save/reopen.
Recheck snapping, wall measurements and exterior appraisal boundaries. Inspect
the rendered canvas and the delivered executable. This checkpoint does not
close the full production goal or certify Apex compatibility.
