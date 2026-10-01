# Straight wall partition junctions

Straight T-junctions, crossing walls, and three-way endpoints now omit buried
plan strokes while preserving each wall's complete material polygons, stored
baseline, dimensions, openings, and calculation inputs. Screen rendering,
overview rendering, previews, and printable plans use the same derived strokes.
This extends the previous guarded two-wall corner implementation.

Contact detection requires the same document placement context and elevation.
Analytical clipping uses actual convex wall material polygons and retains
opening voids. Bounding boxes only prune candidate contacts. Nearby partners
of an admitted miter participate when their material bounds overlap, preventing
a partition near a corner from retaining strokes buried in the adjacent wall.
Unsupported geometry remains unchanged. An unrepresentable clipped fragment
preserves that connected contact component without suppressing disconnected
ordinary junctions. Display clipping never rewrites document geometry.

The native wall-join command now admits real T/X baseline contact and continues
to require physical solid contact. It rejects vertically separated walls and
full-height openings that disconnect the solids. Both source walls remain
authoritative editable records.

## Verification performed

The Release desktop, CLI, and import worker built successfully. Ten focused
CTest cases passed in 26.28 seconds: wall_plan_junctions, document_wall_plan,
wall_join, wall_corner_canvas, connected_wall_canvas,
hosted_opening_resize_desktop, wall_chain_connection,
wall_measurement_desktop, architectural_document_adapter, and
named_plan_vertex_desktop.

Coverage includes exact straight T/X/three-way outlines, unequal thickness,
reversed direction, opening voids, separate floors, source preservation,
physical picking, printable pixel output, and the desktop T-wall join command.
Native fused-volume checks verify the actual overlapping solids. Translated
and rotated cases check model precision; a deliberately unrepresentable large
coordinate case checks conservative fallback.

Independent review found two required corrections: a partition close to an
admitted miter retained buried strokes, and a precision failure suppressed
disconnected contacts. Both regressions failed before their fixes and passed
afterward. The reviewer accepted the bounded fixes; root reran the final ten
cases after the final context/elevation guard.

Release vertex.exe SHA-256:
`855839426AA0CD570DFF2ACD53301D7DB0BDE51FD5FC772B6109525ED31A3983`.

## Remaining qualification

Automated evidence above does not establish user-observed resolution or full
application acceptance. Real mouse evidence and package verification belong
with the delivered runtime artifacts. Curved junction construction, persistent
interior attachment constraints during later wall movement, joined depiction
in all interchange paths, and recognition of exterior appraisal boundaries in
partitioned networks still require work. Apex sample migration, device,
clean-machine, physical printing, and broader production qualification remain
open. This is a development checkpoint within the active production goal.
