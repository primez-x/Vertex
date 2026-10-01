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

## Latest feedback recheck

The requested frame/pin retention, 45-degree snapping, Shift fine adjustment,
and live degree readout were already present in the inspected source. No new
canvas patch was needed. A bounded independent source audit found no remaining
single-object reset defect in ordinary or saved horizontal plan views.

After the integrated Release rebuild, `symbol_transform_desktop` passed in
14.60 seconds and `axis_canvas_controls` in 0.15 seconds. The complete focused
run passed 10/10 in 25.30 seconds, including curve authoring, native dialogs,
architectural adapter, document history, storage, exchange, CLI and schema checks.
Root inspected `artifacts/rotation-current-20260930-final/rotation-live-90.png`:
the pin follows the rotated side, the frame remains clear, and the live readout
shows 90.0 degrees. Those are controlled native checks, not a claim that the
user's reported gesture has been retested in their previous executable.

Current development executable: `build/windows-release/vertex.exe`, SHA-256
`7994bc257bb719e47112e607b544f7b4cb101b8a405bdd797ae013167ce39bd9`.
The earlier installed checkpoint remains separate; this recheck did not replace
that installer or certify the full production application.

## Integrated keyboard-authoring rebuild

The next integrated Release build retained the same rotation implementation.
`symbol_transform_desktop` passed in 14.74 seconds and `axis_canvas_controls`
in 0.15 seconds; ten focused checks passed in 25.24 seconds. Root inspected
`artifacts/rotation-keyboard-build-20260930-final/rotation-live-90.png`, confirming
the rotated pin, clear frame and live 90.0-degree readout.

Development executable SHA-256:
`c6a9a271853d394a8cf73e4233307251e9c2d3738466fd7e703fb611472626b3`.
Launch it with `scripts/run.ps1 -Configuration Release` to supply the process-local
DLL/plugin paths. This verification does not identify the user's previously
launched executable or replace the earlier installed checkpoint.

## Explicit-subtraction integration recheck

The integrated Release `symbol_transform_desktop` check passed in 14.11 seconds;
16 focused checks passed in 36.02 seconds. The frame/pin retention, common-angle
snaps, Shift fine adjustment, live readout and history/reopen behavior remain
covered. No additional canvas patch was needed. The inspected
`artifacts/rotation-subtract-20260930/rotation-live-90.png` retains the rotated
pin and readable 90.0-degree value.

Development executable SHA-256:
`515570e5d0b30124c02ad836c6bed917df545efd032e749f9839d9fa8925ad92`.
Use `scripts/run.ps1 -Configuration Release`. The installed checkpoint remains
separate; this is controlled offscreen verification, not user-observed resolution.

## Runnable rotation delivery

The current Release executable was rebuilt from source checkpoint `f7da00b`.
The requested rotation implementation was already present; this delivery prepares
a fresh installed checkpoint rather than making another canvas patch.

- `symbol_transform_desktop` passed in 14.78 seconds and `axis_canvas_controls`
  in 0.16 seconds (2/2, 14.95 seconds total).
- The native gesture checks cover retained frames and pins, consecutive drags,
  nearby 45/90/180-degree snaps, Shift fine rotation, rendered degrees, restoration
  of a nonzero initial boundary angle, undo/redo, and save/reopen.
- Root inspected `artifacts/rotation-delivery-20260930/rotation-live-90.png`:
  the retained pin is on the rotated side and the live callout reads 90.0 degrees.
- A bounded independent source review found no remaining reset, persistence or
  shortcut conflict in these paths. Angles are measured against the view axes;
  a tilted object's starting angle need not be zero.
- Runtime import inspection found 113 declared binaries and no unresolved imports.
  The staged bundle is `artifacts/packages/vertex-offline-20260930-selection-rotation`:
  3,766 declared files, 2,645 runtime files and 1,114 source-kit files. The source
  allowlist covered all 1,114 required tracked paths at packaging time.
- Packaged executable SHA-256:
  `ebe57ce656770b32b7e04550876cf98be7b17da3a3c29c1654c22a1c2bf846bb`.
- Offline bundle installation completed and verified all 2,645 runtime files.
  Launch `artifacts/installed/vertex-20260930-selection-rotation/bin/vertex.exe`
  directly; it carries its own DLL and plugin paths. Its executable hash matches
  the packaged hash above.
- The installed-runtime smoke passed all six source/reopen runs across Measurement
  and residential/light-commercial Architectural workspaces, with developer
  search paths removed. Evidence:
  `artifacts/rotation-delivery-20260930/installed/run-20260930-220738-ad29a767/report.json`.
  This is development-host execution, not a clean-machine or network-denial test.

Native gesture verification is controlled desktop evidence, not confirmation of
the user's exact gesture in their previously launched executable. The full
production and compatibility acceptance gates remain open.
