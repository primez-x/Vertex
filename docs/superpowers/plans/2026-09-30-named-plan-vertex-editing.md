# Named plan vertex editing implementation plan

**Goal:** Edit measurement corners directly in horizontal saved architectural
plans while retaining analytical geometry, relationships and reversible history.

**Architecture:** Keep the document and public editing API in model coordinates.
Capture the saved view frame, depth, crop and eligible source templates with the
detached preview scene, inverse-map pointer targets, then project candidate
geometry and generated labels before view cropping. Share native wall/opening
projection with refreshed views, including objects moving into the view crop.
The same inverse mapping applies at final commit; previews never authorize edits.

**Technology:** Existing C++20/Qt desktop and receipt-preserving constraint commands.

**Spec:** [Production plan](../../production-plan.md), precision editing and
integrated authoring requirements. Existing view-overlay annotation conventions
remain unchanged. This does not reduce the full production acceptance scope.

## Ownership and verification

- Root owns the native regression, CMake, docs, integration, generators and Git.
- Worker owns `src/desktop/main_window.cpp` and `src/desktop/plan_canvas.cpp`;
  no concurrent writer uses them.
- Advisor challenges coordinate conversion, clipping, scene changes and preview fidelity.

- [x] Reproduce missing handles in a shifted/rotated horizontal named plan.
- [x] Project identified handles with stable IDs and revisions; retain the
  conventional rule that clipped outlines do not expose misleading model handles.
- [x] Capture frame/depth/crop with the immutable source and scene. Convert preview and
  final targets using the same model/view inverse; preserve the public model API.
- [x] Project candidate measured boundaries, related walls/openings, dimensions,
  handles and generated labels before view cropping. Reflect arc sweep for upward
  views. Preserve model-space metrics and overlay annotation conventions.
- [x] Verify both view directions, preview/commit agreement, exact receipts and
  IDs, related-wall movement, dimensions, cancellation, stale completion, crop,
  undo/redo and save/reopen with actual native canvas events.
- [x] Build, run focused regressions, inspect rendering and the integrated diff,
  resolve required review findings, and finish with scoped Git delivery.

Oblique-plane editing requires a separate coordinate mapping and remains a
visible production gap; it is not represented as certified by this work.
