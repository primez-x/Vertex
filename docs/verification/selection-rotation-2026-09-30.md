# Persistent selection rotation

The selection frame and outward rotation pin retain the object's committed
orientation. Dragging snaps every 45 degrees; Shift allows fine adjustment,
and the live readout displays degrees. Returning to zero restores an initially
unrotated object. Angles differing by a complete turn are geometrically equivalent.

The audit found that saved named-plan projections bypassed frame attachment.
Their frames now carry the canonical object's center and orientation, mapped
through the horizontal view basis. Commit pivots map back to model coordinates;
upward views reverse rotation handedness. Frame attachment also survives an
unselected refresh and reopening. The shared frame cache stays in model space.

Annotation rotation and independent-axis resizing copy the original owner and
replace only its annotation state, preserving the required flag and extensions.
The strict annotation schema continues to reject unsupported property fields.

## Verification

- New named-plan native regression failed against the baseline:
  `named plan refresh lost the selected oriented frame or rotation pin`.
- Release `vertex`, `desktop_smoke`, and `symbol_transform_desktop_tests` built.
- Final native `symbol_transform_desktop`: PASS, 12.70 seconds. Covers repeated
  90/180/reset rotations, standard named plans, translated/30-degree-rotated
  horizontal views with both upward/downward normals, Shift fine adjustment,
  pivot preservation, one command per gesture, undo/redo, save/reopen, and a
  post-reopen rotation. Valid annotation owner metadata survives these edits.
- Companion checks `axis_canvas_controls`, `boundary_canvas`,
  `architectural_document_adapter`, and `requirement_schema_contract` passed.
  The final combined CTest run passed all five checks in 13.51 seconds.
- Final native `--wall-group-move-only`: exit 0 after the integrated product rebuild.
- `artifacts/rotation-named-view-final/rotation-live-90.png` was visually
  inspected: the pin follows the rotated frame and the readout shows 90.0 degrees.
- Independent source review's frame propagation and annotation metadata findings
  were resolved. Root integrated review also corrected missing unselected frames.

Named-view snapping and degrees are relative to the view axes. Oblique-plane
rotation/resize remains unsupported; it is explicitly diagnosed rather than
committing projected coordinates as model coordinates. These are native desktop
development-build checks, not physical-input or full production qualification.

## Nearby-angle regression follow-up

The native gesture helper now locates a diagonal pin from its retained local
frame instead of inverting a screen bounding box, which is singular at 45
degrees. Actual pointer angles 43.5, 88.5 and 178.5 degrees commit 45, 90 and
180 degrees. The next drag begins at the committed 45-degree pin, including
after save/reopen. Rendered-widget checks find the actual live degree tokens
for those angles and for a Shift-modified 23.5-degree gesture.

Release `symbol_transform_desktop` passed in 15.05 seconds. The final combined
run with `measurement_group_move` and `coordinated_view_output` passed 3/3 in
37.26 seconds. Root visually inspected the captured 90-degree callout and pin.

## Current-build recheck

The focused Release `symbol_transform_desktop` check passed again in 17.53
seconds at source checkpoint `c2b41ed`. It exercises consecutive committed
drags, nearby 45/90/180-degree snapping, Shift-modified fine adjustment, the
rendered live angle, history navigation, and save/reopen. No new rotation
product patch was required by this recheck.

The installed build is `artifacts/installed/vertex-20260930-curve-length/bin/vertex.exe`
from source checkpoint `04bd847`, which includes the rotation changes. Its
SHA-256 is `33e43add1b294f6391ea8b0481d3bfe098c368822d4d05a2ddf2412b488836cf`.
This focused recheck used the native development test executable; it does not
establish which build a user has launched or constitute a user-observed fix.
The manual symbol check now distinguishes returning to zero from returning to
a nonzero starting angle.
