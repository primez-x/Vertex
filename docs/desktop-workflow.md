# Native desktop workflow checkpoint

This checkpoint is the first real Qt Widgets desktop workflow for Property
Studio. It is an internal implementation checkpoint, not a production-ready
release and not an Apex compatibility claim. The native UI is a dense,
canvas-centered precision workspace with one semantic document behind the
Measurement and Architectural tabs.

## Sources for the root build

The root CMake integration includes these desktop sources and dependencies:

| File | Role |
| --- | --- |
| `include/sketch/desktop/main_window.hpp` | Reusable `sketch::desktop::MainWindow` API and `Workspace` enum |
| `src/desktop/main_window.cpp` | Qt Widgets shell, commands, persistence, inspector, recovery selection, and output actions |
| `src/core/project_organization.cpp` | Pure hierarchy, inherited host context and unresolved relationship diagnostics |
| `src/desktop/building_object_dialog.cpp` | Six-form geometry editing and exact quantity entry records |
| `src/desktop/plan_canvas.hpp` | Native analytic plan canvas types and interaction seam |
| `src/desktop/plan_canvas.cpp` | QPainter grid, calibrated transform, analytic lines/arcs, pan/zoom, selection, and tools |
| `src/desktop/main.cpp` | Executable entry point and visual smoke fixtures |
| `src/visualization/native_model_view.cpp` | Native OCCT architectural viewport (integrated at the Architectural tab seam) |
| `tests/desktop_smoke.cpp` | Programmatic Qt smoke coverage for shared document, edits, history, save/reopen, and draft PDF |

The target needs Qt 6.8.3 modules `Core`, `Gui`, `Widgets`, and
`PrintSupport`. The implementation uses Qt 6.8's `QPdfWriter` from `Gui` and
`QPrintPreviewDialog` from `PrintSupport`; it does not require a direct
`QtPdf` API call. It does not add a cloud design dependency or a new account
requirement. The target also links the existing document/storage and precision
libraries, plus the repository's nlohmann-json include target. The executable
loads the bundled `:/fonts/Inter.ttf` resource and sets it as the application
font before constructing the window; visual smoke exits nonzero if that
resource is unavailable. Native Windows runs also link and include the
`sketch::visualization::NativeModelView` source, which needs OCCT's
`TKService`, `TKV3d`, and `TKOpenGl` visualization toolkits in addition to the
existing modeling toolkit set. The viewer header API is
`setSnapshot(const DocumentSnapshot&)`, `fitAll()`,
`setEntitySelectedCallback(std::function<void(QString)>)`,
`setErrorCallback(std::function<void(QString)>)`, `isReady()`, and
`lastError()`.

## Document API seam

`MainWindow` accepts an optional `std::shared_ptr<sketch::Document>` and owns
one shared pointer for both workspace views. If the supplied document is empty,
the constructor creates the minimum hierarchy through one atomic command:

`property-1` → `building-1` → `floor-1` → `layer-1`.

Nonempty documents retain their actual hierarchy and unassigned entities.
The navigator displays every entity once and supports mouse and keyboard
selection. The Drawing layer control chooses new geometry's destination.
Only a single valid layer can activate automatically; multiple-layer projects
require selection. Commands add buildings, floors with default layers, and
layers, or rename the selected container. A selected source boundary and the
active layer must agree before deriving a slab. These operations use captured
revisions and do not change existing world-coordinate geometry. Details and
remaining level/visibility work are in [project organization](project-organization.md).

Objects are then stored as semantic entities using the existing command API:

```text
measurement_boundary:
  floor_id, layer_id, segments[{start:[x,y], end:[x,y], sweep_radians}],
  classification, factor, factor_expression, factor_numerator,
  factor_denominator

wall:
  floor_id, layer_id,
  baseline:{start:[x,y], end:[x,y], sweep_radians:0},
  height_m, thickness_m, elevation_m, classification

opening:
  wall_id, offset_m, width_m, sill_m, height_m, opening_kind

slab:
  floor_id, layer_id,
  boundary:[{start:[x,y], end:[x,y], sweep_radians}],
  holes:[[...segments]], thickness_m, elevation_m
```

Coordinates and arcs remain metres and radians in the model. QPainter receives
a calibrated view transform only; pixels and rounded labels never enter the
document. The inspector edits wall length, classification, height, and
thickness through `Document::apply`. Undo and redo call the document's
monotonic revision API with the current expected revision.

Closed boundaries also expose an area calculation inspector backed by the
shared calculation kernel. It reports analytical base area, net area,
factored area, perimeter, building total, living total, and the displayed
rounding delta. An entered factor such as `3/4` is parsed exactly and stored as
the original expression, reduced numerator/denominator, and readable numeric
`factor` value. Invalid factors leave the existing entity unchanged.

