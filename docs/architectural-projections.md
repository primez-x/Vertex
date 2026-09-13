# Analytical architectural projections

`project_building_view` derives architectural presentation geometry from the
same semantic building objects that produce the OCCT solids. The companion
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
yet claim the complete production workflow: exact cut/far-depth clipping,
material hatching, annotation overlays, sheet layout editing, and final
production qualification remain open. The desktop Architectural tab now has a transient
Plan/Elevation/Section selector. Non-plan views show derived wall, slab, and
building-object edges on the canvas and route selected-workspace draft output
through that canvas; the section plane is the frame origin plane. Persisted
coordinated-view frames and section cut depth now drive the selector when a
matching typed sheet/view entity is present; the built-in 1.2 m frame remains a
safe fallback. A conservative object-level far-depth filter now culls solids
whose BRep bounding range lies wholly beyond the persisted limit; objects that
cross the limit remain visible until a future exact clipping pass. Material
hatching, annotation overlays, sheet layout editing, and final production
qualification remain open.

Focused coverage includes a rotated-frame elevation, a horizontal circular
section that retains four analytic quarter arcs, a vertical rectangular
section, invalid-frame rejection, a no-intersection diagnostic, and
conservative far-depth culling. Existing plan projection coverage remains
unchanged.
