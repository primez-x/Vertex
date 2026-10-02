# Physical wall precision input — 2026-10-02

## Behavior

In 2D with Draw set to Wall, D can place an exact start point before a canvas
click. During a wall chain, D offers length/heading, rise/run, relative turn,
world coordinate, chord/sweep, chord/height, chord/arc length and
start-tangent/arc length/sweep. Right-click during the chain offers Precise
input. Every accepted edge creates a physical wall with the current thickness
and height, independently undoable together with its new endpoint constraints.

Relative turns use the preceding wall's authoritative ending tangent, including
curves. Original expressions and their required historical replay context are
retained separately from current baseline geometry. Exact input bypasses grid
rounding. Stale document, unit, layer, selection, placement and chain contexts
cannot publish the pending edge. Cancelling retains accepted geometry and
returns focus to the invoking canvas.

## Native verification

Release builds of the application and affected native fixtures succeeded.
Qt checks ran offscreen, without opening or changing user windows:

- `boundary_workflow_tests --wall-precision-only`: five workflows, covering
  keyboard starts, all eight methods, imperial/metric units, relative turns
  after curves, wall settings, exact receipts and replay, cancellation,
  stale context, endpoint connections, undo/redo and save/reopen.
- Full `boundary_workflow_tests`: existing boundary and inline entry workflows,
  adaptive-grid integration and the new physical wall cases.
- Full `boundary_input_dialog_tests`, `boundary_canvas_tests`,
  `wall_measurement_desktop_tests` and `constraint_authoring_tests` passed.
- `desktop_smoke --wall-group-move-only` passed: wall and mixed-object
  movement, connected corners, retained input receipts, history, reopening,
  conflict rejection, cancellation and stale source. The source-build reference
  import gate rejected safely; locally generated trusted pixels then exercised
  reference editing. This is not successful isolated-import qualification.
- The adaptive grid canvas fixture also passed independently. Its visible
  spacing and snapping share one interval; foot, half-foot, inch, half-inch,
  quarter-inch and metric metre/centimetre/millimetre examples are covered.
  Zoom and unit changes preserve retained geometry and exact endpoint priority.

The initial Wall precision check failed before implementation because default
Wall drawing did not expose the exact form. Integration exposed strict replay
context misuse; unused previous-segment and closure inputs are now null.
Further native regressions failed before corrections for pending text-placement
ownership, untouched curve-coordinate rounding and lost canvas focus after
publishing a keyboard anchor. The final five-workflow run passes all three.
An untouched curve editor now retains original coordinates and sweep and
creates no transaction for a no-op Apply.

Screenshots under `artifacts/wall-precision-20261002/wall-precision-ui/` show
the heading form, analytical arc form and the rendered thick curved wall with
dimensions and the grid scale cue. The heading form and curved-wall canvas
were visually inspected.

The older grouped-movement fixture required maintenance: it inspected the
document immediately after an asynchronous release and still expected a
single-wall drag to reject connected motion. That behavior was superseded by
the October 1 connected-wall implementation. The fixture now waits for the
bounded queued result, verifies connected-corner motion and exact undo, and
retains the mixed-group conflict assertions.

User checklist tasks U315–U317 describe this behavior. These checks do not
certify complete Apex keyboard parity, physical pen/touch hardware, native
Apex compatibility or the complete production release. Live-chain undo/redo
continuation and original receipt retention through connected curved resizing
remain additional coverage work; no passing result is claimed for them here.
