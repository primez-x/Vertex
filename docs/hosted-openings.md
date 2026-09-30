# Hosted opening authoring

Select a wall and run **Create door opening** or **Create window opening**.
One editor provides offset along the wall, width, sill height, and height, using
the current input unit and the shared exact measurement parser. Explicit units
and fractions are supported. A live elevation preview shows existing openings
and the proposed opening. Curved walls are shown as an explicitly labeled
unrolled elevation; horizontal distance follows the wall's centreline arc length.

The editor validates the complete host and its openings with
`validate_wall_semantics`. Invalid input, out-of-bounds openings, and overlaps
disable creation and display an inline explanation. Invalid drafts are removed
from the preview. Correcting the fields restores the preview and Create button.
Cancellation leaves the document unchanged.

The main window captures the source document, revision, selection, layer, and
units before opening the editor. Submission checks that context, then uses the
existing hosted-opening command and solid validation. A successful creation is
one undoable document operation. The object retains its host, dimensions, and
door/window classification through the ordinary project format and schedules.

Verification: `desktop_smoke` exercises the actual command palette, invalid and
corrected drafts, fractional input, creation, undo/redo, and curved-host overlap
checks. `modal_authoring_tests` intervenes with project replacement, selection,
layer, units, and revision changes while the new editor is open.

Door creation optionally enables a 90-degree swing. The door inspector's
**Door swing** editor changes the start/end jamb, left/right side, and angle
(greater than zero, at most 180 degrees), or removes the symbol. Jamb order and
swing side are relative to the host wall's drawing direction. The optional
`door_operation` property stores `version: 1`, `hinge: "start" | "end"`,
`side: "left" | "right"`, and numeric `angle_degrees`. Unknown versions, invalid
fields, and invalid angles are rejected by Document validation. Unspecified
existing openings receive no inferred swing.
Doors without swing data retain visible jamb and threshold linework in the
plan and support the same straight-wall width handles; editing them does not
create hinge or swing metadata.

Plan output uses a straight leaf and an analytic circular arc, sharing geometry
between the canvas and printed/exported plan scene. On curved walls, the closed
leaf follows the chord between jambs. Schedules expose read-only hinge, side, and
angle with source provenance. Ordinary dimension edits preserve operation data;
conversion to a window retains it as dormant metadata and suppresses its symbol
and schedule cells. Undo or conversion back to a door restores its use.

Newly authored doors and windows also receive an optional, versioned
`opening_assembly` profile. The profile is type-driven (`door` or `window`) and
stores frame width/depth, panel thickness, glazing thickness, and signed inset
in metres. The host wall cut remains authoritative; the native 3D view adds a
derived frame, leaf or sash, and glazing compound inside that cut. A door leaf
and its optional glazing follow the persisted handed swing. Window profiles
produce a four-bar sash and a real glazing pane. Curved windows use concentric
annular frame, sash and pane solids with signed wall-normal inset. Curved doors
use radial frame parts and a fitted planar chord leaf. Its finite thickness and
chord sagitta must fit the frame head; insufficient depth rejects with a fit
error rather than changing the specified frame dimensions. The pivot uses the
fitted leaf endpoint. Door glazing replaces an aperture in the leaf, avoiding
overlapping solid material. Handing rotates the leaf and pane together;
collision-free clearance at arbitrary swing angles is not certified.
Legacy openings without the
profile remain valid and continue to render as wall cuts, which keeps import
lossless while a user upgrades selected instances.

Schedules expose the profile kind and dimensions as read-only source-backed
properties. The profile is strict schema version 1: unknown versions, missing
fields, kind mismatches, impossible panel/depth relationships, and assemblies
that do not fit the host wall are rejected before a Document mutation.
The opening inspector's **Opening assembly…** command edits all five profile
dimensions (frame width/depth, panel or sash depth, glazing depth, and signed
wall-centreline inset) in the active unit system. The command previews the
complete host and sibling openings, applies one revision-checked history entry,
and supports undo/redo. A door may keep zero glazing; a window requires a
positive glazing depth. Malformed or stale edits remain visible in the dialog
and leave the document unchanged.

The derived compound is not exported as authoritative project geometry;
native DXF/IFC and full ARCH-MOD-002 export acceptance remain production-gate
work.

In an uncropped conventional plan, selecting a door, window or bare opening on
a straight or circular wall shows two width handles at its jambs and a **W × H**
readout (**Arc W × H** on circular hosts). Drag either handle to resize along
the wall while pinning the opposite jamb. Curved width is measured along the
host arc, rather than between the jambs in a straight line. Window rails follow
concentric arcs; door swing geometry uses the straight jamb chord.
Height, sill, wall thickness, handing and manufactured frame dimensions remain
unchanged. The preview regenerates the circular swing and wall cut together.
Overlaps, an out-of-host span or insufficient clear frame width appear as an
invalid proposal; release leaves the project unchanged. Final admission also
regenerates the host, all sibling assemblies and affected wall joins.

The drag commits one undoable revision-fenced command. Escape, focus loss or a
refreshed document cancels it. Save/reopen retains width and offset. Print and
export use committed geometry and exclude handles and drag proposals. Cropped
plans and alternate projection frames retain dimension editing through
properties; their plan-width handles are not available.
