# Coordinated view and sheet semantic model

`SheetViewModel` provides a validated immutable semantic foundation for
ARCH-VIEW-001/002, ARCH-SHEET-001 and IO-OUTPUT-002. The
`sheet_view_model` Document entity codec persists this graph through the
versioned project format and reuses the same validation at create/load/command
boundaries. New desktop projects start with a validated default plan view,
sheet, title block, and independently scaled viewport in that entity. These
requirements are not complete product workflows: rendering, desktop editing
and print/PDF layout integration remain unimplemented here.

Views identify plan, elevation and section definitions by stable ID. A view owns
its finite origin in metres, orthonormal direction/up frame, cut and far depths,
paper line widths, hatch enable/pattern/scale and detail level. Cut depth is a
nonnegative distance along the direction from the origin, limited by far depth;
these are projection instructions, not computed model intersections. All kinds
use the same frame contract; adapters choose the appropriate orientation.

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
lexical order.

Version 1 JSON uses `sketch.sheet_view_model`, rejects unknown/missing fields,
invalid enum names, nonfinite numeric values, malformed frames and dangling
references. Collections serialize in ID order (schedule registry lexically),
independent of insertion order. JSON output and caller inputs are detached from
the stored snapshot. Import validates the complete graph before returning a
snapshot. Tests cover coordinated edits, scale independence, input isolation,
deterministic round trips and malformed geometry, identities and references.
The entity codec tests cover typed Document admission and ProjectStore
save/reopen, including schema, version, unknown-field and dangling-view
rejection.
