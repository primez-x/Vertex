# Wall properties and active-chain history

Outcome: physical walls remain editable after creation, and undo/redo during
drawing leaves the next node at the authoritative retained/restored endpoint.
This closes two concrete gaps in the existing 2D wall workflow; the full
production goal and compatibility gates remain unchanged.

Ownership: worker owns `main_window.cpp` and the native boundary workflow
fixture for the curve-properties correction. Root owns the wall-chain fixture,
documentation, integration, builds, Git and delivery. Root resumes editing
`main_window.cpp` only after the worker returns ownership.

1. Make Classification in the curved-wall dialog effective. Classification-only
   changes preserve exact geometry, original receipts, hosted openings and
   unknown metadata. Combined curve/classification edits publish one command.
   Unchanged Apply, cancel and stale context do not create an edit.
2. Reconcile an active wall chain after document undo/redo using its authored
   owner identities and current baselines. Retain undone identities for redo;
   prune an abandoned branch when a new edge is accepted. No screen coordinate
   or stale retained baseline becomes measurement authority.
3. Verify with actual Qt modal and mouse workflows, including both unit systems,
   exact corners, undo/redo, branch replacement and save/reopen. Record failing
   baseline checks before changing implementation. Review the integrated
   behavior and run the affected wall, boundary and calculation checks.
4. Update practical user tasks, preserve existing results, then commit, push,
   verify the remote ref and deliver the matching local build/source bundle.
