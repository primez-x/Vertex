# Joined boundary vertex editing — 2026-09-30

Dragging an identified straight boundary corner now moves walls and neighboring
boundaries joined by saved endpoint relationships. The selected boundary keeps
the requested canonical shape. Coordinate overlap alone creates no relationship.
The command retains selected and related geometry proofs in one reversible saved
transaction, with original construction receipts and stable identities preserved.

Exact geometry previews run on detached snapshots off the UI thread. Pending and
invalid targets receive visible feedback. Cancellation, scene replacement and
stale serials/revisions discard results. Release recomputes admission using the
final pointer position; screen overrides cannot authorize a document change.
Preview strokes respect visible objects and conventional-plan crop boundaries,
including a named plan in a later sheet/view model record. Empty overrides hide
related geometry moved wholly outside the crop. Preview scene caching distinguishes
the requesting canvas across workspace changes.

## Observed verification

The Release application and focused test targets compiled. These CTests passed:
`constraint_authoring`, `project_workspace_preview_adapters`, `boundary_canvas`,
`project_storage`, and `requirement_schema_contract`.

The native `desktop_smoke --mixed-constraint-workspace-only` workflow passed
direct joined-corner movement, deferred mouse-drag preview and release, undo,
save/reopen, exact cropped paths with a later view-model record, Escape without
source changes, and a restricted architectural preview followed by a Measurement
preview at the same revision. The latter includes the related wall that was hidden
in the architectural view. Gesture coordinates use the actual canvas zoom.

`--boundary-insertion-only` passed normal receipt-backed insertion, direct edits,
stable handle dragging, cloning and reopened typed history. The history assertion
distinguishes one insertion, two vertex moves, one edge resize and one transform.
`--boundary-geometry-preview-only` passed the existing edge-edit preview workflows.

Local runtime captures and stdout/stderr are saved under
`artifacts/joined-vertex/release/`. The live preview image was visually inspected.
An independent source review found and prompted fixes for canvas cache isolation,
crop preservation and multi-record crop lookup, then approved the last correction.

## Remaining qualification

Dimension annotations and area readouts refresh after commit; their current
positions and text are not recomputed in the live vertex preview. Analytical local
curve editing retains its existing command path, but related-object solving for
curves remains unavailable. Large-component preview and shutdown latency are not
qualified. This checkpoint does not certify full Apex compatibility, appraisal
measurement-standard compliance, installers or the complete production release.
