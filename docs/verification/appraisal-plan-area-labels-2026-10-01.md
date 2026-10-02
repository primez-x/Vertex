# Appraisal square footage on plans

This checkpoint implements automatic net-area labels in qualified Appraisal
plans. It is not full Apex or production qualification.

## Product behavior

Meaningful names appear above net area and units; generic names can display
the value alone. The values come from one shared qualified report per property
and refreshed scene, using the complete semantic phase, current workspace
units and saved precision. Ordinary visibility filters do not change totals.
There is no separate persisted numeric annotation truth or format migration.

Selected areas expose a direct Name field in Area attributes. Saving changes
only the trimmed name, removing an empty name and skipping unchanged values.
Names participate in history; stale and read-only edits are refused.

An internal garage has its own contribution while reducing its parent's net
area. Linked voids and independent site outlines receive no standalone building
area value. Unqualified or malformed reports retain meaningful names without
numerical assertions. The inspector supplies the reasons.

Net labels avoid their own deducted footprints, including hidden deductions.
Multiline layout measures the complete block for drawing and hit testing.
Plan-only annotations are excluded from elevation/section sheet viewports.
Output label fonts disable Inter punctuation alternates that substituted
private-use glyphs in PDF extraction.

## Evidence

Baseline native tests failed for the missing direct Name control, missing automatic square footage, insufficient
multiline label height, and private-use PDF punctuation. A separate PDF review
found each plan area value three times because it leaked into elevation and
section viewports. Those paths are corrected.

- Release application and affected native targets built successfully.
- `--plan-area-labels-only` passed in 2.89 seconds. It exercises actual Name
  field edits, clearing, no-ops, stale/read-only refusal and name history, generic/named
  labels, a 100 ft² parent minus a 25 ft² garage, void/site exclusion, layer
  hiding, metric conversion, a real vertex preview/cancel, a measured edge
  edit, dependent parent refresh, Undo/Redo, save/reopen and actual plan PDF.
- The connected edge edit retains the complementary 5-ft edge and changes the
  anchored edge to 4 ft: the resulting 5-ft-high trapezoid is 22.5 ft², leaving
  the parent at 77.5 ft². The test does not assume a rectangle is preserved.
- Native precision coverage uses two readable 151.51 ft² rooms: each displays
  152 ft² at zero decimals but the exact aggregate displays 303 ft². The core
  suite retains the smaller independent 1.51 ft² rounding fixture.
- Focused canvas output checks passed after both baseline failures, including
  actual second-line glyph hit testing and QPdf extraction of standard
  punctuation, units and both lines.
- Final integrated checks passed 7/7 in 43.98 seconds: drawing-set output,
  complete Appraisal desktop workflow, symbol transforms, named-plan vertex
  editing, the complete boundary-canvas suite, requirement schema and source-kit coverage.
- Native PDF extraction requires each net value exactly once. Independent
  pypdf inspection also counts one occurrence each. That reader substitutes
  some units/punctuation; its check is numerical occurrence evidence only,
  not proof of complete consumer text compatibility.
- Root inspected the Poppler rendering: names and values are complete, the
  parent is placed outside the deducted garage, and non-plan viewports contain
  no plan-only labels. The small default sheet fixture is not report-layout
  design qualification.

Evidence: `artifacts/plan-area-labels-20261001/canvas` and
`artifacts/plan-area-labels-20261001/plan-name-final`, including the native PDF
and Poppler image. Earlier independent text inspection is retained in
`artifacts/plan-area-labels-20261001/plan-final/independent-plan-text.json`. Manual task U283 covers the
same user workflow.

Release executable SHA-256:
`559f451eb378ab0b738eca75e7f1e756b8c926a48d584ee970efcba716824a40`.
Launch the working build with `scripts/run.ps1 -Configuration Release`.
The previous installed rotation checkpoint is separate and was not repackaged.

## Remaining limits

At this checkpoint, vertex previews suppressed automatic numbers until commit
or cancel. The subsequent
[live-preview checkpoint](live-appraisal-preview-2026-10-01.md) implements and
checks candidate net-area recomputation, including unchanged deduction parents.
Existing placement can omit text when no readable position fits; very small
or furnished areas need further placement/override work. Physical printing,
external-reader text fidelity and full production/compatibility gates remain
open. These controlled native checks do not claim user-observed resolution.

The subsequent [label-placement checkpoint](plan-label-placement-2026-10-01.md)
addresses the omitted-label limit with exterior leaders and persisted manual
placement. The limits above record this checkpoint's original evidence.
