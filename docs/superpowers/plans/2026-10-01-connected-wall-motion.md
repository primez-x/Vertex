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

## Focus ownership follow-up, 2026-10-03

Outcome: transferring focus away from the canvas cancels the current pointer
gesture and invalidates deferred move results before they can commit. It also
releases the Space-pan latch so the next ordinary click can select or draw.
Existing wall/boundary drafts and armed component placement remain available.

Root owns plan_canvas.cpp, documentation, builds, Git and delivery. The regression
worker owns connected_wall_canvas_tests.cpp and boundary_canvas_tests.cpp.
Freeze writers before builds and preserve unrelated temp.txt.

- [x] Reproduce a released pending move surviving FocusOut and a latched Space-pan.
- [x] Reuse gesture serial invalidation on FocusOut; clear held-input ownership.
- [x] Verify late and already-queued move completion cannot commit, a fresh move
  works, ordinary pan/marquee stop, and no draft cancellation is dispatched.
- [x] Run the complete affected native canvas checks and inspect their captures.

The delivery record must bind the commit, remote ref and matching installed
checkpoint. This is interaction evidence, not full production certification.
