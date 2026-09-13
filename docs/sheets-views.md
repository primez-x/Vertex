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
schedule placement bounds can be edited through typed Document history. Sheet
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
dangling cross-sheet callout or remove the final page. The dialog selects the
page used by draft PDF, SVG, and print output; that presentation selection is
included in the output scene fingerprint without dirtying the document.

Views identify plan, elevation and section definitions by stable ID. A view owns
its finite origin in metres, orthonormal direction/up frame, cut and far depths,
paper line widths, hatch enable/pattern/scale and detail level. Cut depth is a
nonnegative distance along the direction from the origin, limited by far depth;
these are projection instructions, not computed model intersections. All kinds
use the same frame contract; adapters choose the appropriate orientation. The
desktop Architectural view settings command edits these presentation fields
through typed Document history. The desktop renderer conservatively culls
solids whose BRep bounding range lies wholly beyond far depth. Objects crossing
the limit are clipped with an OCCT half-space before projection; output
overlays and production qualification remain open.

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

Version 1 JSON uses `sketch.sheet_view_model`, rejects unknown/missing fields,
invalid enum names, nonfinite numeric values, malformed frames and dangling
references. Collections serialize in ID order (schedule registry lexically),
independent of insertion order. JSON output and caller inputs are detached from
the stored snapshot. Import validates the complete graph before returning a
snapshot. Tests cover coordinated edits, scale independence, input isolation,
deterministic round trips and malformed geometry, identities and references,
including revision and callout replacement, addition, removal, and target
validation.
The entity codec tests cover typed Document admission and ProjectStore
save/reopen, including schema, version, unknown-field and dangling-view
rejection.
