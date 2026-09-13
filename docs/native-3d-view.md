# Native architectural 3D view

`sketch::visualization::NativeModelView` is a reusable Qt Widgets control for
the native Open CASCADE viewport. It keeps the `DocumentSnapshot` as the
source of truth and owns only derived `AIS_Shape` presentations. The widget is
created without a Qt `Q_OBJECT`, so no moc step is required for the view itself.

The native OpenGL view is initialized lazily from `showEvent`. It wraps the
Qt `winId()` in an OCCT `WNT_Window`, then creates an
`OpenGl_GraphicDriver`, `V3d_Viewer`, `V3d_View`, and
`AIS_InteractiveContext`. A driver, window, or context failure leaves the
viewport empty, displays an error overlay, invokes `onError` when installed,
and makes `isReady()` false. The control never substitutes a 2D or synthetic
3D picture for a failed native view.

The `offscreen` and `minimal` Qt platforms are reported as unavailable before
native window and OpenGL construction. Initialization is attempted once per
widget, so an unsupported platform leaves one explicit error instead of
repeated retry attempts.

The first snapshot containing valid solids is fitted automatically. Later
snapshot updates retain the current camera; call `fitAll()` when an explicit
refit is wanted. OCCT keeps the current projection, eye, target, scale, and
twist while the widget is resized or its cached shapes are updated.

`setSnapshot(snapshot, visible_ids)` accepts an optional set of visible semantic
IDs. With no set, every solid is displayed; an empty set hides every solid.
This changes native presentations and picking only. All geometry is still
validated, including hidden objects, and invalid geometry still blocks export.
Cached solids can be hidden and revealed without rebuilding or changing their
coordinates. The native test checks the actual framebuffer, picking and camera
restoration at three display scales. Floor/layer controls are connected
separately; this adapter alone is not the completed visibility workflow.

Right-drag orbits, middle-drag pans, the mouse wheel zooms at the cursor, and a
left click selects the first AIS object under the pointer. The selected object
is mapped back to the stable document entity ID and delivered through
`onEntitySelected`. A click on empty space delivers an empty `QString`, which
lets the inspector clear its selection.

Ctrl+left-drag directly translates a native architectural object. The press
selects a supported column, beam, stair, railing, or roof, converts the cursor
to the current view projection plane, and previews the world-space movement on
the derived AIS presentation. Release clears the preview and emits one
`onEntityTranslationRequested` callback with the stable entity ID and the
finite X/Y/Z delta in metres. The desktop shell commits that request through
the existing typed architectural transform transaction, so the document gets
one normal undo/redo entry and the viewport is rebuilt from the authoritative
snapshot. Walls, slabs, rooms, and placed assembly children remain selectable
but are not moved by this gesture until their dedicated semantic transform
contracts are available; a drag never mutates a presentation without a
successful document command.

Placed assembly children report their synthetic child ID; the desktop shell
resolves that ID back to the persisted host before updating the inspector, so
native picking never creates a second authoritative object.

Qt mouse positions are widget-local logical coordinates with a top-left origin.
Before calling OCCT, the view multiplies them by the widget's
`devicePixelRatioF()` and clamps them to the native `WNT_Window::Size()` in
physical pixels. The resulting top-left pixel coordinates are passed directly
to `AIS_InteractiveContext::MoveTo` and the direct `V3d_View` mouse helpers.
`V3d_View::Pan` accepts a view-plane displacement instead, so the vertical
drag delta is negated when passed to that API.
The AIS selector and `V3d_View::Convert()` apply their own view-space y
conversion internally; applying a second bottom-left flip would mirror
selection and cursor-anchored navigation. The same native-pixel mapping is
used for picking, hover, orbit, pan, and zoom, including 150% and 200%
Windows display scaling.

`fitAll()` calls `V3d_View::MustBeResized()` immediately before `FitAll()` and
uses the current physical WNT client size. This refreshes the camera aspect
after a late layout or DPI resize before framing the cached solids.

`exportViewImage(path)` captures the OCCT framebuffer with
`V3d_View::Dump`; it does not call `QWidget::grab()`, because the latter cannot
capture a native OCCT surface. The method returns `false`, reports through
`lastError()`/`onError`, and keeps the error overlay visible when the destination
path or OCCT image codec is unavailable.

