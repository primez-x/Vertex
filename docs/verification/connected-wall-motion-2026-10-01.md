# Connected wall motion

The wall workflow repair established durable endpoint relations. This change
uses those relations when one or more selected walls move. The core regression
first failed with "Constraint authoring intent has no changes"; the new move
intent now provides explicit targets for every selected wall, independently of
selection order. It preserves selected lengths and signed sweeps, solves only
explicitly related owners, and validates locks and hosted openings before any
mutation. Existing exact measurement receipts and optional receipt metadata
survive rigid edits. A neighbor whose length changes uses the existing guarded
receipt invalidation path.

Desktop wall-only drags use the sealed constraint preview and typed history,
including recovery workspace authority. Analytical interactive projection shows
the changed wall footprints, their doors/windows and updated wall lengths. It
runs through the existing background preview queue instead of blocking pointer
input with model regeneration. Numerical rigid wall transforms use the same
typed solver path. All affected owners commit in one command; undo, redo,
history replay and project save/reopen preserve the result.

Independent review identified mismatches between the last pointer proposal and
release point, plus stale document heads. The gesture now captures document
authority on press, requests the exact final release proposal and refuses a
changed source. A released proposal waits for validation before committing.
Escape, scene replacement and a new gesture invalidate late completions. An
unavailable projection cannot commit even if the underlying geometry could be
valid. Review also found pending-release cancellation and failed-projection
continuation defects; both were repaired with explicit regression cases.

Seven focused Release checks passed across the final relevant runs:
constraint_authoring, project_workspace_preview_adapters,
named_plan_vertex_desktop, wall_opening_palette, wall_chain_connection,
connected_wall_canvas and wall_measurement_desktop. The canvas regression uses
actual Qt widget events for frame-interior dragging, Ctrl-click group selection,
preview without persistence, release at a different point, release without a
move event, source-head rejection, Escape during pending release, unavailable
async projection, anchored refusal, hosted windows and atomic history. Rendered
captures are retained under artifacts/connected-wall-motion-20261001. These are
technical checks; the user's checklist remains untested for the user.

Runtime inspection found no unresolved imports across 113 component binaries.
The executable SHA-256 for this checkpoint is
4875AE449D41B81B679F074319CCF5131DD3F6CAFB44216AF7CFEB77821E6700.
Manual tasks U289–U291 cover dragging connected walls, moving several walls and
refusing a locked move.

Remaining production work includes automatic physical corner miters, live
connected previews for the rotation handle, mixed wall/presentation group
movement, curved-wall connection authoring, large constrained network performance,
and the existing Apex/device/clean-machine qualification gaps. Opening a legacy
project does not infer new connections. Source-derived exterior appraisal
boundaries remain stale until their explicit reviewed refresh. This change does
not certify the complete production release.
