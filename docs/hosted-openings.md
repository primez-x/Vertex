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
**Door operation** editor changes the mechanism, start/end jamb, left/right side, and angle
(greater than zero, at most 180 degrees), or removes the symbol. Jamb order and
swing side are relative to the host wall's drawing direction. The optional
`door_operation` property stores `version: 1`, `hinge: "start" | "end"`,
`side: "left" | "right"`, and numeric `angle_degrees`. Unknown versions, invalid
fields, and invalid angles are rejected by Document validation. Unspecified
existing openings receive no inferred swing.
Version 2 contains exactly six fields: `version`, `kind`, `hinge`, `side`,
`angle_degrees`, and `slide_fraction`. `kind` accepts `hinged`, `double_hinged`,
or `sliding`. `slide_fraction` is finite in [0,1] and must be zero for both
hinged kinds. Angles remain finite in (0,180], including the retained angle of
a slider; sliders do not rotate. Ordinary hinged operations are canonically
encoded as the original four-field version 1, preserving existing records.

The library now separates French double doors, interior/exterior hinged doors,
opaque sliding doors, and single/double garage widths. French doors create two
glazed physical leaves; the opaque slider keeps the same two-track mechanism
without glazing. Cased openings route to an actual bare wall cut. Each placed
opening retains its library name for selection and schedules. All preset sizes
remain editable design defaults.

**Overhead tilt-up** garage doors use a rigid panel hinging along its top edge.
**Open** at 0% is closed, at 50% is halfway through a 90-degree lift, and at 100%
the panel lies horizontally at the header. Select the wall-normal side in
**Door operation**. The frame and host are checked against the requested panel
pose; an invalid choice stays in the editor with an explanation. This mechanism
requires a straight wall and an explicit door assembly. It is a tilt-up door,
not a sectional track simulation or continuous-motion certification. Plans
project its actual pose, including a fully raised panel, and 3D uses the same
solid. Save/history preserve the strict four-field operation version three:
`version`, `kind:overhead_tilt_up`, `side`, and `opening_fraction`.
Schedules display top hinge and open percentage. Compilation and runtime
qualification of these additions remain open.

The **Double** catalog door creates two physical half-width leaves and two
analytic swings. The first leaf's jamb follows `hinge`; the second uses the
opposite jamb. Pivots clear the selected wall and frame faces. Admission checks
the posed full leaf envelopes against the actual cut host, frame and other
leaf, and rejects an angle that crosses the meeting plane or creates a clash.
This applies to straight and fitted curved hosts. It checks the requested
pose; it is not a continuous motion or mechanical hardware certification.

**Sliding glass** creates a movable half-panel and a fixed half-panel on
separate tracks, with real glazing apertures. `hinge` identifies the movable
panel's starting jamb; `side` selects its wall-normal track side. Fraction zero
is closed; one stacks the movable panel behind the fixed panel. The editor's
**Open** percentage controls this travel. Frame depth must accommodate both
panel thicknesses and their clearance. Curved sliding assemblies are rejected
before a history entry. Library placement, dimension edits, undo/redo,
reflection, save/reopen, plans and 3D retain the stored mechanism and travel.
The Jamb selector's **None** option is available only for ordinary hinged
doors. Double and sliding doors always keep an operation; a slider closes with
Open at 0%, without losing its panel family.
Doors without swing data retain visible jamb and threshold linework in the
plan and support the same straight-wall width handles; editing them does not
create hinge or swing metadata.

Plan output uses a straight leaf and an analytic circular arc, sharing geometry
between the canvas and printed/exported plan scene. On curved walls, the closed
leaf follows the chord between jambs. Schedules expose read-only hinge, side, and
angle with source provenance. Double doors also expose their mechanism;
sliders expose mechanism and open percentage without a fictitious swing-angle
cell. Ordinary dimension edits preserve operation data;
conversion to a window retains it as dormant metadata and suppresses its symbol
and schedule cells. Undo or conversion back to a door restores its use.

Newly authored doors and windows also receive an optional, versioned
`opening_assembly` profile. The profile is type-driven (`door` or `window`) and
stores frame width/depth, panel thickness, glazing thickness, and signed inset
in metres. The host wall cut remains authoritative; the native 3D view adds a
derived frame, leaf or sash, and glazing compound inside that cut. A door leaf
and its optional glazing follow the persisted handed swing. Window profiles
produce actual framed glazing panes. Fixed single, double and triple layouts
have one, two or three panes; split layouts include physical mullions. Casements
rotate a glazed sash about its selected jamb. Sliding windows have one fixed
and one movable framed sash on separate tracks; Open ranges from 0% to 100%.
Their layout, handing, angle and travel remain editable in **Opening assembly**.
Double-click the opening to reveal this command in 2D or 3D quick properties.
An open sash remains selectable through its wall aperture without adding a
false closed-pane line to the drawing or exported plan.
Casement angle zero closes the sash. Both moving mechanisms require a straight
host and validate the selected pose against the cut host, frame, sill and other
sash. Continuous motion and clearances to unrelated objects are not certified.
Curved fixed windows use concentric
annular frame, sash and pane solids with signed wall-normal inset. Curved doors
use radial frame parts and a fitted planar chord leaf. Its finite thickness and
chord sagitta must fit the frame head; insufficient depth rejects with a fit
error rather than changing the specified frame dimensions. The pivot uses the
fitted leaf endpoint. Door glazing replaces an aperture in the leaf, avoiding
overlapping solid material. Handing rotates the leaf and pane together;
Legacy single-hinged poses retain their established geometry; collision-free
clearance at arbitrary angles is not certified for those poses. Newly authored
double doors use the stricter pose admission described above.
Legacy openings without the
profile remain valid and continue to render as wall cuts, which keeps import
lossless while a user upgrades selected instances.