## Snapshot contract

The current native solid contract covers:

* A `wall` entity requires `baseline` with `start`, `end`, and
  `sweep_radians`, plus `thickness_m`, `height_m`, and `elevation_m`.
* An `opening` entity is hosted by a wall through `wall_id` and requires
  `offset_m`, `width_m`, `sill_m`, and `height_m`. Hosted openings are cut by
  `make_wall`; they do not create a second independent presentation.
* A `slab` entity requires a `boundary` segment array, a `holes` array of
  segment arrays, `thickness_m`, and `elevation_m`.
* A `room` entity requires a `boundary` (or migration-compatible `segments`)
  segment array, optional `holes`, positive `height_m`, and finite
  `elevation_m`. The room-volume kernel creates a native solid from these
  fields; it is separate from the plan-only `room_boundary` entity.
* `column`, `beam`, `stair`, `railing` and `roof` entities use the strict,
  versioned [building entity format](building-entity-format.md). Both column
  forms, straight beams, stair flights/landings, straight railings, sloped
  panels, gable roofs and hip roofs are rendered from their semantic
  parameters.
* A placed assembly instance is a derived child presentation with the stable
  ID `<catalog-id>:instance:<instance-id>`. Its host architectural solid is
  transformed by the persisted placement (uniform scale, Z rotation, and XY
  translation) before entering the same native display path. The host remains
  authoritative; the catalog and host content jointly invalidate the cache.

The wall/opening/slab/room parser accepts the corresponding unitless scalar spellings as a migration
aid (`thickness`, `height`, `elevation`, `offset`, `width`, and `sill`) but
never supplies a missing value. A malformed required field removes that
entity's stale presentation, records the reason in `lastError()`, and leaves
`isReady()` false. OCCT solid construction errors follow the same path.

Derived solids are cached per semantic entity using exact canonical content
equality for the entity ID, type, required bit, properties, extensions, and all
hosted opening content. Placed assembly children use a synthetic ID and a
canonical key containing the catalog record, resolved host record, placement,
and hosted openings. An unchanged entity reuses its existing `AIS_Shape` and
camera state; an opening edit changes its host wall and any placed children
cache keys. No hash collision can reuse a stale solid. Removed or
type-changed entities have their old AIS object removed from the context.

Known hierarchy and metadata entities (`property`, `building`, `floor`,
`layer`, `label`, `sheet`, `view`, `constraint`, `annotation`, and
`dimension`) are intentionally ignored by this solid view. Plan-only boundary
entities (`boundary`, `measurement_boundary`, and `room_boundary`) are also
ignored because they are intentionally 2D appraisal or annotation geometry.
Architectural geometry without a native solid yet (unsupported entities and
unhosted openings) remains visible as an explicit pending warning. A room with
missing or malformed volume fields is reported as an explicit geometry error.
A warning is also a non-authoritative state: `isReady()` is
false until the complete snapshot has no pending geometry or parse errors.

## Native link requirements

The viewer source uses Qt 6 `Core`, `Gui`, and `Widgets` (`Qt6::Core`,
`Qt6::Gui`, and `Qt6::Widgets`). It uses these OCCT 8.0.1 visualization
toolkits:

* `TKService` supplies `WNT_Window` and the native window services.
* `TKV3d` supplies `V3d_Viewer`, `V3d_View`, and the AIS interactive context
  implementation.
* `TKOpenGl` supplies `OpenGl_GraphicDriver`.

The shape cache calls `make_wall`, `make_slab` and the building-entity codec,
so the target also links `sketch_building_entities` and `sketch_building_objects`.
The
final application target also links its existing architecture toolkit set:
`TKernel`, `TKMath`, `TKG2d`, `TKG3d`, `TKGeomBase`, `TKBRep`, `TKGeomAlgo`,
`TKTopAlgo`, `TKPrim`, `TKBO`, and `TKBool`. OCCT's imported visualization
targets are available after `find_package(OpenCASCADE CONFIG REQUIRED
COMPONENTS FoundationClasses ModelingData ModelingAlgorithms Visualization)`.

For this project, the verified runtime prefixes are:

```text
.deps/qt/6.8.3/msvc2022_64
.deps/native/x64-windows
```

