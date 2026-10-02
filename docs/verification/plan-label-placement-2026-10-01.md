# Plan label placement checkpoint

Calculated area and room labels retain their text when no interior footprint
fits. Automatic placement searches outside the owner and adds a subordinate
leader. Other area labels, components and deductions participate in spacing.
The quick-properties Place label and Automatic controls save or remove a
model-space offset. A stationary click places; dragging still pans; Escape
cancels. Geometry, appraisal facts and numerical calculation truth are separate
from this presentation change.

Annotation state v4 admits finite area-label offsets and placement-only
appearance inheritance. Normal annotation states continue to encode as v3.
Raw unrelated metadata, explicit styles and pinned SVG records are preserved.
Project format documentation describes both optional fields.

## Verification

- Annotation catalog and entity codec checks pass for v4 round-trip,
  malformed/type/version refusal, saved project reopening and pinned artwork.
- The placement workflow passes with actual buttons and pointer gestures,
  panning while armed, cancellation, stale/read-only refusal, Undo/Redo,
  Automatic, native rooms, updated quantities and saved manual positions.
- Candidate repair of an invalid deduction gains qualified quantities before
  commit, including previously anonymous areas with no retained label. A saved
  offset survives withheld qualification; preview positions match commit.
- Full Appraisal, boundary-workflow and boundary-canvas suites pass, including
  retention of a 500-character no-fit room name. Canvas coverage verifies
  first-quantity rendering, hidden-owner exclusion, cancellation and unchanged
  committed output during the preview.
- Named horizontal plans cover rotated/reflected frames, click inversion,
  projected leaders and unchanged owner geometry.
- Root inspected the rendered manual-position canvas: the complete quantity
  sits above the crowded area with a visible leader and an unfilled background.
  The native PDF retains its text and placement. The tiny fixture at the default
  sheet scale is retention evidence, not production report-layout qualification.
- Independent read-only review found no required correction in the final
  candidate-label authority, visibility, offsets or output-isolation paths.

Artifacts are in `artifacts/plan-label-placement-20261001`. Historical failing
checks remain alongside passing checks; they are not omitted from the record.
Manual task U302 covers user verification.

The adaptive-grid checks also pass explicit half-foot and centimetre intervals,
paint/snap agreement, world anchoring and unchanged exact geometry. U081/U082
cover Imperial/Metric grid and snap behavior at different zoom levels.

Release executable SHA-256:
`4cb0c0ac22cc50f59635141aab9ae088498e7372d1fdd7634e11f6a0befecd81`.

Physical printing, user-observed resolution and full production/Apex
compatibility certification remain open. This checkpoint does not complete
the production release goal.
