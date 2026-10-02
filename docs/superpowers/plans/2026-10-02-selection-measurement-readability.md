# Keep measurements readable during selection

Outcome: selecting a wall, measured boundary or symbol leaves its nearby numeric
measurements legible. The frame, rotation control and size badge reserve the
resolved annotation footprints. Baseline and exterior values remain distinct
measurements; neither is hidden or substituted to simplify the picture.

Canvas worker owns plan_canvas.cpp/.hpp and boundary_canvas_tests.cpp. Desktop
acceptance worker owns wall_dimension_desktop_tests.cpp. Root owns integration,
documentation, builds, repository contracts, Git and delivery.

Existing automatic label placement already separates wall and boundary labels.
The observed obstruction comes from selection controls painted afterward. Keep
manual and automatic annotation positions unchanged. Clip frame/connector strokes
around rotated text bounds; move the rotation pin along the existing transformed
normal and place the size badge at a clear viewport-contained candidate.
Drawing and hit testing must use the same resolved pin location and touch target.
Preserve transform orientation, snap behavior, input gestures and useful controls.
PDF and printed output continue to omit ephemeral selection overlays.

Verification: failing rendered regression first; straight and curved measurements,
rotated text, zoom/DPI, canvas edges and live previews; drag the displaced visible
pin and verify the actual transform. Check unchanged document data, manual
annotation style/position and numeric output through native selection, save/reopen
and PDF. Inspect the real selected appraisal drawing capture. Run affected checks
and repository contracts, then commit, push and verify the remote ref. Preserve
unrelated desktop_smoke.cpp and temp.txt. Full production acceptance stays open.