The application must put the matching Qt `bin` directory and OCCT's
configuration directory (`bin` for Release, `debug/bin` for Debug) on `PATH`
before launching. The Qt and OCCT DLLs are dynamically linked; no global
installation is required.

## Visual smoke check

Automated native checks use `scripts/test-native.ps1` after building the desktop
test targets. They render through hidden native HWNDs, exercise picking/panning
and aspect changes, plus geometry edits with undo/redo, at device pixel ratios
1, 1.5 and 2, and retain logs in
`artifacts/native-tests`, including retained PNG captures under the per-run
`captures/` directories. Qt owns HWND visibility; the OCCT wrapper must not
map a window that Qt has deliberately hidden. Geometry and architectural-form
checks run in separate processes at each scale, each with a 15-second deadline.
The executable requires `--expected-dpr` and checks the actual Qt device pixel
ratio before running framebuffer assertions; an ignored scale setting fails
the check. Both output streams remain available on failure.

`scripts/native-process-guard.ps1` retains the launched process handle, requests
termination on a deadline failure, and waits for bounded exit confirmation.
The runner reports a confirmed stop, a natural exit during cleanup, or an
unconfirmed termination separately. `native_process_guard` exercises a hidden
sleeper and normal, nonzero and already-exited children as part of CTest.

The tests also render both column forms, beams, stair flights with landings,
straight railings, sloped roof panels, gable roofs and hip roofs through the
shared building-entity codec.
Malformed forms block export, including after a snapshot update while the
viewport is hidden. Undo restores a valid presentation. Successful framebuffer
export requires a complete supported model; a partial or stale cache cannot
silently produce a successful image.

`tests/support/noninteractive_errors.hpp` sets process-local CRT/Windows error
handling before Qt is created. Exceptions, aborts and assertions retain failing exit codes
and write logs rather than waiting for a dialog. `noninteractive_test_errors`
checks those failure paths. CMake links the process-policy initializer into
every test executable. The assertion route is tested in both Debug and Release,
and the probe explicitly verifies stderr is the CRT error destination. Full application captures run through
`scripts/test-desktop.ps1`, which requests hidden `--smoke` windows, applies a
15-second timeout to each case and records image and executable hashes.
Standalone probes must use valid
`Entity::create(...)` fixtures or explicit JSON objects for metadata and catch
failures at their entry point. No machine-wide error-reporting settings change.

After the desktop target has been wired to the control:

1. Configure and build with `scripts/build.ps1 -Desktop`.
2. Start the executable from a developer prompt with the matching Qt and OCCT
   DLL directories on `PATH`.
3. Open or create a document containing one wall and one slab. Confirm that
   the native viewport shows shaded solids, `fitAll()` frames them, and the
   status overlay is empty (`isReady()` is true).
4. Right-drag, middle-drag, and wheel over the model. Confirm the camera moves
   smoothly and remains valid after a resize. Repeat at 100%, 150%, and 200%
   Windows display scaling so picking and cursor-anchored navigation stay
   aligned with the rendered model.
5. Left-click a wall or slab and confirm the inspector receives its stable
   entity ID; click empty space and confirm the inspector clears selection.
6. In the architectural workspace, Ctrl+left-drag a column or roof. Confirm
   the solid follows the pointer as a preview, release commits one semantic
   translation, and undo/redo restores the prior presentation.
7. Add a hosted opening or edit a wall scalar. Confirm the wall presentation
   updates while the camera remains where the user left it.
8. Call `exportViewImage("viewer-smoke.png")` and confirm the PNG contains the
   rendered OCCT framebuffer. Try an invalid extension/path and confirm the
   method returns `false` with an explicit error instead of a blank success.
9. Add an unsupported roof form, an incomplete room volume, or malformed wall field. Confirm the explicit pending or
   geometry error overlay appears, `onError` is called, and `isReady()` becomes
   false. No placeholder solid should appear. Add a plan-only boundary or
   annotation and confirm it does not produce a missing-3D warning.

The standalone object probe used while the application target is being
integrated is kept under `.deps/probes/visualization`; it builds
`src/visualization/native_model_view.cpp` with MSVC 19.44, Qt 6.8.3, and OCCT
8.0.1. It does not create generated files in the repository root.
