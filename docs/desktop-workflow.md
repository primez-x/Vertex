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

The same Document boundary also admits the architectural semantic records that
span workspaces. An `assembly_model` entity stores an
`sketch.assemblies.v1` model, a `model_phases` entity stores a
`sketch.model_phases` model, and a `room_relationships` entity stores a
`schema_version: 1` relationship snapshot. Each record places its typed JSON
under `properties.model`. Admission decodes the model before mutation, checks
every referenced entity in the candidate snapshot, and verifies that room
references target the declared `room_boundary`, `measurement_boundary`, or
`wall` role. A failed decode or reference check rejects the whole command and
leaves the prior revision unchanged.

The Architectural workspace's **Assemblies** command opens the reusable catalog
editor. It creates an empty typed assembly model when a project has none, then
adds/removes reusable type and instance records and renames types through
revision-fenced Document commands. The editor lists type and instance IDs,
blocks removal of a type that still has placements, and preserves the full
`sketch.assemblies.v1` payload on save/reopen. Material slots, quantities,
overrides, and geometric placement remain explicit model data for the next
authoring slice.

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
  than displaying a plausible total. The calculation inspector's **Edit
  deductions** command lists valid closed boundaries on the active floor and
  stages add/remove changes before applying them. Applying validates that each
  deduction is contained by the selected boundary, rejects duplicate or nested
  deductions, and stores explicit `deduction_ids` on the base entity through
  normal revision-fenced history. Referenced deduction boundaries are removed
  from independent area aggregation, while the selected result exposes each
  requested and newly applied deduction amount. Apex same-type Auto-Subtract
  compatibility remains an open certification item.

The public methods used by the smoke test are `createBoundary`,
`createRoomBoundary`,
`createStraightWall`, `createHostedOpening`,
`createSlabFromSelectedBoundary`, `createSlabFromBoundary`, `selectEntity`,
`editSelectedClassification`, `editSelectedLength`, `editSelectedHeight`,
`editSelectedThickness`, `editSelectedFactor`, `setSelectedCalculationRule`,
`copySelection`, `cutSelection`, `pasteSelection`, `deleteSelection`,
`undoCommand`, `redoCommand`, `saveProjectAs`, and `openProject`. Opening
creation gathers a Door or Window kind and offset,
width, sill, and height quantities, then previews the host wall with all of its
openings through `make_wall` before one atomic document command. Slab creation
validates the selected closed boundary and previews the complete profile with
`make_slab` before the slab command. The UI also exposes `createNewProject`,
`saveProject`, `exportDraftPdf`, `exportNativeViewImage`, and
`showPrintPreview` for the normal workflow.

Selecting the top-level property exposes the compact **Project details**
inspector. It edits the subject name, address, reference, and a bounded JSON
object of string attributes through `MainWindow::editProjectSubject`; the
complete update is one revision-fenced Document command and therefore supports
undo, redo, save, and reopen without a hosted service. Older projects without a
`subject` object fall back to their existing property name until the details are
saved.

Selecting a closed measurement or room boundary also exposes **Area
attributes**. The editor stores a bounded JSON object of string metadata under
`area_attributes`, validates it before mutation, and keeps the update in the
same undo/redo and save/reopen path.

The `Reference` command imports a local PNG, JPEG, BMP, TIFF, or a selected page
of a PDF into the portable Document Asset store and creates a `reference_asset`
record. Raster source bytes are rendered directly. PDF source bytes remain in
the project beside a deterministic PNG preview generated by the bundled Qt PDF
renderer; both asset links are validated. The decoded image is drawn beneath
the shared measurement and architectural canvases; the inspector edits model
position, metres-per-pixel calibration, scale, rotation, flips, intensity, and
visibility through one undoable command. The inspector's known-distance dialog
accepts two source-pixel points and an imperial or metric expression, then
stores the original expression, entered unit, source points, and derived scale
in the same typed history. Repeated imports retain independent underlays, each
selectable from the navigator or canvas and editable in the inspector. Visible
underlays render in stable entity-ID order beneath authored geometry, including
sheet output; the last underlay in that order is picked first where they overlap.
The PDF import dialog asks for a page when the source contains multiple pages;
programmatic imports retain the first-page default. PDF content remains a raster
reference, with no editable extraction. **Trace selected reference** starts the
same receipt-bearing boundary authoring session used by measured drawing, so
the operator can click or precisely enter a boundary over the calibrated image;
the committed boundary remains model geometry and can be classified and
calculated independently of the pixels. Automated edge extraction and editable
PDF geometry remain future work.

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

