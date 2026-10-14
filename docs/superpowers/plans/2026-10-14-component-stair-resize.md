# Component and straight-stair resize plan

**Goal:** Resize independent components and ordinary straight stairs with top
landings in one source-bound side gesture and one Undo.

**Spec:** [Component and stair resizing](../../design/vertex-component-stair-resize.md).

**Architecture:** Native detached raw-preserving stair stage, internally captured
full typed replay, then the existing two-candidate component finalizer once.
Closed Properties43/command52; reader266/extraction264. No phase, hosted rail/root,
connected level, missing landing or multi-flight stair authority in this slice.

## 1. Actual native RED

Native writer owns `tests/mixed_component_stair_resize_desktop_tests.cpp`; root
owns CMake and execution.

- [x] Actual independent component and v1 ordinary stair with four risers and a
  top landing; verify hand flight/tread/landing corners, endpoints, elevations,
  native solids, published ink, exact source and selection before the frame.
- [x] Root compile and run both orders: hand union `[1,7] x [0,2]`, centre `(4,1)`;
  actual missing-frame RED is recorded in this wave's `native-red.log`.
- [x] Extend native tests for both axes/orders, ordinary Plan/rotated Site,
  detached/committed output, one Undo/Redo, real review/stale authority and fresh
  applied/undone/redone editable Open.

## 2. Sealed core bridge

Core writer exclusively owns `include/sketch/plan_axis_resize.hpp`,
`src/architecture/plan_axis_resize.cpp`, mixed_selection_property_edit and
phase_constraint_authoring header/source pairs, and
`tests/mixed_component_stair_resize_tests.cpp`. Root owns all other files.

**API:** `StairDirectionalResizeRequest {stair_id,sx,sy,anchor_m,frame_rotation_radians}`;
`prepare_component_stair_directional_resize(source,component_authoring,stairs,message)`
returns `MixedSelectionPropertyEditPreparation`. Private native contributions
are not caller authority.

- [x] Add meaningful regressions first for closed wire/fences, requests7/8/9,
  anchored base movement, both factors, exact geometry/Z, raw numeric/dormant
  retention and owned/unaffected/opaque receipt policy.
- [x] Implement raw native stage using existing exact stair mathematics with
  retain_distinct_factors; internal full typed capture/replay must match exactly.
- [x] Admit units before elimination; reject phase/baseline-only scope, levels,
  transforms, missing landing, multi-flight, rails, hosted roots and unsupported
  affected dependencies/dimensions without inventing relationships from metadata.
- [x] Enforce combined 1000 members/associations, aggregate stair work100000 and
  existing 1 MiB capture bound before native calls; retain global component
  history/asset/alias freshness and unchanged stair identity inventory.
- [x] Compose original-source contributions through the unchanged finalizer once;
  preserve39..42. Verify explicit component retirement and exact final stair data.
- [x] Root builds/runs new core and historical resize neighbors after ownership
  returns. Writers run no native jobs or Git operations.

## 3. Controller, lifetime and storage

Root owns `src/desktop/main_window.cpp`, `src/desktop/plan_canvas.cpp`,
`src/core/document.cpp`, `src/core/mixed_selection_edit.cpp`, project_store header/
source, CMake and documentation/generators.

- [x] Add the complete admitted stair roster to common frame/bounds, captured
  source operators, preview/review/release dispatch and identity consumers under43.
- [x] Retain all source/presentation/viewport/font/publication and history guards;
  separately manufacture final review result, one release and fresh Open boundary.
- [x] Add canonical reader266 native/semantic floor, SQLite acceptance and
  extraction264; current/undone opaque lookalikes retain their prior floors.
- [x] Qualify native applied/undone/redone persistence and reader265 downgrade
  refusal; source-preserving unit and unsupported reference rejection remains exact.

## 4. Integrated delivery

- [x] Root reviews actual integrated behavior and proof interfaces; request one
  independent read-only challenge for consequential new stair/raw/receipt risks.
- [x] Fix concrete findings, rebuild affected/full Release and run meaningful
  focused/historical checks plus required standard/protected qualification.
- [x] Inspect actual Plan/Site captures, refresh source-kit inventory, run required
  gates and `git diff --check`; record evidence and product limitations accurately.
Root finishes this verified slice with reviewed named-path staging, commit,
push and exact remote-ref verification; the terminal SHA is recorded in the
delivery response and private checkpoint evidence.

User authorization covers execution. Preserve user untracked files, unchanged
installed candidate and all historical contracts. No new Apex compatibility is
claimed without real fixtures.
