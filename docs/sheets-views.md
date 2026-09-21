# Coordinated view and sheet semantic model

`SheetViewModel` provides a validated immutable semantic foundation for
ARCH-VIEW-001/002, ARCH-SHEET-001 and IO-OUTPUT-002. The
`sheet_view_model` Document entity codec persists this graph through the
versioned project format and reuses the same validation at create/load/command
boundaries. New desktop projects start with validated plan, elevation and
section views, a sheet with plan/elevation/section viewports, title block, and
independently scaled viewports in that entity. Draft PDF, SVG, and print output now consumes the persisted sheet
dimensions, viewport bounds/scales, title block and view-specific vector
geometry derived from the same immutable document snapshot, regardless of the
active workspace tab. Desktop sheet title-block/number and viewport
bounds/scale editing now commit through the typed Document history; schedule
placement rendering, including revision-bound door/window, room and material
rows, now uses the same schedule projection as the Schedules dialog. Desktop
sheet layout can add or remove shared-view viewports and registered-schedule
placements as well as edit their bounds and scales. The detached layout dialog
stages all changes until OK, commits them as one undoable Document command, and
discards them on Cancel. Placement IDs remain stable through save/reopen, and a
viewport referenced by a surviving callout is protected from removal. Sheet
revisions and cross-sheet callouts can be added, edited, and removed through
the same revision-checked history path. Vector sheet output renders callout
markers with target sheet/viewport references and a revision block beside the
title block. Printer calibration and production output qualification remain
open. The shared
architectural projection engine now derives analytical plan, elevation, and
section edges from supported building solids; see
`docs/architectural-projections.md` for its explicit boundary and remaining
integration work.

The Drawing sheets dialog also manages the page collection. A new page is
created through the typed model with a validated size, copied project
title-block context, and one independently scaled viewport for each
coordinated view. Removing a page is undoable and fails when it would leave a
dangling cross-sheet callout or remove the final page. Move up/down commands
persist an explicit page order through normal undo/redo and save/reopen. The
dialog also selects the page used by single-sheet PDF, SVG, PNG, and print
output; that presentation selection does not dirty the document. Drawing-set
PDF and print commands enumerate every page in the persisted order and bind the
complete ordered set to a distinct output fingerprint. Drawing-set preview is
rendered from the staged multipage PDF instead of Qt's single-layout preview
widget, so portrait, landscape, and custom pages retain their own geometry.
Physical printing preflights every page and aborts the printer job if a later
page layout or render fails.
The selected sheet's persisted width and height in millimetres determine the
PDF physical page with zero margins, SVG physical dimensions and
viewBox aspect, and the custom paper requested by print preview. Changing the
toolbar page preset does not override a persisted sheet. The preset remains a
fallback for documents without a sheet graph. Print receipts report
`requested_sheet_mm` separately from the actual driver paper/page rectangles
and DPI; driver acceptance does not establish physical printer calibration.
Numeric PDF/SVG and preview-driver regression checks are development evidence.
Physical calibration and paired production-output certification remain open.

Views identify plan, elevation and section definitions by stable ID. A view owns
its finite origin in metres, orthonormal direction/up frame, cut and far depths,
paper line widths, hatch enable/pattern/scale and detail level. It can also carry
an ordered, deduplicated list of stable semantic object IDs for the walls,
rooms, slabs, openings, terrain, and other source objects represented by that
view. Document admission resolves those IDs and rejects a view that would keep
a dangling object reference, so deletes cannot silently retarget presentation
geometry. Hosted opening references admit their wall as a projection dependency,
and selecting an assembly host admits its placed assembly preview; this keeps
the projected solid graph complete without copying or mutating source geometry.
Cut depth is a
nonnegative distance along the direction from the origin, limited by far depth;
these are projection instructions, not computed model intersections. All kinds
use the same frame contract; adapters choose the appropriate orientation. The
desktop Architectural view settings command edits these presentation fields
through typed Document history. The desktop renderer conservatively culls
solids whose BRep bounding range lies wholly beyond far depth. Objects crossing
the limit are clipped with an OCCT half-space before projection. Section views
also own persisted text, detail-line, and explicit-endpoint dimension overlays.
Every overlay has a stable ID, paper-space text/line sizing, and a minimum
coarse/medium/fine detail level; the same retained canvas scene feeds the
interactive view and draft output. Coarse presentation suppresses hatching,
and dimension end ticks print at a fixed 2.5 mm total length independent of
device DPI or viewport model scale. Overlay dimensions are intentionally
non-associative until object/edge references are added. Production visual and
physical-print qualification remain open.

Sheets have explicit positive page dimensions in millimetres, unique sheet
numbers, title-block metadata, revisions, callouts and schedule placements.
Every viewport references one shared view definition and has its own positive
model-to-paper scale denominator (100 denotes 1:100). Changing a shared view
with `with_view` returns a new snapshot and preserves all referencing sheet
placements and their individual scales. Callouts reference a specific viewport
on a specific sheet, permitting cross-sheet coordination. Schedule placements
reference a caller-supplied registry of schedule IDs; this module does not
calculate or render schedule contents.

IDs must be nonblank and unique in their collection. View and sheet IDs are
global within a snapshot; viewport, callout, revision and schedule-placement
IDs are scoped to their sheet and collection. References must resolve. Page
rectangles and callout anchors must lie on the page. Placements may overlap:
collision/layout policy belongs to an editor. Dates and hatch patterns are
opaque metadata; no calendar or renderer-specific pattern interpretation is
implied. Revision IDs are stable identifiers, not chronology inferred from
lexical order. The desktop editor exposes the persisted revision and callout
collections with typed graph validation; callout target sheet and viewport
references are never inferred from display labels.

Version 4 JSON uses `sketch.sheet_view_model`, persists section overlays and an
explicit `sheet_order` that is an exact permutation of the sheet identities, and
rejects unknown/missing fields,
invalid enum names, nonfinite numeric values, malformed frames and dangling
references. Version 1 through 3 documents remain readable, normalize missing
`object_ids` or overlay collections to empty lists, and derive page order from
the canonical sheet-ID sequence before strict validation. Definition collections
serialize in ID order (schedule registry lexically), while `sheet_order` retains
the user-visible page sequence,
independent of insertion order. JSON output and caller inputs are detached from
the stored snapshot. Import validates the complete graph before returning a
snapshot. Tests cover coordinated edits, scale independence, input isolation,
deterministic round trips and malformed geometry, identities and references,
including revision and callout replacement, addition, removal, and target
validation. Placement lifecycle tests also cover duplicate identities, invalid
references and page bounds, callout-protected deletion, empty layouts, dialog
staging, atomic undo/redo, and save/reopen.
The entity codec tests cover typed Document admission and ProjectStore
save/reopen, including schema, version, unknown-field and dangling-view
rejection.