The overflow menu and command palette also provide **Copy selection**, **Cut
selection**, **Paste selection**, and **Delete selection**. Clipboard data is a
bounded `sketch.document.clipboard` JSON payload held by the local system
clipboard; it contains semantic entity records rather than rendered pixels or
service references. Copy is side-effect free. Paste allocates fresh entity,
boundary-edge, vertex, and annotation-child identities, places supported
geometry on the active drawing layer, and remaps hosted openings and dimension
owners. Cut and delete remove owned hosted children in one guarded Document
command, so undo and redo restore the complete graph. Unsupported, malformed,
oversized, or referenced records fail before mutation.

Paste remaps documented relationship fields and local boundary/annotation
identities only. Names, descriptions, label content, template and symbol catalog
identities, unknown property metadata, and extension records retain their exact
values, even when their text matches a copied entity ID. Dimension target
metadata is preserved while its boundary and segment references are updated.

The navigator also exposes a persisted Design phase selector. The first use can
create a baseline registry from the current architectural objects; the manager
then creates named alternatives with explicit baseline demolitions. Selecting a
phase is a revision-checked, undoable document command. The active phase is
applied consistently to both canvases, native 3D visibility, schedule rows, and
 sheet viewports/output. Proposed-object authoring remains a separate production
 work item.

Room and boundary relationships are available from **More → Room relationships**
and the command palette. The editor keeps room boundaries, appraisal measurement
boundaries, and architectural walls as distinct reference kinds. Each declared
relationship records its source, target, and independent/follows/derived-from
kind; graph validation rejects cycles, ambiguous drivers, duplicate pairs, and
role violations before the typed `room_relationships` entity reaches Document
history. **Sync references** adds newly created live geometry and removes stale
endpoints while retaining valid relations. The editor is an authoring slice;
geometry propagation and controlled retargeting still require explicit follow-up
operations.

The command palette also provides **Create room boundary from selected
geometry**. It copies a validated closed measurement or room boundary into a
new `room_boundary` entity with its own name, classification, area, and
analytical segments; the source remains unchanged and the new entity is fully
undoable. This gives architectural rooms a deliberate creation path while
keeping appraisal measurement geometry separate.

**Detect closed areas…** scans the selected wall's floor and drawing layer as an
endpoint-connected analytical graph. Every simple bounded face becomes its own
room boundary in one atomic command; unfinished wall stubs are ignored, source
walls remain unchanged, and the selected classification is applied to each
result. The command is available from **More** and the command palette, and its
generated room entities can be reviewed, edited, undone, and redone through the
ordinary document history. Nested-loop hole semantics and Apex parity fixtures
remain explicit qualification work.

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

## Named revisions and comparison

The **Named revisions** command is available from **More** and the command
palette. It records a user-provided name at the current Document revision
through the normal undoable `NameRevision` command, so names are portable in
`.bldproj` files and survive save/reopen. The editor lists named revisions in
document order and compares any selected revision with the current head by
counting added, removed, and changed entities and assets. Comparison is
read-only and does not alter the current selection or history.

**Restore as new project** reconstructs the selected retained history prefix,
marks that copy clean, and writes it to a new `.bldproj` destination. It never
replaces the open document, discards later edits, or mutates the named source
revision. A destination that already exists is rejected by the same guarded
ProjectStore path as ordinary Save As. Revision copy output and independent
comparison reports still need production fixtures and end-to-end qualification.

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

## Selection transforms

The **Transform selection** command is available from **More** and the command
palette when a supported identified closed boundary or wall is selected. It applies an
analytic rotation around the complete boundary-bounds center, independent
horizontal/vertical reflections, and X/Y offsets in the active input units.
The UI's horizontal flip reflects X and its vertical flip reflects Y for both
walls and boundaries; the core geometry API names the reflection axis instead.
Circular-arc extrema contribute to the boundary pivot and canvas fit; endpoint
boxes and sampled strokes are not used as substitutes for curved extents.
Newly authored boundaries receive stable segment and vertex identities before
they enter the document. Applying a transform then updates the same typed
boundary entity through one revision-checked Document command, preserving
those identities so the operation can be undone and redone exactly. A boundary
with receipt or dependent-reference semantics is rejected until those
relationships have an explicit transform policy.