Schedules expose the profile kind and dimensions as read-only source-backed
properties. Canonical profiles retain strict schema version 1. Version 2 adds
the window layout and its exact movement fields; unknown versions, missing
fields, kind mismatches, impossible panel/depth relationships, and assemblies
that do not fit the host wall are rejected before a Document mutation.
The opening inspector's **Opening assembly…** command edits all five profile
dimensions (frame width/depth, panel or sash depth, glazing depth, and signed
wall-centreline inset) in the active unit system. The command previews the
complete host and sibling openings, applies one revision-checked history entry,
and supports undo/redo. A door may keep zero glazing; a window requires a
positive glazing depth. Malformed or stale edits remain visible in the dialog
and leave the document unchanged.

Architecture-enabled IFC export now derives wall cuts and manufactured door/window
parts from the same native solid kernel. Fill occurrences use IFC door/window
products linked to their opening void with IfcRelFillsElement. Curved wall,
void and fill solids use bounded triangulated geometry at a 1 mm mesh deviation.
Native re-import requires matching regenerated geometry, identity representation
context and valid host/fill relationships; metadata alone cannot activate a profile.
Door/window fills have explicit opening-local placements: local X follows the
jamb chord, Z is up, and local Y agrees with the physical swing side. Bounded
proper Z-up parent placements compose before geometry comparison. Curved IFC
OverallWidth is the opening body's local-X envelope; native width remains the
host-arc station distance. A closed leaf with no specified operation carries a
user-defined description instead of invented handing.
This is a bounded IFC4 subset, not Reference View certification.
Double doors use `DOUBLE_DOOR_SINGLE_SWING`. The slider uses `USERDEFINED`
with "Two-track sliding door; one fixed panel", because IFC4's
`DOUBLE_DOOR_SLIDING` describes two movable panels. See the
[official IFC4 operation definitions](https://standards.buildingsmart.org/IFC/RELEASE/IFC4/ADD2_TC1/HTML/schema/ifcsharedbldgelements/lexical/ifcdoortypeoperationenum.htm).

DXF exports exact 2D wall footprints and manufactured mid-height sections of
door/window frames, panels and glazing, with analytic door swing geometry.
Registered VERTEX_ENTITY_V1 block metadata preserves editable hosted identities,
dimensions, assembly profiles and handing only when the complete host graph and
visible primitives agree. Unsupported edits retain visual geometry and source
bytes with diagnostics. Manufactured blocks carry a depiction-version marker
and activate only when regenerated physical primitives agree. Full ARCH-MOD-002
export acceptance and external CAD compatibility remain open.

In an uncropped conventional plan, selecting a door, window or bare opening on
a straight or circular wall shows two width handles at its jambs and a **W × H**
readout (**Arc W × H** on circular hosts). Drag either handle to resize along
the wall while pinning the opposite jamb. Curved width is measured along the
host arc, rather than between the jambs in a straight line. Window rails follow
concentric arcs; door swing geometry follows the actual trimmed manufactured leaf.
Height, sill, wall thickness, handing and manufactured frame dimensions remain
unchanged. The preview regenerates the manufactured section, swing and wall cut
together from immutable source geometry. Immediate jamb feedback is neutral while
exact geometry runs in a coalesced background queue; stale results are discarded.
Overlaps, an out-of-host span or insufficient clear frame width appear as an
invalid proposal; release leaves the project unchanged. Final admission also
regenerates the host, all sibling assemblies and affected wall joins.

The drag commits one undoable revision-fenced command. Escape, focus loss or a
refreshed document cancels it. Save/reopen retains width and offset. Print and
export use committed geometry and exclude handles and drag proposals. Cropped
plans and alternate projection frames retain dimension editing through
properties; their plan-width handles are not available.

The **Bay** Window style creates a projecting fixed assembly on a straight wall.
Set Projection and Projection side before placing it. Projection is measured
beyond that wall face; the live preview shows the selected side. Double-click
the window and open **Opening assembly** to edit projection, front width as a
percentage of the full wall-opening width, or side. Its three framed panes,
sealed plates and mounting shoulders derive from the common physical model.
There is no separate operable sash or swing. Width handles resize the mouth
while preserving projection and the front-width ratio. Rotation or reflection
of its host carries the bay with it. Save/reopen preserves the complete profile.

Bay schedules report three panes, depth, front ratio and side. IFC uses
`USERDEFINED` with `BAY_WINDOW` for its non-coplanar partition; native import
also requires actual mesh correspondence. DXF uses the manufactured mid-height
section and requires matching primitives before activating an editable host
graph. A bay window does not automatically extend a room, floor or appraisal
measurement area. Curved fitting and roof-hosted skylights remain open work.
