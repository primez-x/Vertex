# Analytical architectural projections

`project_building_view` derives architectural presentation geometry from the
same semantic building objects that produce the OCCT solids. Room volumes use
the same boundary, hole, height, and elevation model as native 3D. The companion
`project_shape_view` accepts a validated derived wall or slab shape so hosted
openings and slab holes use the same projection path. Both APIs accept a
validated model frame in metres: `direction` points from the viewer toward the
model and `up` defines paper-up. The frame must contain finite, unit,
orthogonal vectors, so a persisted `CoordinatedView` can be converted without
guessing screen orientation.

Plan and elevation use Open CASCADE hidden-line removal. The result contains
visible straight edges and analytic circular arcs in the view plane. Section
views intersect the solid with the view plane first, then extract the
intersection lines and circles. Unsupported curve types and planes that do not
intersect a solid fail closed with an error; the implementation never turns a
pixel approximation into measurement geometry.

This is the projection-engine checkpoint for coordinated views. It does not
yet claim the complete production workflow: material hatching, annotation
overlays, sheet layout editing, and final
production qualification remain open. The desktop Architectural tab now has a transient
Plan/Elevation/Section selector. Non-plan views show derived wall, slab,
room-volume, and building-object edges on the canvas and route selected-workspace
draft output
through that canvas; placed assembly instances use transformed host solids in
all three views, and the section plane is the frame origin plane. Persisted
coordinated-view frames and section cut depth now drive the selector when a
matching typed sheet/view entity is present; the built-in 1.2 m frame remains a
safe fallback. A conservative object-level far-depth filter now culls solids
whose BRep bounding range lies wholly beyond the persisted limit. Objects that
cross a finite far plane are clipped with an OCCT half-space before hidden-line
or section projection; objects wholly before the plane reuse their source solid.
Material hatching, annotation overlays, sheet layout editing, and final
production qualification remain open.

Focused coverage includes a rotated-frame elevation, a horizontal circular
section that retains four analytic quarter arcs, a vertical rectangular
section, invalid-frame rejection, a no-intersection diagnostic, far-depth
culling, and exact volume reduction for a crossing solid. Existing plan
projection coverage remains unchanged.
