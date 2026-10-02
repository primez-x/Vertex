# Curved-wall properties and drawing history — 2026-10-02

The curved-wall editor now applies Classification. A classification-only edit
preserves exact geometry, original construction receipts, hosted openings and
opaque entity metadata. A combined curve/classification edit is one command.
Unchanged Apply, Cancel and stale context leave the project unchanged.

During physical Wall drawing, keyboard Undo/Redo resumes at the authoritative
endpoint of the retained/restored chain. Undoing all accepted edges retains the
original anchor. A new edge after Undo replaces the abandoned branch. Undo on
an uncommitted anchor cancels that start without undoing an earlier project
edit, including through the toolbar when the document has no history.

Workspace history preserves this authoring context when no measurement draft
needs restoring. A recovered measurement draft still restores its own workflow.
The current wall geometry supplies continuation and curve tangents; retained
screen coordinates never supply measurement authority.

## Evidence

Release builds succeeded. Native Qt checks ran offscreen:

- `boundary_workflow_tests --wall-curve-properties-only`: actual modal edits in
  both unit systems, exact classification-only preservation, combined edits,
  hosted openings, no-op, Cancel, stale unit/selection/document refusal,
  Undo/Redo and save/reopen.
- `wall_chain_connection_tests --active-chain-history-only`: actual Ctrl+Z/Y
  in Imperial and Metric with both legacy document history and workspace
  history after measurement drawing; endpoint reconciliation, undo-to-anchor,
  branch replacement and save/reopen.
- `wall_chain_connection_tests --wall-anchor-toolbar-only`: actual Undo QAction
  with a valid hierarchy and no document history.
- Full `wall_chain_connection_tests` passed, including persisted connections,
  length editing, deletion and architectural-view routing.
- `boundary_workflow_tests --wall-precision-only` passed with curved-chain
  Ctrl+Z/Y, arc restoration and subsequent D Relative turn. The replacement
  uses the restored arc's ending tangent and original replay context.
- `boundary_canvas_tests --adaptive-grid-only` passed: coherent unit/zoom
  intervals, displayed-grid/snap agreement, signed coordinates, stationary
  cursor updates, exact endpoint priority and retained geometry.
- `wall_measurement_tests` passed, including curved exterior quantities and
  withholding stale or overlapping appraisal totals.
- Full `boundary_workflow_tests` passed after the curved-history additions,
  including existing drawing, inline entry, modal, editing and grid workflows.

Actual baseline failures were recorded for ignored curved-wall Classification,
stale active-chain endpoints, workspace-history cancellation and disabled
anchor-only toolbar Undo. All corresponding focused checks now pass. One test
fixture initially iterated a temporary snapshot by reference; it now retains
the snapshot. An older closure fixture now explicitly chooses Metric for its
two-metre grid coordinates rather than clicking an Imperial-rounded point's
original unsnapped coordinate. Neither fixture issue is reported as a product
fix. The first wall-measurement launch lacked the DLL search path; the run with
the documented runtime path passed.

Logs and native screenshots are under
`artifacts/wall-edit-history-20261002/`. The current curved-wall canvas image
was visually inspected, including its physical thickness, measurement and
20 cm grid scale cue. Independent review accepted both required history fixes.
Checklist U318–U319 provides practical user checks; existing results remain
untouched.

This is scoped implementation and verification. It does not certify full Apex
parity, external project/device compatibility, all compound-history workflows,
or the unified production release. The production goal remains incomplete.
