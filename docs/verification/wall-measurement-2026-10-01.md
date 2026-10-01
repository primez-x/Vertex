# Exterior wall measurements and appraisal totals

This change connects closed physical wall layouts to distinct measurement
boundaries. It is an implementation checkpoint; the full production and Apex
compatibility acceptance gate remains open.

## User workflow

Select a wall in an unbranched closed loop, or Ctrl-select the perimeter walls
in a branched layout. Choose **Measure exterior from walls…** from its context
menu, Tools or command search. Review the exterior outline, wall count and area;
canceling changes nothing. Creation includes stable, bound exterior edge
dimensions through the existing point-native authoring and receipt system.
Repeat creation from the same sources to reuse the existing measured owner.

The outline uses each wall's actual thickness, with outward half-thickness
offsets and mitered corners. A 4 × 3 m baseline with 0.2 m walls has an exterior
area of 13.44 m². Room boundaries and wall objects remain independent. Door and
window cuts do not subtract from the exterior footprint.

The existing Appraisal workflow derives categories from declared property,
floor and area facts. Net area and square-foot conversion use the existing
calculation engine, including linked deductions. Source wall changes or semantic
phase exclusions withhold qualified totals until the source is valid. Select
the derived area and choose **Refresh exterior measurement…** to review and
apply updated geometry. The typed redefinition command preserves its identity,
facts, name, appearance, deduction links and surviving dimension bindings.
Unavailable physical values display a dash rather than a fabricated zero.

## Verification

- Missing core derivation and native creation failed before implementation.
- A separate missing exterior-dimension assertion failed before dimensions were
  added using the existing authoring session.
- Release application and focused native targets built successfully.
- Core geometry checks cover a rectangle, shuffled/reversed walls, stable edge
  ordering across coordinate edits, concave L geometry, different wall widths,
  collinear continuation and invalid loops. Review corrected source-to-edge
  thickness mapping and unsafe optional initialization before delivery.
- Appraisal checks cover stale thickness/baseline changes, missing walls,
  malformed sources, manual outline edits, source context and semantic phase
  exclusion. Hosted opening edits leave the exterior footprint current.
- Native checks exercise actual review actions, cancellation, single-wall and
  multiple-wall source selection, one-command creation, reuse, qualified net
  area with a garage deduction, metric/imperial conversion, name/appearance,
  stale revision refusal, refresh, bound dimensions, undo/redo, save/reopen,
  read-only/open-loop refusal and actual PDF creation/text extraction.
- The separate core build without Qt passes its wall-measurement and appraisal
  checks. The wall drawing/opening regression remains passing.
- Root inspected the rendered review and refreshed plan. Exterior dimensions
  are outside the outline and the plan shows its net appraisal area.

Captures: `artifacts/wall-measurement-20261001/exterior-measurement-review.png`
and `refreshed-exterior-appraisal-area.png`. PDF checks use a temporary file;
they do not qualify physical printing or every external PDF reader.

Release executable SHA-256:
`04DC3DAF2F77B26D8A64D4752F72633143BC060E8FDD3056A3A7F8197B82DE33`.

Manual tasks U284 and U285 cover the user-facing workflow.

## Remaining production gaps

Version 1 supports one simple closed straight-wall loop. Curved exterior
offsets, disconnected/multiple outlines, branched automatic shell inference
and degenerate variable-thickness collinear transitions remain work to do;
unsupported geometry is explicitly refused. Derived boundaries refresh through
the user command; arbitrary source edits do not silently reshape declared
appraisal areas. Large-loop responsiveness and physical output need production
evidence. These gaps remain in the full application scope.