The property entity stores a versioned `calculation_profile` with explicit
classification rules. The inspector's building and living checkboxes edit
that profile through one normal document command and advance its version.
Unknown or unassigned classifications block totals with an explicit error;
living area is never inferred from free-form labels. `calculate_areas` also
rejects unresolved references, invalid geometry, and overlapping areas rather
than displaying a plausible total. The current slice has no deduction editor;
deductions remain a calculation-kernel capability for a later bounded task.

The public methods used by the smoke test are `createBoundary`,
`createStraightWall`, `createHostedOpening`,
`createSlabFromSelectedBoundary`, `createSlabFromBoundary`, `selectEntity`,
`editSelectedClassification`, `editSelectedLength`, `editSelectedHeight`,
`editSelectedThickness`, `editSelectedFactor`, `setSelectedCalculationRule`,
`undoCommand`, `redoCommand`, `saveProjectAs`, and `openProject`. Opening
creation gathers a Door or Window kind and offset,
width, sill, and height quantities, then previews the host wall with all of its
openings through `make_wall` before one atomic document command. Slab creation
validates the selected closed boundary and previews the complete profile with
`make_slab` before the slab command. The UI also exposes `createNewProject`,
`saveProject`, `exportDraftPdf`, `exportNativeViewImage`, and
`showPrintPreview` for the normal workflow.

The `Reference` command imports a local PNG, JPEG, BMP, TIFF, or the first page
of a PDF into the portable Document Asset store and creates a `reference_asset`
record. Raster source bytes are rendered directly. PDF source bytes remain in
the project beside a deterministic PNG preview generated by the bundled Qt PDF
renderer; both asset links are validated. The decoded image is drawn beneath
the shared measurement and architectural canvases; the inspector edits model
position, metres-per-pixel calibration, scale, rotation, flips, intensity, and
visibility through one undoable command. The inspector's known-distance dialog
accepts two source-pixel points and an imperial or metric expression, then
stores the original expression, entered unit, source points, and derived scale
in the same typed history. Multiple underlays, PDF page selection, and
interactive tracing are still pending production work.

## Interaction and persistence

The left navigator expands project, building, floor, and layer nodes. The tool
rail provides Select, Boundary, Wall, Object, Grid, Snap, and Fit. Boundary drawing
accepts clicks, Enter closes the path, and `D` opens a distance/direction input
such as `10 ft @ 90 deg`. Wall drawing accepts two clicks. Select and the
navigator select existing semantic objects, so edits and history operate on
the same document in either workspace tab. Walls display their opening gaps in
the plan canvas; the OCCT architectural view derives the corresponding cut
solids from the wall and opening entities. Command search provides `Create door
opening`, `Create window opening`, and `Create slab from selected boundary`;
each prompts for quantities and runs the same validated commands as the public
workflow seam.

`Object…` opens the column, beam, stair and roof parameter editor. Selecting
one of those objects exposes `Edit object…` in the inspector. Both operations
validate the candidate before one revision-checked document command; stale
edits are rejected, metadata is preserved, and normal undo/redo applies.
Unchanged fields retain their original precision. See
[building-object-editor.md](building-object-editor.md) for the six forms.

These objects appear in the native 3D view and in both plan canvases through
exact OCCT top-down projection. The shared canvas renderer also draws them
in draft PDF/print output. A projection failure is visible in the workspace
and blocks output before a partial PDF or print preview is created. The current
projection has no architectural cut plane, level filtering or hidden-line
style; those coordinated drawing features remain required.

Imperial feet/inches are the default display and input units. Metric can be
selected in the top workspace strip. The quantity parser handles exact input
syntax. The six column/beam/stair/roof forms retain versioned exact quantity
receipts alongside their numeric geometry. Their dialog-to-document tests cover
unrelated edits, opaque future records, undo/redo and save/reopen for every form.
Other commands still need their complete measurement-provenance integration.
Grid, snap, pan, zoom, fit, near-cursor coordinates, keyboard shortcuts, and a
Ctrl+K command search palette are active. Unsupported features are not shown
as enabled controls. The Theme toolbar menu and command palette select light,
dark, or high-contrast palettes; these presentation controls do not alter
document geometry or measurement units.

Draft PDF and SVG export use the same fit-to-content vector scene as the
interactive preview and print callback. Both outputs carry the visible draft
stamp and remain separate from the future authoritative fingerprint/currentness
gate. The workspace strip exposes Letter, Legal, Tabloid, A4, and A3 output
sheet choices; the selected size is applied to PDF and print preview without
changing document geometry or revision.

