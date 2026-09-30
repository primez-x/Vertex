# Live boundary preview annotations — 2026-09-30

Deferred corner previews now project persisted length, angle, and area dimensions
from the candidate geometry. The corner readout shows full boundary area and
perimeter in the active units, including when the view crops the geometry.
Area names use the same containment and component-avoidance placement as normal
document rendering. Text footprints are measured on the UI thread; the detached
worker receives values only. Saved and exported content stays unchanged until
the final command is admitted.

## Observed verification

The Release application and focused native targets compiled. CTests
`symbol_transform_desktop`, `axis_canvas_controls`, and `boundary_canvas` passed
(3/3, 12.90 seconds). Rotation coverage includes retained object-relative pins,
45-degree snapping, Shift precision, return to zero, and undo/save/reopen.

Native `desktop_smoke` selectors `--mixed-constraint-workspace-only`,
`--boundary-insertion-only`, and `--boundary-geometry-preview-only` exited zero.
The mixed workflow checks imperial and metric live dimensions, full-model totals
in a cropped view, cancellation, source immutability, release matching preview,
undo, and furnished/concave area-name placement matching normal rendering.

Captures and stdout/stderr are local under
`artifacts/vertex-preview-annotations/release/`. Visual inspection of the joined
corner capture found overlapping readouts. Bounding-box dimensions are now
suppressed while dragging a vertex; the regenerated capture shows an unobscured
coordinate/area/perimeter readout. Independent source review required sharing the
production area-name placement helper and approved that correction.

## Remaining qualification

These are focused implementation checks. They do not certify full Apex parity,
appraisal standards, installer delivery, or the production release. Curved-owner
relationship solving and large-component preview latency remain open.
