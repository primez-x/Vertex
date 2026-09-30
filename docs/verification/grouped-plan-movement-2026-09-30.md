# Grouped plan movement: scoped verification

The architectural transform adapter now rebases retained wall-length receipts
and supported version 1/2 curve-input endpoints during rigid transforms. Exact
entered expressions, local construction values, and unknown metadata remain
unchanged. Unsupported provenance or scaling that would invalidate retained
measurements rejects before publication.

Canvas translation combines compatible architectural roots, annotation instances,
and reference images into one admitted Document command. Source or selection
replacement cancels a captured mouse gesture before release.

## Evidence

- Baseline native `--wall-group-move-only` failed because the stored wall-length
  receipt baseline did not follow the moved wall.
- Release product and the affected core/canvas/native test targets built successfully.
- CTest: `architectural_document_adapter`, `boundary_canvas`,
  `axis_canvas_controls`, `symbol_transform_desktop`, and
  `requirement_schema_contract`: 5/5 passed (12.49 seconds).
- Native `--wall-group-move-only`: exit 0. Real mouse drags cover grouped measured
  walls, mixed wall/symbol/label/reference movement, single-step history,
  undo/redo, save/reopen, partial constrained-group rejection, Escape, and stale
  source cancellation. Core checks also cover curved provenance and scale refusal.
- Captures: `artifacts/group-move/release/wall-group-move.png` and
  `artifacts/group-move/release/mixed-object-group-move.png`; the mixed capture
  was inspected for visible artwork, selected geometry, and dimensions.
- Independent scoped source review found no required correction.

The current main-canvas rotation check also passed in a separate captured run
(10.39 seconds). `artifacts/rotation-request-final/rotation-live-90.png` was
visually inspected: the outward rotation pin follows the 90-degree frame and
the live readout displays 90.0 degrees. The native checks include repeated
rotation, 45-degree snapping, Shift fine adjustment, return to zero, history,
and reopening. A subsequent independent review identified a separate named-plan
frame propagation gap; its correction and evidence are recorded in
`selection-rotation-2026-09-30.md`.

The writable development build rejects its reference decoder module roots
(`invalid_module_roots`). The native mixed-selection check asserts that rejection,
then uses the established trusted local reference-editing fixture to test movement.
This result proves downstream editing, not real reference import. Real importer
acceptance requires the protected installed runtime and its separate decoder check.

Mixed measurement-boundary groups, dependent dimension groups, full Apex
compatibility, physical-device behavior, and full installed production qualification
remain outside this scoped evidence. The overall production goal remains active.
