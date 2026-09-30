# Corner editing in saved horizontal plans

Saved horizontal architectural plans retain identified measurement corner handles.
Pointer targets map through the captured view frame into model coordinates for
both detached preview and release. The public editing API remains model-space.
Shifted origins, rotated axes and upward views preserve canonical geometry,
original construction records in edit derivations, stable identities and undo.

Preview and refreshed views share native wall/opening projection, far-depth
clipping and crops. Eligible model owners are captured independently of crop
visibility, so a hosted door moving into the view appears during the drag.
Explicit model/layer visibility and saved-view object exclusions remain binding.
Preview-only owners paint interactively and never enter committed export content.
Clipped outlines suppress corner handles; full-model totals remain authoritative.
Typed measurement boundaries do not promote vendor `holes` metadata into geometry.
Native rooms and slabs retain their existing hole semantics.

## Observed verification

- The initial native regression failed with missing named-plan corner handles.
- Release `vertex`, `desktop_smoke`, the new native target and affected canvas,
  rotation and visibility targets built successfully.
- A six-check CTest run passed in 43.09 seconds: `measurement_group_move`,
  `symbol_transform_desktop`, `named_plan_vertex_desktop`, `axis_canvas_controls`,
  `boundary_canvas` and `coordinated_view_output`.
- The expanded named-plan check passed in 8.26 seconds. Real Qt pointer events
  cover both horizontal view normals, translated/rotated frames, straight and
  curved boundaries, joined walls, hosted doors/windows, far-depth exclusion,
  cropping, labels/dimensions, exact source immutability, Escape, scene replacement,
  one commit per release, original records/IDs, undo/redo and save/reopen.
- The cropped-door test checks affected geometry in both directions, verifies
  actual interactive painting of a previously absent door, and verifies unchanged
  explicit-scale output during preview. Restricted views do not reveal the host
  or its opening while the joined model edit is applied.
- `visibility_workflow` passed in 8.06 seconds after replacing obsolete baseline
  stroke assertions with physical wall extent, thickness and opening-gap checks.
  Direct/shared document-head refresh and same-revision replacement remain checked.
- Native smoke selectors `--mixed-constraint-workspace-only`,
  `--boundary-insertion-only` and `--boundary-geometry-preview-only` exited zero.
- Root inspected captures under `artifacts/named-plan-corner-final-20260930/`,
  including upward depth-limited editing and the retained 90-degree rotation pin.
  Independent review's crop-entry omission was fixed and reviewed; root corrected
  test lifecycle/output calls without weakening production guards.

## Limits

Far depth is the existing one-sided clipping plane, not a new near/far slab.
Oblique-plane corner editing and curved-owner relationship solving remain open.
These focused native checks do not certify full Apex compatibility, physical
input, clean-machine installation or the complete production release.
