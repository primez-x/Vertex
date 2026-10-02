# Useful drawing lengths and right-button canvas navigation

Outcome: mouse-drawn walls and measured edges settle on useful length increments
relative to their current start point, selected units and zoom. A diagonal length
must not retain arbitrary pixel-derived decimal inches merely because its world
coordinates lie on grid intersections. Preview and committed geometry agree.
Right-button dragging pans the canvas. A stationary right-click cancels a pending
drawing or new symbol placement; otherwise it opens the relevant context menu.

User steering: wider views use whole, half or quarter feet and whole inches;
close views use fractional inches. Metric follows equivalent common increments.
Typed dimensions remain exact. This adds relative measurement magnetism to the
existing zoom-aware world grid rather than changing stored measurements.

Ownership: drawing worker owns PlanCanvas, drawing input/preview plumbing in
MainWindow and focused boundary-canvas regressions. The already frozen mixed-wall
changes in MainWindow must be preserved. Root owns documentation, integration,
all builds/checks, Git and matching offline installation. Freeze before building.

- [x] Use a bounded common-increment ladder tied to zoom and units for active
  mouse drawing. Keep raw navigation, placement and editing paths distinct.
- [x] Preserve exact existing endpoints, closure and compatible alignment snaps.
  Mouse preview and click use the same effective endpoint; typed input bypasses
  quantization. Snap-off deliberately restores free placement.
- [x] Display snapped imperial drawing lengths with feet/inches/fractions and
  readable metric lengths. Do not round away actual precision in retained data.
- [x] Pan on right-button drag with the ordinary gesture threshold; retain
  selection and active draft. Do not also open a menu, draw or move an object.
  A stationary right-click cancels pending drawing/placement, preserving already
  committed walls and symbols. Idle right-click retains the context menu.
- [x] Verify off-grid origins, diagonals, zoom/unit changes, object/closure snaps,
  preview/commit agreement, typed exact input, Snap-off and right-drag navigation.
  Inspect captures, update user checklist and prepare matching offline delivery.

The full production goal remains active; this interaction checkpoint is not
Apex compatibility or full release certification.

Delivery evidence: the final commit/push and installed executable are recorded
under artifacts/mixed-wall-measurement-20261002/delivery.json. The source plan
checklist records the implemented and locally verified checkpoint; it is not
production acceptance or a user testing result.
