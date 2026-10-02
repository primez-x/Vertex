# Independent wall measurement presentation checkpoint

Double-click a wall, or use its Wall measurement context command, to edit the
derived callout's position, paper text height, color, emphasis, visibility and
rotation. Place measurement accepts a stationary canvas click. Dragging pans;
Escape cancels. Automatic clears the manual offset while retaining appearance
and visibility. A hidden measurement remains accessible through the wall.

The displayed number comes from the analytical baseline. Presentation edits do
not change wall geometry, openings or appraisal calculations. Annotation v6
stores an independent wall-dimension record; raw edits retain unrelated labels,
area overrides, pinned SVGs and opaque metadata. Inserting model-plan text into
an existing v6 owner no longer downgrades its annotation version.

Horizontal named plans project the midpoint-relative anchor and leader, and
inverse-project placement clicks. Automatic text remains upright after rotated
or reflected projection; explicitly authored world angles project faithfully.
Geometry previews and committed canvas/output share the same derivation.

## Verification

- Release builds succeeded for Vertex and the affected desktop executables.
- Annotation catalog and entity codec checks pass v6 round-trip, mixed earlier
  optional records, malformed fields, limits, version refusal and native reopen.
- `wall_dimension_desktop_tests` passes actual quick-properties controls,
  placement, pan/cancel, hidden restoration, automatic positioning, undo/redo,
  derived length changes, save/reopen, stale/read-only refusal and raw/pinned
  sibling preservation. PDF output retains the edited callout and omits it when
  hidden without omitting the wall or hosted opening.
- Full named-plan-vertex, text-library-desktop, boundary-canvas,
  boundary-workflow, appraisal-desktop-workflow and symbol-transform-desktop
  checks pass. Named-plan joined-corner previews retain styled manual wall
  callouts and match committed positions, leaders, typography and rotation.
- Root inspected the actual quick-properties controls and rendered PDF. The
  controls now have usable minimum height and scroll into view from the wall
  context command. Independent source review identified the projected upright
  angle issue; the fix distinguishes automatic and explicit rotations.

Evidence is in `artifacts/wall-measurement-20261001`: final desktop captures in
`pass-4`, individual core logs and full regression results in `regressions`.
Historical failed reopen assertions assumed Metric in a fresh window whose
default was Imperial; the fixture now selects Metric explicitly and still
asserts exact saved entities. Earlier control captures exposed compressed
buttons, which required the minimum layout-size fix.

Release executable SHA-256:
`76e27a10d76d3913d7e38ebf57084868dafdfdd4420265eb1f94dfdde8e9906f`.

Manual tasks U132–U134 include the concrete wall-measurement operations.
Adaptive Imperial/Metric grid behavior remains covered by U081/U082 and the
boundary-canvas checks, including half-foot and centimetre intervals. Grid
navigation does not quantize existing geometry or put the editing grid on output.

This closes the automatic-wall presentation checkpoint. Sparse default-sheet
layout, appraisal report ergonomics, physical printing, user-observed resolution,
broader production qualification and Apex/device compatibility remain open.
This checkpoint does not certify the full production release or complete its goal.