The same **Transform selection** command accepts straight and curved walls.
Their pivot is the endpoint midpoint. Rotation, reflections, and translation
preserve wall dimensions and hosted-opening distances along the baseline.
A single reflection reverses the analytical arc sweep and stored door swing
side; two reflections retain both. The existing hard constraints remain active
and incompatible transforms reject atomically. Valid wall length-entry receipts
retain their exact expression while their recorded baseline follows the move.
Wall copy mode creates fresh wall/opening identities together and retains their
material assignments. It does not copy external constraints or room links.
The editor previews the original and proposed wall/opening or boundary geometry as
inputs change, using a detached validated document candidate. Invalid values
or constraint conflicts clear the preview and disable Apply. Apply commits the
exact cached command, including copy identities; Cancel creates no history.
Intervening document or selection changes invalidate the candidate. Boundary
previews use the same identity, geometry, and receipt checks as committed edits;
unchanged transforms do not create history. Room-boundary propagation and
connected-wall group transforms remain open production work.

Boundary clone mode allocates a new boundary, segment IDs, and vertex IDs. It
preserves drawing context, area metadata, names, custom properties, extensions,
and per-edge metadata, while remapping recognized self-references. User text and
opaque metadata are not rewritten as identifiers. The source entity and its
relationships remain unchanged. Supported construction receipts now accompany
offset copies, retaining original distance/angle expressions and rebuilding the
copied geometry through receipt replay. Boundary dimension labels are copied
with fresh identities and transformed text positions in the same undoable command.
For supported in-place transforms, dimension labels move with their boundary
while retaining their identities, placement mode, and metadata. Rotation,
reflection, and translation use the same label-position path as copies.
Receipt-backed in-place offsets use a typed document command. It preserves
boundary and dimension identities, records the translation intent in history,
and reconstructs the entire expected state during restore. Ordinary entity
edits still cannot rewrite construction inputs. These projects use format 5
to retain the intent through save, recovery, and exchange.
Receipt-bound rotation/reflection remain unfinished. Some
offsets also reject when floating-point translation cannot preserve the exact
closure-vector relationship. Unhandled geometry-owned receipts or dependent
semantics reject the copy without changing history. Imported anonymous legacy boundaries are promoted
only when a command can preserve their exact geometry and metadata; an
in-place transform of such a boundary remains blocked until an explicit
identity-upgrade path is provided. Unsupported boundary versions fail closed
with a version diagnostic. **Insert boundary vertex…** is available from
**More** and the command palette. It accepts an identified edge and a strict
interior fraction, then replaces the boundary atomically with fresh boundary,
edge, and vertex identities. Existing typed links are migrated to the fresh
identities, no overlapping source geometry is left behind, and undo/redo restores
the complete replacement. Dimension targets, annotation overrides, room
relationships, and phase memberships follow the replacement. Boundary names,
label content, extension metadata, and alternative IDs/names are not treated
as references merely because their text matches an old identity.
Per-edge metadata stays with its continuing edge identity: the first piece of
a split retains it, the new second piece starts without it, and unaffected
edges keep their metadata. Unhandled directional receipts on a split edge
reject the edit atomically rather than being discarded or duplicated.
Legacy anonymous and receipt-bound boundaries fail
closed until their identity and derivation policies are explicit. Additional object
transforms, receipt-bound derivation edits, and full linked-relationship
qualification remain open. The **Create room boundary
from selected geometry** command can also assemble the selected wall's
connected analytical component into one room boundary. It preserves the
source walls, rejects open/branched/disconnected topology, and commits the
room through normal document history.

**Redefine boundary…** starts the same Draw First editor against the selected
identified boundary. Finish with the same number of edges to replace its
analytical geometry in place while retaining stable edge and vertex references;
the change is one guarded, undoable Document command. Invalid topology, legacy
anonymous data, receipt-bound authoring, and edge-count changes fail before
mutation. The command remains available from **More** and the command palette.

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

The Drawing sheets command manages the persisted page set. Adding a page seeds
coordinated plan/elevation/section viewports with independent scales; removing
a page uses the typed graph validator and is undoable. The selected page in
that dialog is the page rendered by draft PDF, SVG, and print, and its identity
is bound into the adjacent output fingerprint.

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
