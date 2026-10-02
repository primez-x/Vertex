# Analytical curved-wall exterior measurement checkpoint

Scope: automatic exterior measurement from straight and circular physical walls,
analytical thickness offsets, area/perimeter, associated edge measurements,
appraisal source currentness and refresh. The full production replacement goal
remains open; this checkpoint does not certify Apex native files or caller/device
compatibility, physical printing, clean-machine installation or performance.

## Regression evidence

Root built the new core regression against the original straight-only engine.
The build passed and `wall_measurement_tests` failed with:
`Curved source walls are not supported for exterior measurements`.
This confirms the new analytical shell test exercises an implementation gap.

The core fixture checks a rounded footprint against independently known area
and perimeter, partition exclusion, reversed/shuffled sources, concave curvature
and stale source detection with fixed chord endpoints. Additional Release checks
passed for a complete circle of four quarter-arc walls (known area/perimeter)
and adjacent nonconcentric arcs with different wall thicknesses (unchanged
centers, each wall's own exterior radius, and exact straight-face offsets).
The desktop fixture uses
the real Tools review/create/refresh actions and checks curves in the preview
and persisted geometry, qualified quantities, associated dimensions, undo,
native save/reopen and actual PDF text.

## Integrated evidence

Root observed Release builds and passing runs of `wall_measurement_tests`,
`wall_measurement_desktop_tests`, `appraisal_document_tests` and
`appraisal_report_desktop_tests`. Desktop controls were exercised offscreen with
the bundled font. Logs and captures are retained under
`artifacts/curved-wall-measurement-20261001`. Root inspected the native exterior
review and refreshed measured plan. This is technical verification, not a claim
of user-observed resolution.

Independent source review identified two required corrections: normalized circular
tangent ordering with a cyclic coincident-ray guard, and strict source collinearity
for parallel offset continuation. Both were corrected and the affected suites
passed. The tiny noncollinear-turn regression failed before the latter fix.
The major-arc fixture supplies coverage but did not reproduce the old tangent
ordering defect; review of the corrected angular representation supplies that
specific evidence. The resolution review found no further required correction.

User checklist task U309 describes the manual workflow. Native provenance remains
version 1; analytical geometry and ordinary receipts use the existing boundary
format. Unsupported or ambiguous joins refuse measurement rather than create
an approximate qualified area.

## Delivery status

The delivery record belongs at
`artifacts/curved-wall-measurement-20261001/delivery.json`; it records the actual
commit, executable hash, fresh installation and Desktop shortcut when delivery
has finished. Repository contract checks qualify their contracts only, not the
complete product. The full production goal is not complete.
