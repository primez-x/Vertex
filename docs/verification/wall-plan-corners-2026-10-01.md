# Physical wall corners in plan drawings

Two eligible straight endpoint walls now derive thickness-aware face
intersections. A shared document projection supplies complete closed physical
wall polygons and a separate visible-edge path. Canvas picking retains the
polygons; screen, overview map and fitted print/PDF/image rendering omit internal
corner caps. Stored baselines, entered lengths, opening stations, relationships,
quantities and source-derived appraisal boundaries are not changed by projection.

The projection separates property/building/floor/layer/phase and elevation,
uses hidden hosted openings, and admits only unambiguous endpoint pairs.
Flush or full wall cuts prevent a false join. Unsafe local miters retain
original caps, and the document wrapper prunes unsupported pairs symmetrically.
The finite link set only shrinks, so fallback propagation terminates. Numerical
guards cover near-parallel/acute miters, short remaining runs and unrepresentable
coordinates. Curves and multi-way junctions retain their original geometry.

Source/candidate preview comparison includes corner geometry, so a neighbor can
redraw when its stored baseline stays fixed but a connecting wall changes angle.
Opening-width previews retain surrounding joins and update affected neighboring
walls when resizing leaves a host tail too short for a safe miter. A mouse-driven
regression reproduced the missing neighbor override before the fix, then passed;
cancelling preserves the document and removes all preview overrides.
Conventional and full-depth
horizontal named plans share the analytical paths, with view-frame transforms
and crop applied to both polygons and visible strokes.

Tests first failed because the joined geometry API was absent. Root verification
then caught incorrectly assigned endpoints on the reversed right-hand face;
correcting the start/end indices restored exact closed footprints. The final
focused checks passed: hosted_opening_geometry, document_wall_plan,
wall_corner_canvas, connected_wall_canvas, wall_chain_connection,
wall_measurement, wall_measurement_desktop, hosted_opening_resize_desktop and
named_plan_vertex_desktop. Following the overview-map change, the two affected
canvas checks were rerun. Rendered canvas and output captures are retained under
artifacts/wall-corners-20261001. A pixel check confirms no internal seam at the
corner and proves the fixture detects that seam when the override is removed.
Independent source review examined document grouping and fallback propagation;
test results, rather than that review alone, establish the covered behavior.

Release executable SHA-256:
E4B185EE75FC01A781C493BBEA51730DB9F89BC0C5CCA466F364DF3557882688.
User checklist task U292 covers a corner with differing thicknesses, a hosted
window, wall movement and matching PDF output. Its user result remains untested.

Remaining production gaps include curved/multi-way joints, coordinated 3D
corner solids, oblique/depth-clipped solid views and DXF/IFC joined depiction.
DXF native host-block admission currently validates each detached wall/opening
graph; changing its depiction requires a versioned compatibility implementation
instead of silently invalidating existing native metadata. These gaps and the
existing Apex/device/install/licensing qualifications remain required by the
full production goal. This checkpoint does not certify a complete replacement.
