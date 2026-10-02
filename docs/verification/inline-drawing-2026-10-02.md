# Exact inline drawing input — 2026-10-02

## Delivered behavior

During an active 2D Wall chain or measured outline, a compact canvas control
accepts exact lengths and cardinal directions. Typing a number on the canvas
starts input; an arrow commits that direction, and Enter repeats the visibly
selected direction. The expandable keypad uses the same command path.
Implicit units are feet or metres; explicit units and mixed fractions retain
their Quantity provenance. Typed edges bypass grid rounding.

Each physical wall is one immediate undoable command. Measured outlines retain
local edge/dimension history until their compound commit. Define First waits
for manual dimension placement. Escape in the input clears text without
discarding the draft or committed walls. Changed source, selection, units,
layer or authoring context cannot commit a stale entry.

Wall input receipts are optional historical metadata. Creation validates their
replay against the authoritative baseline; current geometry remains the sole
measurement authority. The existing measured-boundary D dialog remains available
for headings, relative turns, curves and dimension positioning.

## Verification

The Release desktop build completed successfully for `vertex`,
`boundary_workflow_tests`, `boundary_canvas_tests`,
`wall_measurement_desktop_tests` and `appraisal_desktop_workflow_tests`.

All following native Qt checks ran with `QT_QPA_PLATFORM=offscreen` and exited 0:

- `boundary_workflow_tests --inline-drawing-only`: five workflows covering
  physical wall settings, imperial/metric input, fractions, exact connected
  endpoints, closure, receipt persistence, invalid input, keypad buttons,
  retained direction, unit/source/selection changes, local history, Define
  First dimensions, wall undo/redo and save/reopen.
- Full `boundary_workflow_tests`: existing pointer, authoring, editing,
  history and persistence workflows, plus the new inline cases.
- Full `boundary_canvas_tests`: adaptive imperial/metric grid paint/snap,
  endpoint precedence, exact geometry preservation and existing interactions.
- Full `wall_measurement_desktop_tests`: physical wall authoring and appraisal
  measurement workflows.
- `appraisal_desktop_workflow_tests --area-properties-only`: compact appraisal
  properties, structured facts, invalid input, history and persistence.

The independent read-only review found a held-Enter focus transition that
could finish an unfinished outline. Its regression failed before the fix with
“a repeated Enter after edge entry must not commit an unfinished outline”.
The canvas now ignores repeated Enter, Escape and D one-shot actions. The
post-fix native check verifies held Enter and Escape preserve draft geometry
and saved document state. The initial new-feature regression also failed on
the original build because an anchored wall had no inline input control.

Native screenshots were saved under `artifacts/inline-drawing-20261002/`.
The expanded imperial keypad and compact qualified appraisal properties were
visually inspected, including enlarged direction targets and the Properties
close icon. The control follows the application palette and is excluded from
canvas print/export rendering.

User checklist tasks U312–U314 describe these interactions. APX-KEY-001 and
UX-INPUT-001 remain in progress: these mouse/keyboard-driven Qt fixtures do
not certify physical pen/touch hardware, complete Apex keyboard parity or the
full production release.
