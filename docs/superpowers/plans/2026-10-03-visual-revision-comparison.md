# Visual revision comparison

REC-002 requires graphical revision comparison in addition to the existing text report and restoration workflow. The production goal remains open; this work closes the conventional 2D comparison gap without claiming full production acceptance.

Baseline: 218a5e10b4432c6d913c41f4e380f5bea4ddb3ab. The preceding turn verified that the installed build exposes appraisal totals and dimensions in Details; no additional appraisal display patch was needed. Current history comparison only displays text.

## Outcome and ownership

- Root owns this plan, CMake, packaging, PlanCanvas navigation, integration, verification, Git and installation.
- Source worker owns main_window.cpp: extract a shared snapshot-to-plan scene projector and use it for both live and historical drawing views. Do not create hidden MainWindows or temporarily rebind the live Document.
- Test worker owns revision_comparison_desktop_tests.cpp: actual native controls, geometry, annotation appearance, assets, navigation and live-state preservation.
- One read-only advisor examines consequential historical state/scene integrity.

## Implementation

- [x] Observe a native failing regression for missing graphical comparison controls before product changes. The dedicated native test exited 1 for missing revisionBeforeCanvas.
- [x] Extract the authoritative 2D scene projection. Historical caches, entities, assets, appraisal policies, visibility, annotation dependencies and diagnostics belong to the selected snapshot.
- [x] Add revisionBeforeCanvas, revisionAfterCanvas and revisionOverlayCanvas to the existing revision dialog, with side-by-side/overlay mode and a context selector drawn from the union of both historical hierarchies.
- [x] Preserve authored side-by-side appearance; use derived overlay colors and dashed removed geometry, temporary IDs and a visible Added/Removed/Changed legend. Asset-only differences must highlight affected owners.
- [x] Resolve Current head against the snapshot captured at comparison time. Fixed named revisions remain fixed.
- [x] Add finite/clamped PlanCanvas view transforms and navigation notifications for fit, zoom, mouse/touch pan and overview movement. Synchronize comparison panes without recursive notification loops.
- [x] Expose missing scope, unsupported artwork and unresolved dimensions per pane. Do not substitute current geometry or silently include unrelated floors.
- [x] Preserve text comparison and non-destructive restoration. Comparison cannot modify live history, save markers, dirty state, selection or drafts.

## Verification and delivery

- [x] Freeze all source writers before every build and ensure no task fixture remains live.
- [x] Native named/named and named/current cases: exact lines/arcs, dimension styles, additions/removals, retained raster bytes, context resolution, overlay and synchronized navigation.
- [x] Verify live state before/after closing comparison; ordinary edit and Undo still work.
- [x] Inspect native captures. Exercise existing named revisions plus affected live drawing, references, appraisal dimensions and visibility checks after shared-scene extraction.
- [x] Resolve actionable independent findings; run diff and source-kit/requirement checks. Thirteen native checks pass, the contract has no errors, 1250 tracked source-kit files are covered, and 113 runtime binaries have no unresolved imports.
- [ ] Update user-facing instructions/checklist, commit reviewed paths, push and verify remote ref. Install a matching build and verify its desktop shortcut without launching the user's application.

Saved architectural frames, linked sections/elevations and 3D comparison remain explicit production requirements; conventional plan comparison alone does not certify those paths.
