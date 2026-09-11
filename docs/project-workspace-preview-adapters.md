# Workspace preview adapters

`ProjectWorkspace::prepare_constraint_authoring` and `prepare_boundary_commit`
accept existing sealed previews and return ordinary `PreparedWorkspaceEdit`
tickets. Preparation forks the current complete document snapshot and invokes
the authoritative preview apply function. This retains its rejection, document
identity, revision, full source digest, display integrity, and recomputation
checks. Failed validation cannot mutate the workspace.

The adapter derives a stable ID-ordered entity upsert/erase diff from the
validated fork, preserves its command message, and passes that command through
the normal workspace preparation path. Commit retains workspace identity and
epoch checks, one document history event, lifecycle navigation, and generation
accounting. An entity-only guard rejects unexpected asset mutations or no-ops.

Boundary preview preparation is a document edit. Call `prepare_finish_boundary`
to finish and retire an active authoring session through its lifecycle.

The implementation is a separate translation unit that links both
`sketch_project_workspace` and `sketch_constraint_authoring` (with boundary commit
provided by the workspace dependency). It is kept outside the base workspace
library to preserve architecture-optional builds. The Windows desktop links the
adapter library and routes recovered constraint and boundary commits through it;
legacy documents retain their direct compatibility path until migration is
complete.

Focused tests compare prepared snapshots to direct authoritative apply, check
atomic rejection for rejected/stale/foreign previews and foreign tickets, and
verify workspace history plus undo/redo for both adapters.
