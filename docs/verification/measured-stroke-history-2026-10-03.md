# Measured-stroke history continuity

Active Measured lines strokes now continue through their own segment Undo/Redo.
The pen returns to the exact retained endpoint, including the original anchor
when every side is undone. A different side after Undo keeps the stroke and
retained receipt/vertex identities while replacing the abandoned Redo branch.
Undo at an uncommitted anchor cancels only that local point. Valid no-op Redo
leaves drawing active. Explicit finish, closed strokes and native reopen do not
unexpectedly start drawing again.

The session retains only revision IDs and an index. Existing immutable document
history owns complete geometry, receipts, entities and assets. Before and after
navigation, the desktop validates document identity, exact entity/asset maps and
drawing context before restoring the model and pen. Stale, foreign or failed
transitions end local authoring while ordinary document history remains usable.
Workspace-backed recovery follows the same exact checks. No command codec or
project-format change is required.

## Actual verification

The pre-implementation native regression failed at the assertion that Ctrl+Z
retains the active pen. Release builds succeeded after implementation. Eight
affected executable checks passed: measurement_linework_history_desktop,
measurement_linework_desktop, measurement_linework_area_desktop,
measurement_area_source_desktop, boundary_editing_desktop,
desktop_workspace_recovery, project_store and appraisal_details_panel (each
has a `_tests` executable).

The new fixture uses real canvas Ctrl+Z/Ctrl+Y and clicks. It covers repeated
history to the anchor, typed expressions and exact world replay, stable IDs,
branching, local anchor cancellation, current and stale no-op Redo, foreign
edits, finish/close/retrace, layer/workspace/phase context, workspace-backed
history and saved/reopened strokes. An additional actual active-save/reopen
fixture verifies that saving retains the pen and reopening does not resume it.
The final changed target also passed directly through CTest with its declared
offscreen Qt and runtime paths.

An initial test fixture mixed Qt enum keys and an integer in an initializer
list; explicit casts repaired compilation. A later layer fixture switched to
the already active layer because layer creation selects it. It now restores the
original layer before testing a real context change; no layer protection was
weakened. Root reviewed the integrated source and inspected captures after Undo
and continuation. No user application was launched by these offscreen checks.

Logs and native captures are retained under
`artifacts/measured-stroke-history-20261003`. Checklist U368 provides user steps;
its user result remains Not tested. The full production goal, actual Apex
compatibility, normative ANSI validation, source/license qualification and
clean-machine/network-denied acceptance remain unfinished. Installation and
remote-ref evidence are recorded separately in that folder's delivery metadata.