Open loads into a temporary `LoadResult` and swaps the document only after a
successful load, so corrupt, unsupported, or unreadable files leave the
current document intact. Saves use `ProjectStore` exclusively. The current
file fingerprint is supplied when replacing an existing destination, and a
save failure leaves the document and original file in place. Unknown required
data remains read-only and is surfaced in the inspector; edit and save actions
are disabled. Closing a dirty editable document prompts to save, discard, or
cancel.

Architectural transaction previews now have a Document adapter that emits one
typed, revision-fenced command for create, property edit, transform, duplicate,
and delete operations. The adapter preserves unrelated measurement entities,
supports one-step Document undo, and is covered by a save/reopen fixture. The
architectural inspector routes property edits through that adapter, including
recovery-backed workspace edits, while preserving numeric and structured JSON
values. The full object-family schema and visible architectural inspector
bindings remain under qualification.

New projects also include a typed `sheet_view_model` entity containing a
default plan view and A-101 sheet. It is validated and saved with the same
Document revision as geometry; interactive sheet editing and layout rendering
remain under qualification.

## Wall dimensions and constraints

In either workspace, select a straight wall and choose **Dimensions and
constraints** in the inspector or command palette. Editing the inspector's wall
length opens the same preview dialog. Choose the endpoint to keep fixed and
whether walls connected by explicit constraints may move, then choose Preview.
Dashed lines and square endpoint marks show the current walls; blue lines show
the proposed walls. A ring identifies an unchanged endpoint. Length changes and
endpoint displacement appear in the comparison details before Apply.

The dialog also adds, edits and removes horizontal, vertical, coincident,
fixed-length, parallel, perpendicular and fixed-anchor relations using stable
wall endpoint choices. An incompatible edit shows its diagnostics and leaves
Apply disabled. Any input change invalidates the previous preview. Cancel does
not change the document. Apply recomputes the stored intent against the captured
document and records geometry and relation changes as one undoable command.

Opening, slab, building-object and organization dialogs retain document identity,
revision, selection, active layer and units across their prompts. A changed
context rejects the old intent rather than directing it to a different source.

The wall constraint integration remains under qualification. Curved-wall
constraints, persistent boundary vertex bindings, level dependency propagation
and the complete production constraint workflow are still required.

`scripts/test-constraint-editor.ps1` captures the dialog at normal and 150 percent
scale with the bundled font and a 15-second process timeout. It exercises both
workspace entry points; separate native 3D rendering tests remain necessary.

## Draft output and visual smoke

Floor/layer checkboxes and Show all control both workspace plans and the native
3D presentations. These transient view filters preserve the document, history
and calculation membership. Mouse checkbox clicks preserve selection; keyboard
navigation selects rows and Space toggles their visibility. Hidden drawing
destinations remain explicit. See `project-visibility.md` for project-switch
behavior and the scaled capture runner.

PDF export and print preview use the canvas's shared QPainter geometry renderer
with an independent fit-to-content paper transform and a white background.
They stamp `DRAFT — internal checkpoint` while sheets, profiles, and complete
output qualification remain open. PDF, SVG, and native 3D image exports write
an adjacent output-fingerprint manifest covering the document head, page/filter
view descriptor, linked processing roles, and running Windows executable. An
active filter instead stamps `DRAFT — VIEW FILTER ACTIVE` and explains that
view filters do not change totals. Draft output does not mutate the document.

The executable supports:

```text
property-studio.exe --smoke --smoke-output C:\path\desktop-smoke.png --smoke-size 1366x768
property-studio.exe --smoke --smoke-workspace architectural --smoke-output C:\path\architectural.png --smoke-3d-output C:\path\model.png
```

`--smoke` creates a representative 12 m × 8 m boundary and two interior walls
through the same public command methods, fits the view, captures one screenshot,
and exits nonzero if the image cannot be written. `--smoke-workspace
architectural` instead seeds walls, a hosted Door opening, and a slab through
the native command seam. With that mode the optional `--smoke-3d-output` path
is exported from the real OCCT framebuffer; an unavailable or not-ready native
viewer returns a nonzero exit code. If no output path is given, each image is
written under the system temporary directory with a unique filename.
`--smoke-size` is optional and defaults to 1366 × 768.

On native Windows the Architectural tab contains the shared semantic plan
canvas beside the real OCCT `NativeModelView`. Snapshot refreshes, stable-ID
selection, errors, and Fit are wired to the same `Document`. Offscreen and
minimal Qt platforms intentionally skip native viewer construction so the
visual smoke path remains deterministic; the tab shows an explicit reason
instead of a fake projection. The native export method therefore reports an
unavailable viewer on those platforms. Broader production 3D coverage remains
outside this checkpoint.
